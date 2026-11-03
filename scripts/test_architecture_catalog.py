"""Architecture drift checks must reject stale mappings without inventing isolation."""
import copy
from pathlib import Path
import tempfile
import unittest

import architecture_review as ar


class CatalogTests(unittest.TestCase):
    def setUp(self):
        self.inventory = {'src/app.cpp': b'#include "core.hpp"\n', 'src/core.hpp': b'class Core {};',
                          'src/native.ipp': b'inline void apply() {}', 'tests/test_core.cpp': b'TEST_CASE("core") {}'}
        self.config = {'evidence': [{'id': 'core', 'path': 'src/core.hpp', 'needle': 'class Core'}],
                       'landscape': {'capabilities': [
                           {'id': 'app', 'paths': ['src/app.cpp'], 'evidence': ['core'], 'tests': ['tests/test_*.cpp']},
                           {'id': 'core', 'paths': ['src/core.hpp'], 'evidence': ['core'], 'tests': []}],
                           'layers': [{'id': 'top', 'capabilities': ['app']}, {'id': 'bottom', 'capabilities': ['core']}],
                           'scenarios': [{'id': 'change', 'capabilities': ['core', 'app']}]}}

    def test_drift_rejects_missing_needle_even_when_file_exists(self):
        self.assertEqual(ar.validate_map(self.config, self.inventory, self.inventory), [])
        self.config['evidence'][0]['needle'] = 'class Retired'
        self.assertIn('missing anchor: core (src/core.hpp)', ar.validate_map(self.config, self.inventory, self.inventory))

    def test_each_scope_and_reference_is_checked(self):
        c = self.config['landscape']['capabilities'][0]
        c['paths'].append('src/retired*')
        c['tests'].append('tests/deleted*')
        c['evidence'].append('unknown')
        self.config['landscape']['scenarios'][0]['capabilities'].append('missing')
        errors = ar.validate_map(self.config, self.inventory, self.inventory)
        self.assertTrue(any('empty code scope: app -> src/retired*' == e for e in errors))
        self.assertTrue(any('empty test scope' in e for e in errors))
        self.assertIn('unknown evidence: app -> unknown', errors)
        self.assertIn('unknown scenario capability: missing', errors)
        self.config['business_flows'] = [{'id': 'gallery', 'evidence': ['retired_gallery']}]
        self.assertIn('unknown business evidence: gallery -> retired_gallery',
                      ar.validate_map(self.config, self.inventory, self.inventory))

    def test_duplicate_and_unlayered_capabilities_cannot_hide(self):
        self.config['evidence'].append(copy.deepcopy(self.config['evidence'][0]))
        self.config['landscape']['layers'][1]['capabilities'] = ['app']
        errors = ar.validate_map(self.config, self.inventory, self.inventory)
        self.assertIn('duplicate evidence: core', errors)
        self.assertIn('capability must appear in one layer: core', errors)
        self.assertIn('capability must appear in one layer: app', errors)

    def test_catalog_separates_unparsed_unassigned_and_static_edges(self):
        report = {'changes': [], 'potential_files': []}
        graph = {'files': ['src/app.cpp', 'src/core.hpp'], 'edges': [
            {'source': 'src/app.cpp', 'target': 'src/core.hpp', 'kind': 'IMPORTS_FROM', 'line': 1}]}
        ar.enrich_catalog(report, self.config, self.inventory, graph)
        catalog = report['catalog']
        self.assertEqual(catalog['coverage'], {'inventory_files': 4, 'indexed_files': 2, 'mapped_files': 2,
                                              'curated_files': 2, 'directory_default_files': 0, 'unindexed_files': 2})
        self.assertEqual(catalog['unassigned_files'], ['src/native.ipp'])
        self.assertEqual(catalog['dependencies'][0]['source'], 'app')
        self.assertEqual(catalog['dependencies'][0]['target'], 'core')
        self.assertEqual(catalog['capabilities'][0]['test_files'], ['tests/test_core.cpp'])
        original = catalog['inventory_identity']
        self.inventory['src/native.ipp'] += b'\n// changed'
        ar.enrich_catalog(report, self.config, self.inventory, graph)
        self.assertNotEqual(report['catalog']['inventory_identity'], original)

    def test_overlapping_responsibilities_do_not_fabricate_dependencies(self):
        self.config['landscape']['capabilities'][1]['paths'].append('src/app.cpp')
        report = {'changes': [], 'potential_files': []}
        graph = {'files': list(self.inventory), 'edges': [
            {'source': 'src/app.cpp', 'target': 'src/core.hpp', 'kind': 'CALLS'}]}
        ar.enrich_catalog(report, self.config, self.inventory, graph)
        self.assertEqual(report['catalog']['dependencies'], [])

    def test_directory_defaults_never_steal_curated_files(self):
        self.config['landscape']['capabilities'].append({'id':'fallback', 'paths':['src'], 'directory_default':True})
        report={'changes':[], 'potential_files':[]}
        ar.enrich_catalog(report, self.config, self.inventory, {'files':[], 'edges':[]})
        catalog=report['catalog']
        self.assertEqual(catalog['capabilities'][2]['files'], ['src/native.ipp'])
        self.assertEqual(catalog['coverage']['curated_files'], 2)
        self.assertEqual(catalog['coverage']['directory_default_files'], 1)
        self.assertEqual(catalog['unassigned_files'], [])

    def test_catalog_reads_selected_revision_and_worktree_separately(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder).resolve()
            for args in [('init', '-q'), ('config', 'user.name', 'Test'), ('config', 'user.email', 'test@example.invalid')]:
                ar.git(root, *args)
            (root / 'src').mkdir()
            p = root / 'src' / 'native.ipp'
            p.write_bytes(b'old')
            (root / 'src' / 'CMakeLists.txt').write_bytes(b'add_library(native native.ipp)')
            ar.git(root, 'add', '.')
            ar.git(root, 'commit', '-qm', 'fixture')
            head = ar.git(root, 'rev-parse', 'HEAD').decode().strip()
            p.write_bytes(b'current')
            config = {'landscape': {'source_roots': ['src']}}
            self.assertEqual(ar.catalog_sources(root, head, config)['src/native.ipp'], b'old')
            self.assertEqual(ar.catalog_sources(root, head, config, True)['src/native.ipp'], b'current')
            self.assertIn('src/CMakeLists.txt', ar.catalog_sources(root, head, config))


if __name__ == '__main__':
    unittest.main()
