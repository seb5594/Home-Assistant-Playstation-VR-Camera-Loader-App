#!/usr/bin/env python3
"""Keep psvr-capture running for the connected camera and wire up its outputs.

For every camera session:
  1. pick the stream from the modes the camera firmware offers
  2. register the PulseAudio microphone (audio modes, PS4 camera only)
  3. start the AK5703 microphone ADC over USB
  4. run  psvr-capture | pacat  (+ parec of the echo reference, if configured)
and tear everything down again when the camera disappears.
"""
import argparse
import signal
import subprocess
import time

import ak5703
from camera_models import TRYING, ModelMemory, other, port_key
import firmware_manager
from log_util import log as _log
from pulse_source import (EchoReferenceError, PulseMicrophone, playback_command, reference_command,
                          resolve_echo_reference)
from runtime import (AUDIO_STATUS, CAMERA_STATUS, ENGINE_STATUS, RUN_DIR, file_age, read_json, request_preview,
                     write_json)
import stream_modes
import usb_device
import video_tuning
from video_nodes import capture_modes, controls, node_matches

ENGINE = '/usr/local/bin/psvr-capture'
SILENT_DBFS = -95.0
RETRY_SECONDS = 10
BLANK_SESSIONS_BEFORE_SWITCH = 3   # engine restarts without a single frame


def log(level, message):
    _log(level, message, 'Capture')


def find_camera():
    """(camera, node) for the first initialized camera whose node is still live."""
    status = read_json(CAMERA_STATUS, {})
    for camera in status.get('initialized', []):
        for node in camera.get('video_nodes', []):
            if node_matches(node, camera['bus'], camera['ports'], int(camera['pid'], 16)):
                return camera, node
    return None


