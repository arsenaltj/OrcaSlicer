"""Validate paired package contents; archive checks run outside packaging timings.

Compare the actual NSIS/MSIX/PDB inventories made from the same install tree.
Timestamps and compressed sizes may differ, but paths, sizes and available CRCs
must match. Archives with entries lacking CRCs are extracted and SHA-256 hashed.
7-Zip also tests archive integrity. This does not install/run the app.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import subprocess
import tempfile
from pathlib import Path


def inventory(text: str) -> list[dict]:
    result = []
    for block in text.replace('\r\n', '\n').strip().split('\n\n'):
        fields = dict(line.split(' = ', 1) for line in block.splitlines() if ' = ' in line)
        if 'Path' not in fields or fields.get('Folder') == '+' or fields.get('Attributes', '').startswith('D'):
            continue
        if 'Size' not in fields:
            continue
        result.append({'path': fields['Path'].replace('\\', '/'), 'size_bytes': int(fields['Size']), 'crc': fields.get('CRC', '')})
    if not result or len({entry['path'] for entry in result}) != len(result):
        raise ValueError('Empty or duplicate archive inventory')
    return sorted(result, key=lambda entry: entry['path'])


def compare(directory: Path, seven_zip: str) -> dict:
    profiles = {name: json.loads((directory / f'packaging-{name}.json').read_text(encoding='utf-8-sig')) for name in ('baseline', 'optimized')}
    baseline, optimized = profiles.values()
    for name, profile in profiles.items():
        if profile['profile'] != name or profile['status'] != 'succeeded' or profile['inputs_unchanged'] is not True:
            raise ValueError(f'{name} did not preserve its inputs and finish successfully')
    for key in ('architecture', 'version', 'build_directory', 'input_manifest', 'msix_staging_manifest'):
        if not baseline[key] or baseline[key] != optimized[key]:
            raise ValueError(f'Packaging comparison has different {key}')
    required = {'nsis', 'msix'} | ({'pdb'} if baseline['architecture'] == 'x64' else set())
    artifacts = {name: {a['kind']: a for a in profile['artifacts']} for name, profile in profiles.items()}
    if set(artifacts['baseline']) != required | {'unused-portable-zip'} or set(artifacts['optimized']) != required:
        raise ValueError('A required package is missing or the optimized profile still creates the unused ZIP')
    inventories = {}
    for name in profiles:
        inventories[name] = {}
        for kind in sorted(required):
            artifact = artifacts[name][kind]
            path = Path(artifact['path'])
            with path.open('rb') as stream:
                digest = hashlib.file_digest(stream, 'sha256').hexdigest()
            if path.stat().st_size != artifact['size_bytes'] or digest != artifact['sha256']:
                raise ValueError(f'{name}/{kind} changed after packaging')
            for operation in ('t', 'l'):
                args = [seven_zip, operation, '-sccUTF-8'] + (['-slt', '-ba'] if operation == 'l' else []) + [str(path)]
                process = subprocess.run(args, capture_output=True, encoding='utf-8', errors='strict', timeout=900)
                (directory / f'archive-{name}-{kind}-{operation}.log').write_text(process.stdout + process.stderr, encoding='utf-8')
                if process.returncode:
                    raise ValueError(f'{name}/{kind} archive {operation} failed: {process.returncode}')
                if operation == 'l':
                    entries = inventory(process.stdout)
                    inventories[name][kind] = {'entries': entries, 'extracted_sha256': None}
                    if any(not entry['crc'] for entry in entries):
                        # NSIS listings may omit CRCs. File names/sizes alone do
                        # not prove preservation; compare the extracted bytes.
                        for entry in entries:
                            relative = Path(entry['path'])
                            if relative.is_absolute() or '..' in relative.parts or ':' in entry['path']:
                                raise ValueError('Unsafe archive inventory path')
                        with tempfile.TemporaryDirectory(prefix='orca-package-compare-') as temporary:
                            expanded = Path(temporary).resolve()
                            extraction = subprocess.run([seven_zip, 'x', '-y', '-sccUTF-8', '-o' + str(expanded), str(path)], capture_output=True, encoding='utf-8', errors='strict', timeout=900)
                            (directory / f'archive-{name}-{kind}-x.log').write_text(extraction.stdout + extraction.stderr, encoding='utf-8')
                            if extraction.returncode:
                                raise ValueError(f'{name}/{kind} archive extraction failed')
                            hashes = {}
                            for file in sorted(expanded.rglob('*')):
                                if file.is_symlink() or not file.resolve().is_relative_to(expanded):
                                    raise ValueError('Extracted archive path escapes temporary directory')
                                if file.is_file():
                                    with file.open('rb') as stream:
                                        hashes[file.relative_to(expanded).as_posix()] = hashlib.file_digest(stream, 'sha256').hexdigest()
                            if not hashes or set(hashes) != {entry['path'] for entry in entries}:
                                raise ValueError('Extraction does not match the archive file inventory')
                            inventories[name][kind]['extracted_sha256'] = hashes
    if inventories['baseline'] != inventories['optimized']:
        (directory / 'package-inventories.json').write_text(json.dumps(inventories, indent=2), encoding='utf-8')
        raise ValueError('Packaging profiles contain different files, lengths or CRCs')
    return {
        'schema_version': 1, 'contents_match': True, 'archive_integrity': 'passed',
        'input_manifest_match': True, 'msix_staging_match': True,
        'baseline_seconds': baseline['elapsed_seconds'], 'optimized_seconds': optimized['elapsed_seconds'],
        'reduction': 1 - optimized['elapsed_seconds'] / baseline['elapsed_seconds'],
        'sizes': {name: {kind: a['size_bytes'] for kind, a in rows.items()} for name, rows in artifacts.items()},
        'inventories': inventories['optimized'],
        'scope': 'Archive integrity and payload inventory; desktop and installer execution not verified',
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    seven_zip = shutil.which('7z') or str(Path('C:/Program Files/7-Zip/7z.exe'))
    report = compare(args.directory, seven_zip)
    (args.directory / 'packaging-comparison.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({k: v for k, v in report.items() if k != 'inventories'}, indent=2))


if __name__ == '__main__':
    main()
