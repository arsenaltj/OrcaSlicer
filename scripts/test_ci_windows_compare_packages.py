import copy
import json
import hashlib
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from ci_windows_compare_packages import inventory, compare


class PackageInventoryTests(unittest.TestCase):
    def paired_reports(self, directory):
        for profile in ('baseline', 'optimized'):
            artifacts = []
            for kind in ['nsis', 'msix', 'pdb'] + (['unused-portable-zip'] if profile == 'baseline' else []):
                path = directory / f'{profile}-{kind}.archive'
                path.write_bytes(b'package fixture')
                artifacts.append(dict(kind=kind, path=str(path), size_bytes=path.stat().st_size, sha256=hashlib.sha256(path.read_bytes()).hexdigest()))
            report = dict(profile=profile, status='succeeded', inputs_unchanged=True, architecture='x64', version='test', build_directory='build', input_manifest={'pdb': ['original']}, msix_staging_manifest=['original'], artifacts=artifacts, elapsed_seconds=20 if profile == 'baseline' else 10)
            (directory / f'packaging-{profile}.json').write_text(json.dumps(report), encoding='utf-8')

    def test_nested_unicode_files_ignore_directory_and_timestamp(self):
        listing = 'Path = resources\nFolder = +\nSize = 0\n\nPath = resources\\中文.json\nSize = 9\nCRC = ABCD\nModified = yesterday\n'
        self.assertEqual(inventory(listing), [{'path': 'resources/中文.json', 'size_bytes': 9, 'crc': 'ABCD'}])

    def test_empty_and_duplicate_inventories_fail(self):
        for value in ('', 'Path = x\nSize = 2\n\nPath = x\nSize = 2'):
            with self.assertRaises(ValueError):
                inventory(value)

    def test_different_input_or_missing_pdb_fails_before_native_tools(self):
        baseline = dict(profile='baseline', status='succeeded', inputs_unchanged=True, architecture='x64', version='test', build_directory='build', input_manifest={'pdb': ['original']}, msix_staging_manifest=['original'], artifacts=[{'kind': k} for k in ('pdb', 'nsis', 'msix', 'unused-portable-zip')])
        optimized = copy.deepcopy(baseline)
        optimized.update(profile='optimized', artifacts=[{'kind': k} for k in ('pdb', 'nsis', 'msix')])
        for mutate in (lambda p: p['input_manifest'].update(pdb=['changed']), lambda p: p['artifacts'].pop(0), lambda p: p.update(status='failed'), lambda p: p.update(inputs_unchanged=False)):
            candidate = copy.deepcopy(optimized)
            mutate(candidate)
            with tempfile.TemporaryDirectory() as tmp:
                directory = Path(tmp)
                for name, data in [('baseline', baseline), ('optimized', candidate)]:
                    (directory / f'packaging-{name}.json').write_text(json.dumps(data), encoding='utf-8')
                with patch('ci_windows_compare_packages.subprocess.run', side_effect=AssertionError('native tools must not run')):
                    with self.assertRaises(ValueError):
                        compare(directory, '7z')

    def run_pair(self, *, differing_crc=False, missing_crc=False, differing_bytes=False, corrupt=False):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            self.paired_reports(directory)
            calls = []
            def native(args, **kwargs):
                calls.append(args[1])
                optimized = Path(args[-1]).name.startswith('optimized')
                if args[1] == 'x':
                    output = Path(next(x[2:] for x in args if x.startswith('-o')))
                    (output / 'app.exe').write_bytes(b'different' if optimized and differing_bytes else b'original')
                text = 'Path = app.exe\nSize = 8\n'
                if not missing_crc:
                    text += 'CRC = ' + ('BBBB' if optimized and differing_crc else 'AAAA') + '\n'
                return subprocess.CompletedProcess(args, 2 if corrupt and args[1] == 't' else 0, stdout=text, stderr='')
            with patch('ci_windows_compare_packages.subprocess.run', side_effect=native):
                report = compare(directory, '7z')
            return report, calls

    def test_verified_pair_reports_speed_and_retains_required_packages(self):
        report, calls = self.run_pair()
        self.assertTrue(report['contents_match'])
        self.assertEqual(report['reduction'], .5)
        self.assertEqual(set(report['sizes']['optimized']), {'nsis', 'msix', 'pdb'})
        self.assertEqual(calls.count('t'), 6)
        self.assertNotIn('x', calls)

    def test_crc_difference_or_corrupt_archive_blocks_comparison(self):
        for options in (dict(differing_crc=True), dict(corrupt=True)):
            with self.assertRaises(ValueError):
                self.run_pair(**options)

    def test_missing_crc_requires_matching_extracted_hashes(self):
        report, calls = self.run_pair(missing_crc=True)
        self.assertTrue(report['contents_match'])
        self.assertEqual(calls.count('x'), 6)
        with self.assertRaises(ValueError):
            self.run_pair(missing_crc=True, differing_bytes=True)


if __name__ == '__main__':
    unittest.main()
