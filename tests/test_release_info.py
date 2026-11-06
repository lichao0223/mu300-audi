"""CI must select the published kernel and refuse mismatched or incomplete bundles."""
import gzip
import hashlib
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from helpers import TOP

spec = importlib.util.spec_from_file_location('release_info', TOP / 'kernel/kits/release-info.py')
release_info = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release_info)


class ReleaseInfo(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='mu300-release-info-')
        self.addCleanup(self.tmp.cleanup)
        self.directory = Path(self.tmp.name)

    def image(self, krel='7.3.0', config=b'CONFIG_Z=y\n# comment\n# CONFIG_A is not set\n'):
        image = b'header\0Linux version ' + krel.encode() + b' (builder) #1\0'
        image += b'IKCFG_ST' + gzip.compress(config) + b'IKCFG_EDtail'
        (self.directory / 'Image').write_bytes(image)

    def test_version_and_normalized_config(self):
        self.image()
        (self.directory / 'kernel.release').write_text('7.3.0\n')
        expected = hashlib.sha256(b'# CONFIG_A is not set\nCONFIG_Z=y\n').hexdigest()
        self.assertEqual(release_info.release_info(self.directory, '7.3'), ('7.3.0', expected))
        self.image(config=b'# CONFIG_A is not set\n\nCONFIG_Z=y\n# another comment\n')
        self.assertEqual(release_info.release_info(self.directory, '7.3')[1], expected)

    def test_vendor_suffix_and_stable_patch_version(self):
        for key, krel in [('5.4', '5.4.254-gb50db5b6224c'), ('6.18', '6.18.59'), ('7.3', '7.3.1')]:
            with self.subTest(krel=krel):
                self.image(krel)
                self.assertEqual(release_info.release_info(self.directory, key)[0], krel)

    def test_ci_selects_the_source_tarball_version(self):
        for krel, kv in [('7.3.0', '7.3'), ('7.3.1', '7.3.1')]:
            with self.subTest(krel=krel):
                self.image(krel)
                result = subprocess.run([sys.executable, str(TOP / 'kernel/kits/release-info.py'),
                                         str(self.directory), '7.3'], check=True, capture_output=True, text=True)
                values = dict(line.split('=', 1) for line in result.stdout.splitlines())
                self.assertEqual(values['KREL'], krel)
                self.assertEqual(values['KV'], kv)

    def test_wrong_series_or_recorded_version(self):
        self.image()
        with self.assertRaisesRegex(ValueError, 'not kernel'):
            release_info.release_info(self.directory, '6.18')
        (self.directory / 'kernel.release').write_text('7.3.1\n')
        with self.assertRaisesRegex(ValueError, 'differs'):
            release_info.release_info(self.directory, '7.3')

    def test_missing_version_config_and_corrupt_gzip(self):
        for image in [b'not a kernel', b'Linux version 7.3.0 (builder)',
                      b'Linux version 7.3.0 (builder)\0IKCFG_STbrokenIKCFG_ED']:
            with self.subTest(image=image):
                (self.directory / 'Image').write_bytes(image)
                with self.assertRaises((ValueError, OSError)):
                    release_info.release_info(self.directory, '7.3')
        self.image(config=b'# comments only\n')
        with self.assertRaisesRegex(ValueError, 'empty'):
            release_info.release_info(self.directory, '7.3')


if __name__ == '__main__':
    unittest.main()
