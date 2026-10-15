"""Ingress API: compact status, request files, and disabled-video behaviour."""
from http.server import ThreadingHTTPServer
import importlib
import json
import os
import re
import threading
import time
import unittest
import urllib.error
import urllib.request
from unittest import mock

import helpers
import runtime


def start_server(**environment):
    with mock.patch.dict(os.environ, environment):
        import web
        web = importlib.reload(web)
    server = ThreadingHTTPServer(('127.0.0.1', 0), web.Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server, f'http://127.0.0.1:{server.server_address[1]}', web


def get(url, method='GET'):
    request = urllib.request.Request(url, method=method, data=b'' if method == 'POST' else None)
    with urllib.request.urlopen(request, timeout=5) as response:
        return response.status, response.read()


def post(url, body, content_type='application/json'):
    request = urllib.request.Request(url, method='POST', data=body, headers={'Content-Type': content_type})
    try:
        with urllib.request.urlopen(request, timeout=5) as response:
            return response.status, json.loads(response.read())
    except urllib.error.HTTPError as error:
        return error.code, json.loads(error.read())


class Server:
    """Context manager around start_server()."""

    def __init__(self, **environment):
        self.environment = environment

    def __enter__(self):
        self.server, base, _web = start_server(**self.environment)
        return base

    def __exit__(self, *_exc):
        self.server.shutdown()
        self.server.server_close()


class WebTests(unittest.TestCase):
    def setUp(self):
        for folder in (helpers.RUN_DIR, helpers.CONFIG_DIR):
            for path in folder.iterdir():
                path.unlink()

    def test_status_merges_camera_microphone_and_engine(self):
        runtime.write_json(runtime.CAMERA_STATUS, {'initialized': [{'pid': '058a', 'bus': 3, 'ports': [2],
                                                                    'video_nodes': ['/dev/video0']}],
                                                   'bootloaders': 0, 'camera_model': 'auto'})
        runtime.write_json(runtime.AUDIO_STATUS, {'state': 'streaming', 'source': 'sony_playstation_vr_camera_input'})
        runtime.write_json(runtime.ENGINE_STATUS, {'geometry': '3448x808', 'fps': 30.0, 'streaming': True,
                                                   'audio': {'layout': 'packed', 'output_dbfs': -30}})
        server, base, _ = start_server(OUTPUT_MODE='microphone')
        try:
            status = json.loads(get(base + '/api/status')[1])
        finally:
            server.shutdown()
            server.server_close()
        self.assertEqual(status['camera']['state'], 'ready')
        self.assertEqual(status['camera']['port'], '3-2')
        self.assertEqual(status['microphone']['state'], 'streaming')
        self.assertEqual(status['audio']['layout'], 'packed')
        self.assertFalse(status['preview_enabled'])

    def test_stale_engine_status_is_not_shown_as_live(self):
        runtime.write_json(runtime.ENGINE_STATUS, {'geometry': '3448x808', 'fps': 30.0, 'streaming': True, 'audio': {}})
        old = time.time() - 60
        os.utime(runtime.ENGINE_STATUS, (old, old))
        server, base, _ = start_server(OUTPUT_MODE='microphone')
        try:
            status = json.loads(get(base + '/api/status')[1])
        finally:
            server.shutdown()
            server.server_close()
        self.assertIsNone(status['audio'])

    def test_compare_request_and_result(self):
        server, base, _ = start_server(OUTPUT_MODE='microphone')
        try:
            self.assertEqual(get(base + '/api/compare', 'POST')[0], 202)
            self.assertTrue((helpers.RUN_DIR / 'compare.request').exists())
            self.assertFalse(json.loads(get(base + '/api/compare')[1])['ready'])
            (helpers.RUN_DIR / 'compare.request').unlink()       # psvr-capture picked it up
            runtime.COMPARE_RAW.write_bytes(b'RIFF')
            runtime.COMPARE_PROCESSED.write_bytes(b'RIFF')
            self.assertTrue(json.loads(get(base + '/api/compare')[1])['ready'])
            self.assertEqual(get(base + '/compare/processed.wav')[1], b'RIFF')
        finally:
            server.shutdown()
            server.server_close()

    def test_video_endpoints_respect_microphone_mode(self):
        server, base, _ = start_server(OUTPUT_MODE='microphone')
        try:
            with self.assertRaises(urllib.error.HTTPError) as caught:
                get(base + '/still.jpg')
            self.assertEqual(caught.exception.code, 403)
            self.assertFalse((helpers.RUN_DIR / 'preview.request').exists())
        finally:
            server.shutdown()
            server.server_close()

    def test_still_requests_preview_and_serves_fresh_jpeg(self):
        runtime.SNAPSHOT.write_bytes(b'\xff\xd8jpeg\xff\xd9')
        server, base, _ = start_server(OUTPUT_MODE='both')
        try:
            status, body = get(base + '/still.jpg')
        finally:
            server.shutdown()
            server.server_close()
        self.assertEqual((status, body), (200, b'\xff\xd8jpeg\xff\xd9'))
        self.assertTrue(runtime.preview_requested())

    def test_preview_is_always_part_of_camera_modes(self):
        for mode, expected in (('camera', True), ('both', True), ('microphone', False)):
            with Server(OUTPUT_MODE=mode) as base:
                self.assertEqual(json.loads(get(base + '/api/status')[1])['preview_enabled'], expected, mode)

    def test_missing_firmware_is_explained(self):
        runtime.write_json(runtime.CAMERA_STATUS, {'bootloaders': 1, 'initialized': [], 'firmware': {},
                                                   'error': {'code': 'firmware_missing', 'message': 'No firmware'}})
        with Server(OUTPUT_MODE='microphone') as base:
            camera = json.loads(get(base + '/api/status')[1])['camera']
        self.assertEqual((camera['state'], camera['error_code']), ('no_firmware', 'firmware_missing'))

    def test_picture_controls_are_clamped_saved_and_published(self):
        with Server(OUTPUT_MODE='both') as base:
            status, answer = post(base + '/api/video', json.dumps({'saturation': 999, 'gamma': 120,
                                                                   'white_balance': 0, 'unknown': 1}).encode())
            self.assertEqual(status, 200)
            self.assertEqual((answer['values']['saturation'], answer['values']['gamma']), (400, 120))
            live = runtime.read_json(runtime.RUN_DIR / 'video.json')
            self.assertIs(live['white_balance'], False)
            self.assertNotIn('unknown', live)
            loaded = json.loads(get(base + '/api/video')[1])
            self.assertEqual(loaded['values']['gamma'], 120)
            self.assertEqual(post(base + '/api/video/reset', b'{}')[1]['values']['gamma'], 100)
            self.assertEqual(post(base + '/api/video', b'[1, 2]')[0], 400)

    def test_picture_controls_need_a_camera_mode(self):
        with Server(OUTPUT_MODE='microphone') as base:
            with self.assertRaises(urllib.error.HTTPError) as caught:
                get(base + '/api/video', 'POST')
        self.assertEqual(caught.exception.code, 404)

    def test_firmware_upload(self):
        import firmware_manager
        image = firmware_manager.OV580_SIGNATURE + bytes(40000)
        with Server(OUTPUT_MODE='microphone') as base:
            status, answer = post(base + '/api/firmware?model=ps4', image, 'application/octet-stream')
            self.assertEqual((status, answer['file'], answer['known']), (200, 'firmware_ps4.bin', False))
            self.assertEqual(post(base + '/api/firmware', image, 'application/octet-stream')[1]['error'], 'model')
            self.assertEqual(post(base + '/api/firmware?model=ps5', b'x' * 40000,
                                  'application/octet-stream')[1]['error'], 'signature')
        self.assertTrue((helpers.CONFIG_DIR / 'firmware_ps4.bin').exists())

    def test_chunked_bodies_from_the_ingress_proxy_are_read(self):
        """Home Assistant's ingress forwards POST bodies chunked, without Content-Length."""
        import http.client
        import firmware_manager
        with Server(OUTPUT_MODE='both') as base:
            host, port = base.split('//')[1].split(':')
            for path, body, content_type in (
                    ('/api/video', b'{"saturation": 222}', 'application/json'),
                    ('/api/firmware?model=ps4', firmware_manager.OV580_SIGNATURE + bytes(40000),
                     'application/octet-stream')):
                connection = http.client.HTTPConnection(host, int(port), timeout=5)
                pieces = [body[i:i + 4096] for i in range(0, len(body), 4096)]
                connection.request('POST', path, body=iter(pieces), encode_chunked=True,
                                   headers={'Content-Type': content_type, 'Transfer-Encoding': 'chunked'})
                response = connection.getresponse()
                answer = json.loads(response.read())
                connection.close()
                self.assertEqual(response.status, 200, (path, answer))
            self.assertEqual(json.loads(get(base + '/api/video')[1])['values']['saturation'], 222)
        self.assertTrue((helpers.CONFIG_DIR / 'firmware_ps4.bin').exists())

    def test_chunked_decoder_limits(self):
        import io
        import web
        self.assertEqual(web.read_chunked(io.BytesIO(b'4\r\nWiki\r\n5;x=1\r\npedia\r\n0\r\n\r\n'), 100), b'Wikipedia')
        self.assertIsNone(web.read_chunked(io.BytesIO(b'ff\r\n' + b'a' * 255 + b'\r\n0\r\n\r\n'), 100))
        self.assertIsNone(web.read_chunked(io.BytesIO(b'zz\r\n'), 100))

    def test_mjpeg_stream_sends_complete_frames(self):
        runtime.SNAPSHOT.write_bytes(b'\xff\xd8frame\xff\xd9')
        with Server(OUTPUT_MODE='camera') as base:
            expected = b'--frame\r\nContent-Type: image/jpeg\r\nContent-Length: 9\r\n\r\n\xff\xd8frame\xff\xd9'
            with urllib.request.urlopen(base + '/preview.mjpg', timeout=5) as response:
                self.assertIn('multipart/x-mixed-replace', response.headers['Content-Type'])
                self.assertEqual(response.read(len(expected)), expected)

    def test_infinite_values_do_not_break_the_save(self):
        with Server(OUTPUT_MODE='camera') as base:
            status, answer = post(base + '/api/video', b'{"gamma": 1e999, "contrast": 120}')
        self.assertEqual((status, answer['values']['gamma'], answer['values']['contrast']), (200, 100, 120))

    def test_parallel_saves_are_all_answered(self):
        import concurrent.futures
        with Server(OUTPUT_MODE='camera') as base:
            with concurrent.futures.ThreadPoolExecutor(8) as pool:
                results = list(pool.map(lambda value: post(base + '/api/video', json.dumps({'brightness': value % 50}).encode())[0],
                                        range(80)))
        self.assertEqual(set(results), {200})

    def test_app_slug_is_asked_once(self):
        import web
        ask = mock.Mock(return_value='abcd1234_sony_playstation_vr_camera_loader')
        slug = web.AppSlug(ask=ask, hostname='ignored')
        self.assertEqual(slug.get(), 'abcd1234_sony_playstation_vr_camera_loader')
        self.assertEqual(slug.get(), 'abcd1234_sony_playstation_vr_camera_loader')
        ask.assert_called_once()

    def test_app_slug_falls_back_to_the_host_name(self):
        import supervisor_api
        import web
        ask = mock.Mock(side_effect=supervisor_api.SupervisorError('unreachable'))
        slug = web.AppSlug(ask=ask, hostname='abcd1234-sony-playstation-vr-camera-loader')
        self.assertEqual(slug.get(), 'abcd1234_sony_playstation_vr_camera_loader')
        slug.get()
        ask.assert_called_once()          # no Supervisor call on every status poll
        self.assertIsNone(web.AppSlug(ask=ask, hostname='').get())

    def test_status_names_the_app_for_the_configuration_link(self):
        with Server(OUTPUT_MODE='microphone') as base:
            status = json.loads(get(base + '/api/status')[1])
        self.assertIn('app_slug', status)

    def test_other_containers_are_refused(self):
        import web
        handler = web.Handler.__new__(web.Handler)
        handler.client_address = ('172.30.33.7', 50000)
        handler.send_error = lambda code: setattr(handler, 'refused', code)
        self.assertFalse(handler.peer_allowed())
        self.assertEqual(handler.refused, 403)
        handler.client_address = ('172.30.32.2', 50000)
        self.assertTrue(handler.peer_allowed())

    def test_every_page_text_exists_in_both_languages(self):
        page = (helpers.SOURCE / 'web_ui.html').read_text()
        english, german = page.split('  en: {')[1].split('  de: {')
        keys = lambda block: set(re.findall(r'(?:^|[\s,{])(\w+): "', block))
        self.assertEqual(keys(english), keys(german.split('\n};')[0]))

    def test_page_has_no_external_resources_or_tracking(self):
        page = (helpers.SOURCE / 'web_ui.html').read_text()
        self.assertFalse(re.search(r'(src|href)="(https?:)?//', page))
        self.assertNotIn('diagnostics.json', page)
        for key in re.findall(r'data-t="(\w+)"', page):
            self.assertIn(f'{key}:', page.split('de: {')[1], f'German text missing for {key}')


if __name__ == '__main__':
    unittest.main()