class CaptureService:
    def __init__(self, settings, memory=None):
        self.settings = settings
        self.audio_wanted = settings.mode in ('microphone', 'both')
        self.video = settings.preview != 'off' and settings.mode in ('camera', 'both')
        self.microphone = PulseMicrophone(settings.set_default_input) if self.audio_wanted else None
        self.memory = memory or ModelMemory()
        self.stopping = False
        self.last_error = None
        self.blank_sessions = 0
        self.blank_attempt = None      # which firmware attempt blank_sessions counts for
        self.status = {}

    # ---- status ---------------------------------------------------------

    def publish(self, state, message, **extra):
        self.status = {'state': state, 'message': message, 'mode': self.settings.mode,
                       'source': self.microphone.source if self.microphone and self.microphone.ready else None,
                       'default_input': bool(self.microphone and self.microphone.default_active),
                       'updated': time.time(), **extra}
        write_json(AUDIO_STATUS, self.status)

    def fail(self, message):
        if message != self.last_error:
            log('ERROR', message)
            self.last_error = message
        self.publish('error', message)

    # ---- one camera session ----------------------------------------------

    def engine_command(self, node, stream, audio, reference_fd):
        s = self.settings
        command = [ENGINE, '--device', node, '--geometry', stream.geometry, '--fps', str(stream.fps),
                   '--run-dir', str(RUN_DIR)]
        if audio:
            command += ['--audio', '--beamformer', s.beamforming,
                        '--noise-suppression', s.noise_suppression, '--noise-strength', str(s.noise_strength)]
            if s.auto_gain:
                command.append('--auto-gain')
            if reference_fd is not None:
                command += ['--echo-reference-fd', str(reference_fd)]
        if self.video:
            command += ['--preview', s.preview, '--preview-fps', str(s.preview_fps)]
        return command

    def start_reference(self):
        """parec of the echo reference sink: (process, sink, error_code, error_text)."""
        try:
            sink = resolve_echo_reference(self.settings.echo_reference)
        except EchoReferenceError as exc:
            log('WARNING', f'Echo cancellation off: {exc}')
            return None, None, exc.code, str(exc)
        except (OSError, RuntimeError, subprocess.TimeoutExpired) as exc:
            log('WARNING', f'Echo cancellation off: {exc}')
            return None, None, 'unavailable', str(exc)
        if sink is None:
            return None, None, None, None
        process = subprocess.Popen(reference_command(sink), stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        log('INFO', f'Echo cancellation listens to {sink}')
        return process, sink, None, None

    def session(self, camera, node):
        processes = []
        port = port_key(camera['bus'], camera['ports'])
        modes = capture_modes(node)
        generation = stream_modes.generation(modes)
        # Without a mode list (old kernel) the camera is assumed to be a PS4 one.
        audio = self.audio_wanted and (not modes or stream_modes.has_microphones(modes))
        stream = stream_modes.choose(modes, self.settings.usb_profile, audio, self.settings.preview_fps)
        extra = {'video_node': node, 'usb_port': usb_device.format_port(camera['bus'], camera['ports']),
                 'generation': generation, 'capture_mode': f'{stream.geometry}@{stream.fps}',
                 'controls': controls(node)}
        if self.audio_wanted and not audio:
            if self.microphone.ready:
                self.microphone.unregister()
            message = 'The PS5 HD camera has no microphones'
            if self.last_error != message:
                log('WARNING', f'{message}; only video is available.')
                self.last_error = message
            self.publish('no_microphone', message, **extra)
            if not self.video:
                # Nothing streams in this mode; the PS5 video modes are the best proof available.
                record = self.memory.record(port) or {}
                if record.get('state') == TRYING and record.get('model') == generation:
                    self.memory.confirm(port)
                self.wait_while_connected(camera, node)
                return
        ENGINE_STATUS.unlink(missing_ok=True)       # never judge this session by the last one
        try:
            if audio:
                if not self.microphone.ready:
                    self.microphone.register()
                self.publish('starting', 'Starting the microphone', **extra)
                device = usb_device.find(int(camera['pid'], 16), camera['bus'], camera['ports'])
                if device is None:
                    raise RuntimeError('Camera USB control interface is not accessible inside the app')
                with device:
                    extra['adc'] = ak5703.configure(device, self.settings.gain)

            reference, sink, error_code, error_text = (self.start_reference() if audio
                                                       else (None, None, None, None))
            if reference:
                processes.append(reference)
            extra.update(echo_reference=sink, echo_error=error_text, echo_error_code=error_code)
            reference_fd = reference.stdout.fileno() if reference else None
            command = self.engine_command(node, stream, audio, reference_fd)
            engine = subprocess.Popen(command, stdout=subprocess.PIPE if audio else subprocess.DEVNULL,
                                      pass_fds=(reference_fd,) if reference_fd is not None else ())
            processes.append(engine)
            if reference:
                reference.stdout.close()   # the engine owns the read end now
            player = None
            if audio:
                player = subprocess.Popen(playback_command(), stdin=engine.stdout)
                engine.stdout.close()
                processes.append(player)
            if not audio and (self.memory.record(port) or {}).get('state') == TRYING:
                request_preview()          # stream briefly so a new firmware gets confirmed
            if audio:
                self.publish('starting', 'Waiting for the first audio', **extra)
            elif not self.audio_wanted:
                self.publish('disabled', 'Microphone not used in camera mode', **extra)
            self.supervise(camera, node, port, engine, player, audio, extra)
        finally:
            for process in reversed(processes):
                if process.poll() is None:
                    process.terminate()
            for process in reversed(processes):
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()

    def wait_while_connected(self, camera, node):
        while not self.stopping and node_matches(node, camera['bus'], camera['ports'], int(camera['pid'], 16)):
            time.sleep(1)

    def note_frames(self, port):
        """First frames of a session: the firmware on this camera works."""
        self.memory.confirm(port)
        self.blank_sessions = 0

    def blank_session(self, port):
        """The engine stopped without a single frame; maybe it is the wrong firmware."""
        record = self.memory.record(port) or {}
        attempt = (port, record.get('model'), record.get('updated'))
        if attempt != self.blank_attempt:          # a new upload starts a new count
            self.blank_attempt, self.blank_sessions = attempt, 0
        self.blank_sessions += 1
        if (self.settings.camera_model != 'auto' or record.get('state') != TRYING or
                self.blank_sessions < BLANK_SESSIONS_BEFORE_SWITCH or
                len(firmware_manager.available()) < 2):
            return None
        self.memory.fail(port)
        self.blank_sessions = 0
        return (f'The camera delivers no picture with the {record["model"].upper()} firmware. Unplug it for a '
                f'moment; the app will load the {other(record["model"]).upper()} firmware next.')

    def supervise(self, camera, node, port, engine, player, audio, extra):
        started = time.monotonic()
        announced = False
        frames_seen = False
        while not self.stopping:
            time.sleep(1)
            age = file_age(ENGINE_STATUS)
            engine_status = read_json(ENGINE_STATUS, {}) if age is not None and age < 3 else {}
            if not frames_seen and engine_status.get('frames', 0) > 0:
                self.note_frames(port)
                frames_seen = True
            if engine.poll() is not None:
                hint = None if frames_seen else self.blank_session(port)
                raise RuntimeError(hint or f'psvr-capture stopped (exit code {engine.returncode})')
            if player is not None and player.poll() is not None:
                raise RuntimeError(f'PulseAudio playback stopped (exit code {player.returncode})')
            if not node_matches(node, camera['bus'], camera['ports'], int(camera['pid'], 16)):
                log('INFO', 'Camera disconnected')
                self.blank_sessions = 0
                return
            if not audio:
                continue
            levels = (engine_status.get('audio') or {}).get('channel_dbfs') or []
            if levels and max(levels) <= SILENT_DBFS and time.monotonic() - started > 3:
                self.publish('silent', 'The camera sends no microphone data', **extra)
            elif levels:
                self.publish('streaming', 'Microphone active', **extra)
                if not announced:
                    log('INFO', f'Microphone streaming into {self.microphone.source}')
                    announced = True
                    self.last_error = None

    # ---- main loop -------------------------------------------------------

    def run(self):
        signal.signal(signal.SIGTERM, self.stop)
        signal.signal(signal.SIGINT, self.stop)
        if self.video:
            video_tuning.startup()
        self.publish('waiting', 'Waiting for the camera')
        try:
            if self.microphone:
                self.microphone.remove_stale()
            while not self.stopping:
                found = find_camera()
                if found is None:
                    if self.microphone and self.microphone.ready:
                        self.microphone.unregister()
                    if self.status.get('state') != 'waiting':
                        self.publish('waiting', 'Waiting for the camera')
                    time.sleep(1)
                    continue
                try:
                    self.session(*found)
                except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as exc:
                    self.fail(str(exc))
                    self.sleep(2 if find_camera() is None else RETRY_SECONDS)
        finally:
            if self.microphone:
                self.microphone.unregister()
            self.publish('stopped', 'Service stopped')

    def sleep(self, seconds):
        deadline = time.monotonic() + seconds
        while not self.stopping and time.monotonic() < deadline:
            time.sleep(0.2)

    def stop(self, *_args):
        self.stopping = True


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mode', choices=('microphone', 'camera', 'both'), default='microphone')
    parser.add_argument('--camera-model', choices=('auto', 'ps4', 'ps5'), default='auto')
    parser.add_argument('--usb-profile', choices=stream_modes.PROFILES, default='High')
    parser.add_argument('--gain', type=int, choices=range(101), default=80, metavar='0-100')
    parser.add_argument('--set-default-input', action='store_true')
    parser.add_argument('--beamforming', choices=('focus', 'adaptive', 'sum'), default='focus')
    parser.add_argument('--noise-suppression', choices=('off', 'rnnoise'), default='off')
    parser.add_argument('--noise-strength', type=int, choices=range(101), default=75, metavar='0-100')
    parser.add_argument('--auto-gain', action='store_true')
    parser.add_argument('--echo-reference', default='')
    parser.add_argument('--preview', choices=('off', 'native', 'hd', 'sd', 'low'), default='off')
    parser.add_argument('--preview-fps', type=int, default=10)
    return parser.parse_args(argv)


def main():
    CaptureService(parse_args()).run()


if __name__ == '__main__':
    main()