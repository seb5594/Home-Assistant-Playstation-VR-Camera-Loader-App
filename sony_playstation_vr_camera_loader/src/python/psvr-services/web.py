#!/usr/bin/env python3
"""Ingress page and the small HTTP API behind it.

Nothing here touches the camera: images, levels and recordings all come from
files written by psvr-capture. The page polls one compact status document
once per second, and only while it is visible.
"""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from io import BytesIO
import json
import os
from pathlib import Path
import select
import subprocess
import threading
import time
from urllib.parse import parse_qs, urlsplit
import wave
import zlib

import firmware_manager
from log_util import log
from runtime import (AUDIO_STATUS, CAMERA_STATUS, COMPARE_PROCESSED, COMPARE_RAW, ENGINE_STATUS, ROWS_DUMP,
                     RUN_DIR, SNAPSHOT, file_age, read_json, request, request_preview)
import supervisor_api
import video_tuning

PAGE = (Path(__file__).with_name('web_ui.html')).read_bytes()
OUTPUT_MODE = os.environ.get('OUTPUT_MODE', 'microphone')
PREVIEW_FPS = int(os.environ.get('VIDEO_FPS', '5'))
# The preview is part of the page whenever the camera picture is used at all.
VIDEO_ENABLED = OUTPUT_MODE != 'microphone'
STALE_ENGINE_SECONDS = 5
MAX_JSON_BODY = 4096
# Home Assistant ingress proxy, and go2rtc inside the app. Nobody else needs this API.
ALLOWED_PEERS = frozenset({'172.30.32.2', '127.0.0.1', '::1'})
TEST_SECONDS = 4
TEST_RATE = 16000


class ConfigurationWriter:
    """Copies picture settings from the app page into the app configuration.

    Sliders send many small saves; only the last state is written, a moment
    after the changes stop, so the Supervisor sees one update per adjustment.
    """

    def __init__(self, delay=2.0, write=supervisor_api.update_camera_options):
        self.delay = delay
        self.write = write
        self.lock = threading.Lock()
        self.pending = None
        self.timer = None
        self.state = 'saved'     # saved | pending | error

    def schedule(self, values):
        with self.lock:
            self.pending = dict(values)
            self.state = 'pending'
            if self.timer is not None:
                self.timer.cancel()
            self.timer = threading.Timer(self.delay, self.flush)
            self.timer.daemon = True
            self.timer.start()

    def flush(self):
        with self.lock:
            values, self.pending, self.timer = self.pending, None, None
        if values is None:
            return
        try:
            self.write(video_tuning.to_options(values))
            self.state = 'saved'
        except supervisor_api.SupervisorError as exc:
            self.state = 'error'
            log('WARNING', f'Picture settings not written to the app configuration: {exc}')


CONFIGURATION = ConfigurationWriter()


class AppSlug:
    """This installation's slug, for the link to its configuration page.

    Asked from the Supervisor once. If that fails, the container's host name
    stands in: it is the slug with dashes (abcd1234-sony-playstation-vr-camera-loader).
    """

    def __init__(self, ask=supervisor_api.app_slug, hostname=None):
        self.ask = ask
        self.hostname = hostname if hostname is not None else os.environ.get('HOSTNAME', '')
        self.value = None
        self.lock = threading.Lock()

    def get(self):
        with self.lock:              # the page and main() may ask at the same time
            if self.value is None:
                try:
                    self.value = self.ask()
                except supervisor_api.SupervisorError as exc:
                    log('DEBUG', f'App slug taken from the host name ({exc})')
                    self.value = self.hostname.replace('-', '_')
            return self.value or None


APP_SLUG = AppSlug()


def camera_summary(status, microphone):
    initialized = status.get('initialized') or []
    error = status.get('error') or {}
    firmware = {model: image.summary() for model, image in firmware_manager.available().items()}
    summary = {'setting': status.get('camera_model', 'auto'), 'firmware': firmware,
               'error': error.get('message'), 'error_code': error.get('code')}
    if initialized:
        camera = initialized[0]
        summary.update(state='ready', pid=camera['pid'],
                       model=camera.get('model') or microphone.get('generation'),
                       port=f"{camera['bus']}-" + '.'.join(str(p) for p in camera['ports']))
    elif status.get('bootloaders'):
        summary['state'] = 'no_firmware' if error.get('code') == 'firmware_missing' else 'firmware'
    else:
        summary['state'] = 'missing'
    return summary


