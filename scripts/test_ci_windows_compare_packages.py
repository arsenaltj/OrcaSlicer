import copy
import json
import hashlib
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from ci_windows_compare_packages import inventory, compare


# Representative blocks from the real 7-Zip NSIS listing in CI run 34935451971.
NSIS_LISTING = (
    'Path = $PLUGINSDIR\\InstallOptions.dll\nSize = \nPacked Size = 7684\n'
    'Modified = \nAttributes = \nMethod = LZMA:23\nSolid = -\nOffset = 0\n\n'
    'Path = resources\\images\\OrcaSlicer.ico\nSize = \nPacked Size = 138833\n'
    'Modified = 2026-09-15 06:07:04.0000000\nAttributes = \n'
    'Method = LZMA:23\nSolid = -\nOffset = 19273\n\n'
    'Path = Uninstall.exe\nSize = \nPacked Size = 138846\n'
    'Modified = \nAttributes = \nMethod = LZMA:23\nSolid = -\nOffset = 191524570\n'
)
NSIS_FILES = {
    '$PLUGINSDIR/InstallOptions.dll': b'plugin contents',
    'resources/images/OrcaSlicer.ico': b'icon contents',
    'Uninstall.exe': b'uninstaller contents',
}


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
        for value in ('', 'Path = x\nSize = 2\n\nPath = x\nSize = 2', 'Path = x\nSize = \n\nPath = x\n'):
            with self.assertRaises(ValueError):
                inventory(value)

    def test_blank_and_missing_sizes_are_unknown_and_zero_is_known(self):
        listing = 'Path = blank\nSize = \n\nPath = missing\nCRC = AAAA\n\nPath = zero\nSize = 0\n'
        self.assertEqual(inventory(listing), [
            {'path': 'blank', 'size_bytes': None, 'crc': ''},
            {'path': 'missing', 'size_bytes': None, 'crc': 'AAAA'},
            {'path': 'zero', 'size_bytes': 0, 'crc': ''},
        ])

    def test_invalid_declared_sizes_fail(self):
        for size in ('-1', 'invalid', '1.5'):
            with self.subTest(size=size), self.assertRaises(ValueError):
                inventory(f'Path = app.exe\nSize = {size}\n')

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

    def run_pair(self, *, differing_crc=False, missing_crc=False, differing_bytes=False, corrupt=False,
                 listing=None, files=None, optimized_files=None, extraction_error=False):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            self.paired_reports(directory)
            calls = []
            def native(args, **kwargs):
                calls.append(args[1])
                optimized = Path(args[-1]).name.startswith('optimized')
                if args[1] == 'x':
                    output = Path(next(x[2:] for x in args if x.startswith('-o')))
                    payloads = files if files is not None else {'app.exe': b'changed!' if optimized and differing_bytes else b'original'}
                    if optimized and optimized_files is not None:
                        payloads = optimized_files
                    for name, contents in payloads.items():
                        file = output / name
                        file.parent.mkdir(parents=True, exist_ok=True)
                        file.write_bytes(contents)
                text = listing
                if text is None:
                    text = 'Path = app.exe\nSize = 8\n'
                    if not missing_crc:
                        text += 'CRC = ' + ('BBBB' if optimized and differing_crc else 'AAAA') + '\n'
                failed = (corrupt and args[1] == 't') or (extraction_error and args[1] == 'x')
                return subprocess.CompletedProcess(args, 2 if failed else 0, stdout=text, stderr='')
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
        with self.assertRaisesRegex(ValueError, 'Packaging profiles contain different'):
            self.run_pair(missing_crc=True, differing_bytes=True)

    def test_real_nsis_listing_retains_and_measures_every_file(self):
        entries = inventory(NSIS_LISTING)
        self.assertEqual({entry['path'] for entry in entries}, set(NSIS_FILES))
        self.assertTrue(all(entry['size_bytes'] is None and not entry['crc'] for entry in entries))
        report, calls = self.run_pair(listing=NSIS_LISTING, files=NSIS_FILES)
        self.assertTrue(report['contents_match'])
        self.assertEqual(calls.count('x'), 6)
        result = report['inventories']['nsis']
        self.assertEqual({entry['path']: entry['size_bytes'] for entry in result['entries']},
                         {name: len(contents) for name, contents in NSIS_FILES.items()})
        self.assertEqual(result['extracted_sha256'],
                         {name: hashlib.sha256(contents).hexdigest() for name, contents in NSIS_FILES.items()})

    def test_unknown_size_still_requires_extraction_when_crc_exists(self):
        for size_field in ('Size = \n', ''):
            with self.subTest(size_field=size_field):
                report, calls = self.run_pair(listing=f'Path = app.exe\n{size_field}CRC = AAAA\n')
                self.assertEqual(calls.count('x'), 6)
                self.assertEqual(report['inventories']['nsis']['entries'][0]['size_bytes'], 8)

    def test_unknown_size_files_with_changed_bytes_fail(self):
        for name in NSIS_FILES:
            with self.subTest(name=name):
                changed = dict(NSIS_FILES)
                changed[name] = b'X' + changed[name][1:]
                with self.assertRaisesRegex(ValueError, 'Packaging profiles contain different'):
                    self.run_pair(listing=NSIS_LISTING, files=NSIS_FILES, optimized_files=changed)

    def test_missing_or_extra_extracted_files_fail(self):
        variants = [dict(NSIS_FILES, extra=b'unlisted')]
        for name in NSIS_FILES:
            variants.append({path: contents for path, contents in NSIS_FILES.items() if path != name})
        for files in variants:
            with self.subTest(paths=sorted(files)), self.assertRaisesRegex(ValueError, 'Extraction does not match'):
                self.run_pair(listing=NSIS_LISTING, files=NSIS_FILES, optimized_files=files)

    def test_extraction_validates_declared_lengths(self):
        for size in (0, 7, 9):
            with self.subTest(size=size), self.assertRaisesRegex(ValueError, 'Extracted file length differs'):
                self.run_pair(listing=f'Path = app.exe\nSize = {size}\n')

    def test_unknown_size_does_not_allow_extraction_errors(self):
        with self.assertRaisesRegex(ValueError, 'archive extraction failed'):
            self.run_pair(listing=NSIS_LISTING, files=NSIS_FILES, extraction_error=True)

    def test_unsafe_unknown_size_paths_fail_before_extraction(self):
        for path in ('../escape.exe', '/absolute.exe', 'C:/drive.exe'):
            with self.subTest(path=path), tempfile.TemporaryDirectory() as tmp:
                directory = Path(tmp)
                self.paired_reports(directory)
                def native(args, **kwargs):
                    if args[1] == 'x':
                        raise AssertionError('unsafe paths must be rejected before extraction')
                    return subprocess.CompletedProcess(args, 0, stdout=f'Path = {path}\nSize = \n', stderr='')
                with patch('ci_windows_compare_packages.subprocess.run', side_effect=native):
                    with self.assertRaisesRegex(ValueError, 'Unsafe archive inventory path'):
                        compare(directory, '7z')


if __name__ == '__main__':
    unittest.main()
