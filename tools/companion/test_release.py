import copy
import hashlib
import json
import shutil
import re
from pathlib import Path
import tempfile
import unittest
from build_site import mirror, validate, version_assets, ROOT
from release_assets import FILES


class ReleaseTests(unittest.TestCase):
    def setUp(self):
        self.payload = b'release-fixture'
        self.manifest = {'schema': 1, 'layout': 'orb-printsphere-v1', 'chip': 'ESP32-S3',
            'flashSize': 0x1000000, 'parts': [
                {'path': name, 'offset': offset, 'size': len(self.payload),
                 'sha256': hashlib.sha256(self.payload).hexdigest()}
                for name, (_, offset, _) in FILES.items()]}

    def test_only_public_images_are_mirrored(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp)
            entry = mirror(path, 'v1.2.3', json.dumps(self.manifest).encode(), lambda _: self.payload)
            self.assertEqual(set(p.name for p in (path / 'releases/v1.2.3').iterdir()),
                             {*FILES, 'companion-release.json'})
            self.assertEqual(entry['manifest'], 'releases/v1.2.3/companion-release.json')

    def test_ui_assets_are_versioned_including_module_dependencies(self):
        with tempfile.TemporaryDirectory() as temp:
            site = Path(temp)
            def prepare():
                shutil.copytree(ROOT / 'web/companion', site, dirs_exist_ok=True)
                version_assets(site)
                page = (site / 'index.html').read_text(encoding='utf-8')
                module = re.search(r'src="(installer\.[a-f0-9]+\.mjs)"', page).group(1)
                css = re.search(r'href="(style\.[a-f0-9]+\.css)"', page).group(1)
                self.assertTrue((site / css).is_file())
                code = (site / module).read_text(encoding='utf-8')
                layout = re.search(r"from './(layout\.[a-f0-9]+\.mjs)'", code).group(1)
                self.assertTrue((site / layout).is_file())
                return module, layout
            original = prepare()
            self.assertEqual(prepare(), original)
            # A layout-only change must also invalidate the importing installer URL.
            shutil.copytree(ROOT / 'web/companion', site, dirs_exist_ok=True)
            with (site / 'layout.mjs').open('a', encoding='utf-8') as out:
                out.write('\n// changed layout')
            version_assets(site)
            page = (site / 'index.html').read_text(encoding='utf-8')
            self.assertNotIn(original[0], page)

    def test_rejects_modified_download(self):
        with tempfile.TemporaryDirectory() as temp, self.assertRaises(ValueError):
            mirror(Path(temp), 'v1.2.3', json.dumps(self.manifest).encode(), lambda _: b'bad')

    def test_rejects_path_traversal(self):
        with tempfile.TemporaryDirectory() as temp, self.assertRaises(ValueError):
            mirror(Path(temp), '../bad', json.dumps(self.manifest).encode(), lambda _: self.payload)

    def test_rejects_private_asset_and_wrong_address(self):
        bad = copy.deepcopy(self.manifest)
        bad['parts'][0]['path'] = 'orb-before-dualboot.bin'
        with self.assertRaises(ValueError):
            validate(bad)
        bad = copy.deepcopy(self.manifest)
        bad['parts'][0]['offset'] = 0x9000
        with self.assertRaises(ValueError):
            validate(bad)


if __name__ == '__main__':
    unittest.main()
