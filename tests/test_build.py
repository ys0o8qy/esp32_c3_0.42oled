import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('build', Path(__file__).resolve().parents[1] / 'scripts/build.py')
build = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build)

class BuildConfigTests(unittest.TestCase):
    def parse(self, text):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / '.env'
            path.write_text(text)
            return build.validate(build.read_env(path))

    def test_defaults_and_missing_file(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / 'missing'
            self.assertEqual(build.read_env(path), build.DEFAULTS)
            with self.assertRaises(ValueError): build.read_env(path, required=True)
        self.assertIn('#define C3_THINGSBOARD 0', build.header(self.parse('')))

    def test_literal_secrets_and_unicode(self):
        e = self.parse('WIFI_SSID="房间"\nWIFI_PASSWORD=\'$(echo x)#\\abc"\'\n')
        self.assertEqual(e['WIFI_PASSWORD'], '$(echo x)#\\abc"')
        self.assertNotIn('$(echo', build.header(e))
        self.assertEqual(build.literal('"\\\n'), '"\\042\\134\\012"')

    def test_thingsboard(self):
        e = self.parse('REPORT_MODE=thingsboard\nTHINGSBOARD_URL=https://tb.example:8443/\nTHINGSBOARD_ACCESS_TOKEN=abc')
        self.assertIn('#define C3_THINGSBOARD 1', build.header(e))
        for extra in ('REPORT_TOKEN=wrong', 'REPORT_URL=http://other.example'):
            with self.assertRaises(ValueError): build.validate(dict(e, **dict([extra.split('=',1)])))

    def test_invalid_input(self):
        for text in ('UNKNOWN=x', 'WIFI_SSID=a\nWIFI_SSID=b', 'WIFI_SSID="oops',
                     'WIFI_SSID='+'房'*11, 'WIFI_PASSWORD=short',
                     'REPORT_INTERVAL_SECONDS=0', 'REPORT_INTERVAL_SECONDS=86401',
                     'REPORT_TOKEN="space here"', 'REPORT_URL=http://user:pass@host',
                     'REPORT_URL=http://host:99999', 'REPORT_URL=https://host\\evil',
                     'REPORT_MODE=thingsboard\nTHINGSBOARD_URL=https://tb.example',
                     'REPORT_MODE=thingsboard\nTHINGSBOARD_URL=https://tb.example/api\nTHINGSBOARD_ACCESS_TOKEN=x'):
            with self.subTest(text=text), self.assertRaises(ValueError): self.parse(text)

    def test_regeneration_does_not_retain_secrets(self):
        configured = build.header(self.parse('WIFI_SSID=test\nWIFI_PASSWORD=12345678'))
        clean = build.header(self.parse(''))
        self.assertNotEqual(configured, clean)
        self.assertIn('#define C3_WIFI_PASSWORD ""', clean)

class IncrementalBuildTests(unittest.TestCase):
    def test_staging_preserves_mtime_and_refreshes_defaults(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp) / 'source'; root.mkdir()
            (root / 'main.ino').write_text('void setup() {}')
            (root / 'BuildDefaults.h').write_text('must not overwrite generated header')
            stage = Path(temp) / 'stage'
            env = dict(build.DEFAULTS, WIFI_SSID='first')
            build.prepare_sketch(stage, env, root)
            original = {p.name:p.stat().st_mtime_ns for p in stage.iterdir()}
            build.prepare_sketch(stage, env, root)
            self.assertEqual(original, {p.name:p.stat().st_mtime_ns for p in stage.iterdir()})
            (stage / 'deleted.cpp').write_text('stale')
            build.prepare_sketch(stage, build.DEFAULTS, root)
            self.assertFalse((stage / 'deleted.cpp').exists())
            self.assertEqual((stage / 'BuildDefaults.h').read_text(), build.header(build.DEFAULTS))
            self.assertEqual((stage / 'main.ino').stat().st_mtime_ns, original['main.ino'])

if __name__ == '__main__': unittest.main()
