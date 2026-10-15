"""Configuration, translations, run.sh and image stay consistent with each other."""
import json
import re
import shutil
import subprocess
import unittest

import helpers

ROOT = helpers.ROOT


def option_keys(text, section):
    """Dotted keys of the options/schema block in config.yaml (indent based)."""
    keys, stack = [], []
    block = text.split(f'\n{section}:\n', 1)[1]
    for line in block.splitlines():
        if line and not line.startswith(' '):
            break
        match = re.match(r'^( +)([a-z_]+):(.*)$', line)
        if not match:
            continue
        depth = len(match.group(1)) // 2 - 1
        stack[depth:] = [match.group(2)]
        if match.group(3).strip():
            keys.append('.'.join(stack))
    return keys


class ConfigTests(unittest.TestCase):
    config = (ROOT / 'config.yaml').read_text()

    def test_every_option_has_a_schema_entry(self):
        self.assertEqual(sorted(option_keys(self.config, 'options')), sorted(option_keys(self.config, 'schema')))

    def test_nested_options_added_after_alpha10_are_optional(self):
        # The Supervisor does not merge new keys into a saved nested group.
        schema = dict(line.strip().split(': ', 1) for line in self.config.split('\nschema:\n')[1].splitlines()
                      if ': ' in line and not line.strip().startswith('#'))
        for key in ('usb_profile', 'beamforming', 'auto_gain', 'echo_reference'):
            self.assertTrue(schema[key].endswith('?'), key)

    def test_removed_options_are_gone(self):
        for key in ('show_video_preview', 'video_gamma', 'video_contrast', 'firmware_file', 'output_mode'):
            self.assertNotIn(f'{key}:', self.config)
            for language in ('de', 'en'):
                self.assertNotIn(f' {key}:', (ROOT / 'translations' / f'{language}.yaml').read_text())

    @unittest.skipUnless(shutil.which('jq'), 'jq not installed')
    def test_old_configuration_is_migrated(self):
        script = (ROOT / 'run.sh').read_text()
        program = re.search(r"<<'JQ' \|\| true\n(.*?)\nJQ\n", script, re.S).group(1)
        old = {'output_mode': 'Beides', 'mode': 'Microphone', 'camera_model': 'ps4', 'firmware_file': 'firmware_ps4.bin',
               'mqtt': {'host': 'broker'}, 'scan_interval': 5,
               'microphone': {'gain': 75, 'usb_mode': '1280x800', 'fps': 30},
               'camera': {'show_video_preview': True, 'video_gamma': 190, 'video_contrast': 75, 'video_fps': 15}}
        result = subprocess.run(['jq', '-c', program], input=json.dumps(old), capture_output=True, text=True,
                                check=True)
        self.assertEqual(json.loads(result.stdout), {
            'mode': 'Both', 'camera_model': 'auto', 'microphone': {'gain': 75},
            'camera': {'video_fps': 15, 'mqtt': {'host': 'broker'}}})

    def test_translations_cover_every_option(self):
        for language in ('de', 'en'):
            text = (ROOT / 'translations' / f'{language}.yaml').read_text()
            for key in option_keys(self.config, 'schema'):
                self.assertIn(f' {key.split(".")[-1]}:', text, f'{language}: {key}')

    def test_run_script_reads_only_known_options(self):
        script = (ROOT / 'run.sh').read_text()
        known = set(option_keys(self.config, 'schema'))
        for key in re.findall(r'\$\(option "?([a-z_.]+)', script):
            if key.startswith(('mqtt.', 'camera.mqtt.')) or '${' in key:
                continue
            self.assertIn(key, known)

    def test_version_matches_image_label(self):
        version = re.search(r'^version: "([^"]+)"', self.config, re.M).group(1)
        self.assertIn(f'BUILD_VERSION={version}', (ROOT / 'Dockerfile').read_text())

    def test_apparmor_profile_is_named_after_the_slug(self):
        slug = re.search(r'^slug: (\S+)', self.config, re.M).group(1)
        self.assertRegex((ROOT / 'apparmor.txt').read_text(), rf'profile {slug} ')

    def test_picture_controls_match_the_engine(self):
        import video_tuning
        engine = (helpers.ENGINE / 'preview.c').read_text()
        defaults_block = engine.split('void picture_tuning_defaults')[1].split('}')[0]
        defaults = {name: int(value) for name, value in re.findall(r't->(\w+) = (-?\d+);', defaults_block)}
        ranges = {name: (int(low), int(high))
                  for name, low, high in re.findall(r't->(\w+) = clamp_int\(t->\w+, (-?\d+), (\d+)\)', engine)}
        for name, (default, low, high) in video_tuning.CONTROLS.items():
            self.assertEqual(defaults[name], default, name)
            if name not in video_tuning.SWITCHES:
                self.assertEqual(ranges[name], (low, high), name)

    def test_build_context_keeps_every_engine_source(self):
        """beta1 shipped a .dockerignore that dropped a test header and broke the image build."""
        def pattern_regex(pattern):
            out, i = '', 0
            pattern = pattern.rstrip('/')
            while i < len(pattern):
                if pattern.startswith('**/', i):
                    out, i = out + '(?:.*/)?', i + 3
                elif pattern.startswith('**', i):
                    out, i = out + '.*', i + 2
                else:
                    out += {'*': '[^/]*', '?': '[^/]'}.get(pattern[i], re.escape(pattern[i]))
                    i += 1
            return re.compile(out + r'(?:/.*)?\Z')   # a directory pattern covers its contents

        rules = []
        for line in (ROOT / '.dockerignore').read_text().splitlines():
            line = line.strip()
            if line and not line.startswith('#'):
                rules.append((line.startswith('!'), pattern_regex(line.lstrip('!'))))
        for path in sorted((ROOT / 'src' / 'c').rglob('*')):
            if path.suffix not in ('.c', '.h') and path.name != 'Makefile':
                continue
            relative = path.relative_to(ROOT).as_posix()
            excluded = False
            for keep, regex in rules:
                if regex.match(relative):
                    excluded = not keep
            self.assertFalse(excluded, f'{relative} would be missing from the image build')

    def test_no_kernel_module_privilege_needed(self):
        self.assertNotIn('kernel_modules', self.config)


if __name__ == '__main__':
    unittest.main()
