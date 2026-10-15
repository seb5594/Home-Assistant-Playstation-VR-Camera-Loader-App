"""Microphone ADC setup, PulseAudio registration and the capture supervisor."""
import errno
import subprocess
import unittest
from unittest.mock import patch

import helpers  # noqa: F401
import ak5703
import capture_service
import pulse_source
import stream_modes


class FakeMemory:
    """ModelMemory stand-in that records what the capture service reports."""

    def __init__(self, record=None):
        self.stored = record
        self.confirmed, self.failed = [], []

    def record(self, port):
        return self.stored

    def confirm(self, port):
        self.confirmed.append(port)

    def fail(self, port):
        self.failed.append(port)


class Ak5703Tests(unittest.TestCase):
    class Device:
        def __init__(self, stall_at=None, stall_twice=False):
            self.writes = []
            self.stall_at = stall_at
            self.stall_twice = stall_twice
            self.halts = 0

        def ctrl_transfer(self, request_type, request, value, index, payload, timeout=1000):
            register = payload[8] | (payload[9] << 8)
            if register == self.stall_at and (self.stall_twice or not self.halts):
                raise OSError(errno.EPIPE, 'stall')
            self.writes.append((register, payload[10]))
            return len(payload)

        def clear_halt(self, endpoint):
            self.halts += 1

    def test_gain_reaches_all_four_capsules(self):
        device = self.Device()
        ak5703.configure(device, 80)
        volumes = {register: value for register, value in device.writes if register in ak5703.VOLUME_REGISTERS}
        self.assertEqual(volumes, {0x07: 0xE5, 0x08: 0xE5, 0x17: 0xE5, 0x18: 0xE5})
        self.assertEqual(len(device.writes), len(ak5703.INIT_SEQUENCE))

    def test_gain_is_capped_at_documented_maximum(self):
        self.assertEqual(ak5703.volume_register_value(100), 0xF1)
        self.assertEqual(ak5703.volume_register_value(0), 0x80)
        self.assertAlmostEqual(ak5703.volume_db(0xF1), 36.0)

    def test_single_stall_is_recovered(self):
        device = self.Device(stall_at=0x0020)
        result = ak5703.configure(device, 50)
        self.assertEqual((result['usb_stalls'], device.halts), (1, 1))
        self.assertIn((0x0020, 0xA9), device.writes)

    def test_repeated_stall_names_the_register(self):
        with self.assertRaisesRegex(RuntimeError, '0x0020'):
            ak5703.configure(self.Device(stall_at=0x0020, stall_twice=True), 50)


class FakePulse:
    """Minimal pactl stand-in that tracks loaded modules."""

    def __init__(self, modules=(), default_source='alsa_input.usb', sinks=('alsa_output.hdmi',),
                 switch_on_connect=False):
        self.modules = list(modules)       # (id, name, args)
        self.next_id = 100
        self.default_source = default_source
        self.sinks = list(sinks)
        self.default_sink = self.sinks[0] if self.sinks else ''
        self.switch_on_connect = switch_on_connect   # like module-switch-on-connect
        self.calls = []

    def run(self, argv, capture_output, text, timeout):
        command, args = argv[1], argv[2:]
        self.calls.append((command, *args))
        out = ''
        if command == 'load-module':
            self.next_id += 1
            self.modules.append((str(self.next_id), args[0], ' '.join(args[1:])))
            if args[0] == 'module-null-sink':
                self.sinks.append(next(a.split('=')[1] for a in args if a.startswith('sink_name=')))
                if self.switch_on_connect:
                    self.default_sink = self.sinks[-1]
            out = str(self.next_id)
        elif command == 'unload-module':
            self.modules = [m for m in self.modules if m[0] != args[0]]
        elif command == 'list' and args[1] == 'modules':
            out = '\n'.join('\t'.join(m) for m in self.modules)
        elif command == 'list' and args[1] == 'sources':
            names = [f'{s}.monitor' for s in self.sinks]
            names += [a.split('=')[1] for m in self.modules for a in m[2].split() if a.startswith('source_name=')]
            out = '\n'.join(f'{i}\t{name}\tmodule\ts16le\tIDLE' for i, name in enumerate(names))
        elif command == 'list' and args[1] == 'sinks':
            out = '\n'.join(f'{i}\t{name}\tmodule' for i, name in enumerate(self.sinks))
        elif command == 'get-default-source':
            out = self.default_source
        elif command == 'set-default-source':
            self.default_source = args[0]
        elif command == 'get-default-sink':
            out = self.default_sink
        elif command == 'set-default-sink':
            self.default_sink = args[0]
        return subprocess.CompletedProcess(argv, 0, out + '\n', '')


