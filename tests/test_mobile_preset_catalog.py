"""Host checks for the generated native/Android catalog's real profile provenance."""
import hashlib
import importlib.util
import json
import re
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('mobile_catalog', ROOT / 'mobile-slicing/generate_preset_catalog.py')
GEN = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GEN)

class OfflineCatalogTests(unittest.TestCase):
    def setUp(self):
        self.catalog = json.loads((ROOT / 'mobile-slicing/preset-catalog-v1.json').read_text(encoding='utf-8'))

    def test_committed_json_and_native_bytes_match_the_generator(self):
        for path, content in GEN.generate().items():
            with self.subTest(path=path):
                self.assertEqual(path.read_text(encoding='utf-8'), content)

    def test_native_catalog_literals_fit_msvc_and_reconstruct_json(self):
        header = (ROOT / 'src/orca_mobile_slicing_sdk/preset_catalog.generated.hpp').read_text(encoding='utf-8')
        chunks = re.findall(r'R"ORCAPRESET\((.*?)\)ORCAPRESET"', header, re.DOTALL)
        self.assertGreater(len(chunks), 1)
        self.assertTrue(all(len(chunk.encode('utf-8')) <= GEN.CPP_LITERAL_MAX_BYTES for chunk in chunks))
        self.assertEqual(''.join(chunks), (ROOT / 'mobile-slicing/preset-catalog-v1.json').read_text(encoding='utf-8'))

    def test_all_provenance_hashes_match_the_actual_versioned_sources(self):
        self.assertTrue(self.catalog['sourceRefs'])
        for entry in self.catalog['devices'] + self.catalog['materials']:
            for ref in entry.get('sourceRefs', []):
                with self.subTest(path=ref['path']):
                    self.assertEqual(hashlib.sha256((ROOT / ref['path']).read_bytes().replace(b'\r\n', b'\n')).hexdigest(), ref['sha256'])

    def test_temperature_density_flow_and_price_are_resolved_from_parent_json(self):
        for entry, kind in zip(self.catalog['materials'][1:], ('pla', 'pet')):
            with self.subTest(material=kind):
                parent = json.loads((ROOT / ('resources/profiles/OrcaFilamentLibrary/filament/base/fdm_filament_' + kind + '.json')).read_text())
                common = json.loads((ROOT / 'resources/profiles/OrcaFilamentLibrary/filament/base/fdm_filament_common.json').read_text())
                for key in ('nozzle_temperature', 'nozzle_temperature_initial_layer', 'filament_density', 'filament_flow_ratio', 'filament_cost'):
                    self.assertEqual(entry['config'][key], parent.get(key, common[key]))
                self.assertEqual(len(entry['sourceRefs']), 3)
                self.assertNotIn('costCurrency', entry)

    def test_unsupported_legacy_keys_are_disclosed_not_sent_to_native(self):
        schema = (ROOT / 'src/libslic3r/PrintConfig.cpp').read_text(encoding='utf-8')
        keys = set(re.findall(r'\badd\s*\(\s*"([^"]+)"', schema))
        for entry in self.catalog['devices'][1:] + self.catalog['materials'][1:]:
            with self.subTest(preset=entry['id']):
                self.assertFalse(set(entry['config']) - keys)
                self.assertTrue(entry['unsupportedSourceOptions'])
                self.assertFalse(set(entry['unsupportedSourceOptions']) & keys)
                self.assertEqual(hashlib.sha256((ROOT / entry['schemaSourceRef']['path']).read_bytes().replace(b'\r\n', b'\n')).hexdigest(), entry['schemaSourceRef']['sha256'])

    def test_source_hashes_are_stable_across_git_line_endings(self):
        import tempfile
        with tempfile.TemporaryDirectory(dir=ROOT) as directory:
            sample = Path(directory) / 'profile.json'
            sample.write_bytes(b'{"value": "real"}\n')
            lf = GEN.digest(sample)
            sample.write_bytes(b'{"value": "real"}\r\n')
            crlf = GEN.digest(sample)
            self.assertEqual(lf, crlf)
            self.assertEqual(lf['hashFormat'], 'utf8-lf')

    def test_generic_machine_is_a_disclosed_full_reset_using_catalog_pla(self):
        device = self.catalog['devices'][1]
        self.assertTrue(device['requiresConfirmation'])
        self.assertEqual(device['resetScopes'], ['device', 'material', 'process'])
        self.assertEqual(device['config']['printable_area'], ['0x0', '200x0', '200x200', '0x200'])
        for key, value in self.catalog['materials'][1]['config'].items():
            self.assertEqual(device['config'][key], value)

if __name__ == '__main__':
    unittest.main()