def api_status():
    engine = read_json(ENGINE_STATUS) if (file_age(ENGINE_STATUS) or 1e9) < STALE_ENGINE_SECONDS else None
    microphone = read_json(AUDIO_STATUS, {'state': 'waiting'})
    camera = camera_summary(read_json(CAMERA_STATUS, {}), microphone)
    if OUTPUT_MODE == 'camera':
        microphone = {'state': 'disabled', 'video_node': microphone.get('video_node'),
                      'capture_mode': microphone.get('capture_mode'), 'controls': microphone.get('controls')}
    return {
        'mode': OUTPUT_MODE,
        'app_slug': APP_SLUG.get(),
        'preview_enabled': VIDEO_ENABLED,
        'camera': camera,
        'microphone': microphone,
        'audio': engine.get('audio') if engine else None,
        'capture': {'geometry': engine['geometry'], 'fps': engine['fps'], 'streaming': engine['streaming']}
                   if engine else None,
        'preview': engine.get('preview') if engine else None,
    }


def fresh_snapshot(max_age):
    age = file_age(SNAPSHOT)
    if age is None or age > max_age:
        return None
    try:
        frame = SNAPSHOT.read_bytes()
    except OSError:
        return None
    return frame if frame.startswith(b'\xff\xd8') and frame.endswith(b'\xff\xd9') else None


def wait_for_snapshot(timeout, max_age):
    deadline = time.monotonic() + timeout
    while True:
        frame = fresh_snapshot(max_age)
        if frame is not None or time.monotonic() >= deadline:
            return frame
        time.sleep(0.05)