class PulseSourceTests(unittest.TestCase):
    def test_register_creates_16khz_transport_and_named_input(self):
        pulse = FakePulse()
        with patch.object(pulse_source.subprocess, 'run', pulse.run), \
                patch.object(pulse_source, 'reload_audio'):
            microphone = pulse_source.PulseMicrophone()
            microphone.register()
            sink_args = next(m[2] for m in pulse.modules if m[1] == 'module-null-sink')
            self.assertIn('rate=16000', sink_args)
            self.assertIn('channels=1', sink_args)
            self.assertEqual(microphone.source, pulse_source.SOURCE)
            microphone.unregister()
        self.assertEqual(pulse.modules, [])

    def test_stale_and_legacy_modules_are_removed_but_others_kept(self):
        pulse = FakePulse(modules=[
            ('1', 'module-null-sink', f'sink_name={pulse_source.SINK} rate=48000 channels=1'),
            ('2', 'module-remap-source', 'master=playstation_camera_microphone.monitor source_name=playstation_camera_input'),
            ('3', 'module-null-sink', 'sink_name=playstation_camera_microphone'),
            ('4', 'module-alsa-card', 'device_id=0'),
        ])
        with patch.object(pulse_source.subprocess, 'run', pulse.run):
            removed = pulse_source.PulseMicrophone().remove_stale()
        self.assertEqual(removed, 3)
        self.assertEqual([m[0] for m in pulse.modules], ['4'])

    def test_default_input_is_restored(self):
        pulse = FakePulse(default_source='alsa_input.usb')
        with patch.object(pulse_source.subprocess, 'run', pulse.run), \
                patch.object(pulse_source, 'reload_audio'):
            microphone = pulse_source.PulseMicrophone(set_default_input=True)
            microphone.register()
            self.assertEqual(pulse.default_source, pulse_source.SOURCE)
            microphone.unregister()
        self.assertEqual(pulse.default_source, 'alsa_input.usb')

    def test_automatic_switch_to_the_transport_is_undone(self):
        pulse = FakePulse(sinks=('alsa_output.hdmi',), switch_on_connect=True)
        with patch.object(pulse_source.subprocess, 'run', pulse.run), \
                patch.object(pulse_source, 'reload_audio'):
            microphone = pulse_source.PulseMicrophone()
            microphone.register()
            self.assertEqual(pulse.default_sink, 'alsa_output.hdmi')
            microphone.unregister()

    def test_missing_default_speaker_does_not_break_registration(self):
        # The user's host: default output is an HDMI sink that is gone, no other speaker exists.
        pulse = FakePulse(sinks=(), switch_on_connect=True)
        pulse.default_sink = 'alsa_output.pci-0000_04_00.1.hdmi-stereo'
        with patch.object(pulse_source.subprocess, 'run', pulse.run), \
                patch.object(pulse_source, 'reload_audio') as reload:
            microphone = pulse_source.PulseMicrophone()
            microphone.register()
            self.assertTrue(reload.called, 'Home Assistant must be told about the new input')
            self.assertEqual(pulse.default_sink, pulse_source.SILENCE_SINK)
            self.assertIn(pulse_source.SOURCE, [a.split('=')[1] for m in pulse.modules for a in m[2].split()
                                                if a.startswith('source_name=')])
            with self.assertRaises(pulse_source.EchoReferenceError) as caught:
                pulse_source.resolve_echo_reference('default')
            self.assertEqual(caught.exception.code, 'no_speaker')
            microphone.unregister()
        self.assertEqual(pulse.modules, [])

    def test_failed_default_switch_is_not_fatal(self):
        pulse = FakePulse(sinks=('alsa_output.hdmi',), switch_on_connect=True)
        original = pulse.run

        def refuse_default(argv, **kwargs):
            if argv[1] == 'set-default-sink':
                return subprocess.CompletedProcess(argv, 1, '', 'Failure: No such entity')
            return original(argv, **kwargs)

        with patch.object(pulse_source.subprocess, 'run', refuse_default), \
                patch.object(pulse_source, 'reload_audio') as reload:
            microphone = pulse_source.PulseMicrophone()
            microphone.register()
        self.assertTrue(microphone.ready and reload.called)

    def test_default_echo_reference_skips_the_transport(self):
        pulse = FakePulse(sinks=(pulse_source.SINK, 'alsa_output.usb_speaker'))
        with patch.object(pulse_source.subprocess, 'run', pulse.run):
            self.assertEqual(pulse_source.resolve_echo_reference('default'), 'alsa_output.usb_speaker')

    def test_echo_reference_errors_carry_codes_for_the_page(self):
        pulse = FakePulse(sinks=(pulse_source.SINK,))
        with patch.object(pulse_source.subprocess, 'run', pulse.run):
            for setting, code in (('default', 'no_speaker'), (pulse_source.SINK, 'transport'),
                                  ('alsa_output.missing', 'not_found')):
                with self.assertRaises(pulse_source.EchoReferenceError) as caught:
                    pulse_source.resolve_echo_reference(setting)
                self.assertEqual(caught.exception.code, code)

    def test_echo_reference_resolution(self):
        pulse = FakePulse(sinks=('alsa_output.hdmi', pulse_source.SINK))
        with patch.object(pulse_source.subprocess, 'run', pulse.run):
            self.assertIsNone(pulse_source.resolve_echo_reference(''))
            self.assertEqual(pulse_source.resolve_echo_reference('default'), 'alsa_output.hdmi')
            self.assertEqual(pulse_source.resolve_echo_reference('alsa_output.hdmi'), 'alsa_output.hdmi')
            with self.assertRaisesRegex(RuntimeError, 'not a speaker'):
                pulse_source.resolve_echo_reference(pulse_source.SINK)
            with self.assertRaisesRegex(RuntimeError, 'not found'):
                pulse_source.resolve_echo_reference('alsa_output.missing')

    def test_playback_matches_transport(self):
        command = pulse_source.playback_command()
        self.assertIn('--rate=16000', command)
        self.assertIn(f'--device={pulse_source.SINK}', command)


