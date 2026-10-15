"""Picture settings: app configuration, page saves and the write-back to the configuration."""
import json
import re
import tempfile
import time
import unittest
from pathlib import Path
from unittest import mock

import helpers
import supervisor_api
import video_tuning


class PictureSettingsTests(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        root = Path(self.folder.name)
        self.store, self.live, self.options = root / 'video_tuning.json', root / 'video.json', root / 'options.json'

    def tearDown(self):
        self.folder.cleanup()

    def write_options(self, camera):
        self.options.write_text(json.dumps({'mode': 'Both', 'camera': camera}))

    def test_option_format_round_trip(self):
        values = {'brightness': -20, 'contrast': 120, 'gamma': 180, 'saturation': 250,
                  'auto_levels': 0, 'white_balance': 1, 'denoise': 2}
        options = video_tuning.to_options(values)
        self.assertEqual(options['picture_noise_filter'], 'strong')
        self.assertIs(options['picture_auto_levels'], False)
        self.assertEqual(video_tuning.from_options(options), values)

    def test_configuration_wins_at_start_and_fills_gaps_from_the_last_save(self):
        self.store.write_text(json.dumps({'saturation': 300, 'gamma': 140}))
        self.write_options({'picture_saturation': 200, 'picture_noise_filter': 'off'})
        values = video_tuning.startup(self.store, self.live, self.options)
        self.assertEqual((values['saturation'], values['gamma'], values['denoise']), (200, 140, 0))
        self.assertEqual(json.loads(self.live.read_text())['saturation'], 200)
        self.assertEqual(json.loads(self.store.read_text())['saturation'], 200)   # page starts from it

    def test_page_saves_change_only_what_was_moved(self):
        self.write_options({'picture_saturation': 200, 'picture_contrast': 90})
        video_tuning.startup(self.store, self.live, self.options)
        with mock.patch.object(video_tuning, 'OPTIONS', self.options):
            values = video_tuning.save({'brightness': 30}, self.store, self.live)
            self.assertEqual((values['brightness'], values['saturation'], values['contrast']), (30, 200, 90))
            self.assertEqual(video_tuning.current(self.store, self.options)['brightness'], 30)

    def test_missing_keys_are_reported_for_older_configurations(self):
        self.write_options({'video_fps': 10})
        self.assertEqual(video_tuning.missing_in_configuration(self.options), set(video_tuning.CONTROLS))
        self.write_options(video_tuning.to_options(video_tuning.defaults()))
        self.assertEqual(video_tuning.missing_in_configuration(self.options), set())

    def test_option_schema_matches_the_controls(self):
        schema = (helpers.ROOT / 'config.yaml').read_text().split('\nschema:\n')[1]
        for name, (_default, low, high) in video_tuning.CONTROLS.items():
            option = video_tuning.OPTION_NAMES[name]
            entry = re.search(rf'^\s+{option}: (\S+)$', schema, re.M).group(1)
            self.assertTrue(entry.endswith('?'), f'{option} must be optional for older configurations')
            if name == 'denoise':
                self.assertEqual(entry, 'list(' + '|'.join(video_tuning.NOISE_FILTER) + ')?')
            elif name in video_tuning.SWITCHES:
                self.assertEqual(entry, 'bool?')
            else:
                self.assertEqual(entry, f'int({low},{high})?')


class ConfigurationWriteBackTests(unittest.TestCase):
    def test_camera_group_is_merged_not_replaced(self):
        posted = []

        def fake_call(method, path, body=None, timeout=3):
            if method == 'GET':
                return {'options': {'mode': 'Both', 'camera': {'video_fps': 15, 'mqtt': {'host': 'x'},
                                                               'picture_gamma': 100}}}
            posted.append(body)
            return {}

        with mock.patch.object(supervisor_api, 'call', fake_call):
            self.assertTrue(supervisor_api.update_camera_options({'picture_gamma': 150}))
            self.assertFalse(supervisor_api.update_camera_options({'picture_gamma': 100}))   # nothing new
        camera = posted[0]['options']['camera']
        self.assertEqual((camera['picture_gamma'], camera['video_fps'], camera['mqtt']), (150, 15, {'host': 'x'}))
        self.assertEqual(posted[0]['options']['mode'], 'Both')

    def test_slider_bursts_become_one_configuration_update(self):
        import web
        writes = []
        writer = web.ConfigurationWriter(delay=0.1, write=writes.append)
        for value in range(10):
            writer.schedule(dict(video_tuning.defaults(), brightness=value))
        self.assertEqual(writer.state, 'pending')
        time.sleep(0.3)
        self.assertEqual(len(writes), 1)
        self.assertEqual(writes[0]['picture_brightness'], 9)
        self.assertEqual(writer.state, 'saved')

    def test_failed_update_is_visible(self):
        import web

        def refuse(_changes):
            raise supervisor_api.SupervisorError('POST /addons/self/options: rejected')

        writer = web.ConfigurationWriter(delay=0.01, write=refuse)
        writer.schedule(video_tuning.defaults())
        time.sleep(0.1)
        self.assertEqual(writer.state, 'error')


if __name__ == '__main__':
    unittest.main()