def record_from_pulse(source):
    """Four seconds straight from PulseAudio: exactly what Assist receives."""
    process = subprocess.Popen(['parec', '--raw', '--format=s16le', f'--rate={TEST_RATE}', '--channels=1',
                                '--latency-msec=60', f'--device={source}'],
                               stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    target = TEST_RATE * 2 * TEST_SECONDS
    payload = bytearray()
    deadline = time.monotonic() + TEST_SECONDS + 4
    try:
        while len(payload) < target and time.monotonic() < deadline:
            readable, _, _ = select.select([process.stdout], [], [], 0.5)
            if readable:
                chunk = os.read(process.stdout.fileno(), min(32768, target - len(payload)))
                if not chunk:
                    break
                payload.extend(chunk)
            elif process.poll() is not None:
                break
    finally:
        process.terminate()
        try:
            process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            process.kill()
        process.stdout.close()
    if len(payload) < target // 2:
        return None
    output = BytesIO()
    with wave.open(output, 'wb') as recording:
        recording.setnchannels(1)
        recording.setsampwidth(2)
        recording.setframerate(TEST_RATE)
        recording.writeframes(bytes(payload))
    return output.getvalue()


def read_chunked(stream, limit):
    """Decode a chunked request body; None if it is larger than `limit` or malformed."""
    body = bytearray()
    while True:
        line = stream.readline(1024)
        try:
            size = int(line.split(b';', 1)[0].strip(), 16)
        except ValueError:
            return None
        if size == 0:
            while stream.readline(1024).strip():    # optional trailer headers
                pass
            return bytes(body)
        if len(body) + size > limit:
            return None
        body += stream.read(size)
        stream.readline(16)                          # CRLF after the chunk


class Handler(BaseHTTPRequestHandler):
    server_version = 'psvr-camera'

    # ---- helpers ------------------------------------------------------------

    def send_body(self, body, content_type, status=200, extra=None):
        self.send_response(status)
        self.send_header('Content-Type', content_type)
        self.send_header('Cache-Control', 'no-store')
        self.send_header('Content-Length', str(len(body)))
        for key, value in (extra or {}).items():
            self.send_header(key, value)
        self.end_headers()
        self.wfile.write(body)

    def send_json(self, value, status=200):
        self.send_body(json.dumps(value).encode(), 'application/json', status)

    def send_file(self, path, content_type, download=None):
        try:
            body = Path(path).read_bytes()
        except OSError:
            self.send_error(404)
            return
        extra = {'Content-Disposition': f'attachment; filename="{download}"'} if download else None
        self.send_body(body, content_type, extra=extra)

    # ---- routes ---------------------------------------------------------------

    def do_GET(self):
        if not self.peer_allowed():
            return
        path = self.path.split('?', 1)[0]
        routes = {
            '/': lambda: self.send_body(PAGE, 'text/html; charset=utf-8'),
            '/index.html': lambda: self.send_body(PAGE, 'text/html; charset=utf-8'),
            '/api/status': lambda: self.send_json(api_status()),
            '/api/video': lambda: self.send_json({'values': video_tuning.current(), 'controls': video_tuning.CONTROLS,
                                                  'configuration': CONFIGURATION.state}),
            '/api/compare': lambda: self.send_json({'ready': self.result_ready('compare', COMPARE_PROCESSED)}),
            '/api/rows': lambda: self.send_json({'ready': self.result_ready('rows', ROWS_DUMP)}),
            '/compare/raw.wav': lambda: self.send_file(COMPARE_RAW, 'audio/wav'),
            '/compare/processed.wav': lambda: self.send_file(COMPARE_PROCESSED, 'audio/wav'),
            '/rows.bin': lambda: self.send_file(ROWS_DUMP, 'application/octet-stream', 'psvr-microphone-rows.bin'),
            '/still.jpg': self.still,
            '/preview.mjpg': self.mjpeg,
            '/microphone.wav': self.microphone_test,
        }
        handler = routes.get(path)
        if handler is None:
            self.send_error(404)
        else:
            try:
                handler()
            except (BrokenPipeError, ConnectionResetError):
                pass

    def read_body(self, limit):
        """Request body up to `limit` bytes, or None.

        Home Assistant's ingress proxy forwards bodies with chunked transfer
        encoding and no Content-Length, which BaseHTTPRequestHandler does not
        decode by itself.
        """
        if 'chunked' in self.headers.get('Transfer-Encoding', '').lower():
            return read_chunked(self.rfile, limit)
        try:
            length = int(self.headers.get('Content-Length', '0'))
        except ValueError:
            length = -1
        if 0 < length <= limit:
            return self.rfile.read(length)
        if limit < length <= 4 * limit:
            self.rfile.read(length)    # drain, so the browser receives the error instead of a reset
        return None

    def peer_allowed(self):
        if self.client_address[0] in ALLOWED_PEERS:
            return True
        self.send_error(403)
        return False

    def save_video(self):
        body = self.read_body(MAX_JSON_BODY)
        try:
            changes = json.loads(body) if body else None
        except ValueError:
            changes = None
        if not isinstance(changes, dict):
            self.send_json({'error': 'invalid'}, 400)
            return
        values = video_tuning.save(changes)
        CONFIGURATION.schedule(values)
        self.send_json({'values': values, 'configuration': CONFIGURATION.state})

    def upload_firmware(self):
        """Raw .bin body; ?model=ps4|ps5 is only needed for unknown versions."""
        query = parse_qs(urlsplit(self.path).query)
        data = self.read_body(firmware_manager.MAX_SIZE)
        if data is None:
            self.send_json({'error': 'size'}, 400)
            return
        try:
            image = firmware_manager.store(data, (query.get('model') or [None])[0])
        except firmware_manager.FirmwareError as exc:
            self.send_json({'error': exc.code, 'message': str(exc)}, 400)
            return
        except OSError as exc:
            self.send_json({'error': 'write', 'message': str(exc)}, 500)
            return
        self.send_json({'model': image.model, **image.summary()})

    def do_POST(self):
        if not self.peer_allowed():
            return
        path = self.path.split('?', 1)[0]
        if path == '/api/video' and VIDEO_ENABLED:
            self.save_video()
        elif path == '/api/video/reset' and VIDEO_ENABLED:
            values = video_tuning.reset()
            CONFIGURATION.schedule(values)
            self.send_json({'values': values, 'configuration': CONFIGURATION.state})
        elif path == '/api/firmware':
            self.upload_firmware()
        elif path == '/api/compare' and OUTPUT_MODE != 'camera':
            COMPARE_PROCESSED.unlink(missing_ok=True)
            COMPARE_RAW.unlink(missing_ok=True)
            request('compare')
            self.send_json({'started': True}, 202)
        elif path == '/api/rows':
            ROWS_DUMP.unlink(missing_ok=True)
            request('rows')
            self.send_json({'started': True}, 202)
        else:
            self.send_error(404)

    @staticmethod
    def result_ready(name, result):
        return result.exists() and not (RUN_DIR / f'{name}.request').exists()

    def still(self):
        if not VIDEO_ENABLED:
            self.send_error(403, 'Video is disabled in the app configuration')
            return
        request_preview()
        max_age = max(2.0, 2.0 / PREVIEW_FPS)
        # A paused stream needs a moment to start (camera-only mode).
        frame = wait_for_snapshot(timeout=3.0, max_age=max_age)
        if frame is None:
            self.send_error(503, 'No fresh camera image yet')
            return
        # Lets the page count real new frames for its fps display.
        self.send_body(frame, 'image/jpeg', extra={'X-Frame-Id': str(zlib.crc32(frame))})

    def mjpeg(self):
        """MJPEG for go2rtc and the MQTT camera; keeps the preview alive while read."""
        if not VIDEO_ENABLED:
            self.send_error(403, 'Video is disabled in the app configuration')
            return
        request_preview()
        if wait_for_snapshot(timeout=5.0, max_age=5.0) is None:
            self.send_error(503, 'No camera image yet')
            return
        self.send_response(200)
        self.send_header('Content-Type', 'multipart/x-mixed-replace; boundary=frame')
        self.send_header('Cache-Control', 'no-store')
        self.end_headers()
        last_sent = None
        last_request = 0.0
        idle_since = time.monotonic()
        while time.monotonic() - idle_since < 10:
            now = time.monotonic()
            if now - last_request > 2:
                request_preview()
                last_request = now
            try:
                modified = SNAPSHOT.stat().st_mtime_ns
            except OSError:
                modified = None
            if modified and modified != last_sent:
                frame = fresh_snapshot(max_age=5.0)
                if frame:
                    self.wfile.write(b'--frame\r\nContent-Type: image/jpeg\r\nContent-Length: ' +
                                     str(len(frame)).encode() + b'\r\n\r\n' + frame + b'\r\n')
                    self.wfile.flush()
                    last_sent = modified
                    idle_since = now
            time.sleep(0.02)

    def microphone_test(self):
        microphone = read_json(AUDIO_STATUS, {})
        if microphone.get('state') != 'streaming' or not microphone.get('source'):
            self.send_error(503, 'Microphone is not streaming')
            return
        body = record_from_pulse(microphone['source'])
        if body is None:
            self.send_error(502, 'PulseAudio delivered no audio')
            return
        self.send_body(body, 'audio/wav', extra={'Content-Disposition': 'attachment; filename="camera-pulseaudio.wav"'})

    # Only server-side failures reach the app log; the page polls every second.
    def log_request(self, code='-', size='-'):
        if isinstance(code, int) and code >= 500:
            super().log_request(code, size)

    def log_error(self, format, *args):
        if args and isinstance(args[0], int) and args[0] >= 500:
            super().log_error(format, *args)


def main():
    RUN_DIR.mkdir(parents=True, exist_ok=True)
    APP_SLUG.get()      # one Supervisor call now instead of during the first page load
    if VIDEO_ENABLED and video_tuning.missing_in_configuration():
        # Older installations: show the current picture settings in the Configuration tab too.
        CONFIGURATION.schedule(video_tuning.current())
    server = ThreadingHTTPServer(('0.0.0.0', 8099), Handler)
    server.daemon_threads = True
    server.serve_forever()


if __name__ == '__main__':
    main()