PS4_MODES = {(3448, 808): (8, 15, 30, 60), (1748, 408): (8, 15, 30, 60, 120), (898, 200): (30, 60, 120, 240),
             (1280, 800): (30, 60)}
PS5_MODES = {(5148, 1088): (30,), (3840, 1080): (30,), (2560, 800): (30, 60), (1920, 1080): (30, 60)}


class StreamModeTests(unittest.TestCase):
    def test_profiles_pick_raw_modes_at_their_highest_rate_for_audio(self):
        picks = [stream_modes.choose(PS4_MODES, profile, True, 10) for profile in stream_modes.PROFILES]
        self.assertEqual([(m.geometry, m.fps) for m in picks],
                         [('3448x808', 60), ('1748x408', 120), ('898x200', 240)])
        self.assertTrue(all(m.raw for m in picks))

    def test_video_only_streams_as_slowly_as_the_preview_allows(self):
        self.assertEqual(stream_modes.choose(PS4_MODES, 'High', False, 5).fps, 8)
        self.assertEqual(stream_modes.choose(PS4_MODES, 'High', False, 20).fps, 30)
        self.assertEqual(stream_modes.choose(PS4_MODES, 'Low', False, 15).fps, 30)

    def test_firmware_generation_comes_from_the_eye_size(self):
        self.assertEqual(stream_modes.generation(PS4_MODES), 'ps4')
        self.assertEqual(stream_modes.generation(PS5_MODES), 'ps5')
        self.assertIsNone(stream_modes.generation({(1920, 1080): (30,)}))
        self.assertFalse(stream_modes.has_microphones(PS5_MODES))
        self.assertEqual(stream_modes.choose(PS5_MODES, 'High', False, 10).geometry, '5148x1088')

    def test_unknown_modes_fall_back_to_the_ps4_table(self):
        self.assertEqual(stream_modes.choose({}, 'High', True, 10).geometry, '3448x808')


class CaptureServiceTests(unittest.TestCase):
    def test_microphone_command(self):
        settings = capture_service.parse_args(['--mode', 'microphone', '--beamforming', 'adaptive',
                                               '--noise-suppression', 'rnnoise', '--auto-gain'])
        stream = stream_modes.choose(PS4_MODES, settings.usb_profile, True, settings.preview_fps)
        command = capture_service.CaptureService(settings).engine_command('/dev/video2', stream, True, 3)
        self.assertEqual(command[:3], [capture_service.ENGINE, '--device', '/dev/video2'])
        for expected in (['--geometry', '3448x808'], ['--fps', '60'], ['--beamformer', 'adaptive'],
                         ['--noise-suppression', 'rnnoise'], ['--echo-reference-fd', '3']):
            self.assertTrue(any(command[i:i + 2] == expected for i in range(len(command))), expected)
        self.assertIn('--auto-gain', command)
        self.assertNotIn('--preview', command)

    def test_camera_only_command_has_no_audio_and_no_picture_options(self):
        settings = capture_service.parse_args(['--mode', 'camera', '--usb-profile', 'Medium',
                                               '--preview', 'sd', '--preview-fps', '5'])
        stream = stream_modes.choose(PS4_MODES, settings.usb_profile, False, settings.preview_fps)
        command = capture_service.CaptureService(settings).engine_command('/dev/video0', stream, False, None)
        self.assertNotIn('--audio', command)
        self.assertIn('1748x408', command)
        self.assertEqual(command[command.index('--fps') + 1], '8')
        self.assertEqual(command[command.index('--preview') + 1], 'sd')
        self.assertNotIn('--gamma', command)

    def test_a_working_stream_confirms_the_firmware(self):
        memory = FakeMemory({'state': 'trying', 'model': 'ps4'})
        service = capture_service.CaptureService(capture_service.parse_args(['--mode', 'camera']), memory)
        service.note_frames('3-2')
        self.assertEqual(memory.confirmed, ['3-2'])

    def test_blank_sessions_switch_the_firmware_in_auto_mode(self):
        memory = FakeMemory({'state': 'trying', 'model': 'ps4'})
        service = capture_service.CaptureService(capture_service.parse_args(['--mode', 'camera']), memory)
        with patch.object(capture_service.firmware_manager, 'available', return_value={'ps4': 1, 'ps5': 2}):
            hints = [service.blank_session('3-2') for _ in range(capture_service.BLANK_SESSIONS_BEFORE_SWITCH)]
        self.assertEqual(hints[:-1], [None] * (len(hints) - 1))
        self.assertIn('PS5 firmware next', hints[-1])
        self.assertEqual(memory.failed, ['3-2'])

    def test_a_forced_model_is_never_switched(self):
        memory = FakeMemory({'state': 'trying', 'model': 'ps4'})
        settings = capture_service.parse_args(['--mode', 'camera', '--camera-model', 'ps4'])
        service = capture_service.CaptureService(settings, memory)
        with patch.object(capture_service.firmware_manager, 'available', return_value={'ps4': 1, 'ps5': 2}):
            for _ in range(5):
                self.assertIsNone(service.blank_session('3-2'))
        self.assertEqual(memory.failed, [])

    def test_only_live_nodes_count_as_camera(self):
        status = {'initialized': [{'pid': '058a', 'bus': 3, 'ports': [2], 'video_nodes': ['/dev/video0']}]}
        with patch.object(capture_service, 'read_json', return_value=status), \
                patch.object(capture_service, 'node_matches', return_value=False):
            self.assertIsNone(capture_service.find_camera())
        with patch.object(capture_service, 'read_json', return_value=status), \
                patch.object(capture_service, 'node_matches', return_value=True):
            camera, node = capture_service.find_camera()
        self.assertEqual(node, '/dev/video0')


if __name__ == '__main__':
    unittest.main()
