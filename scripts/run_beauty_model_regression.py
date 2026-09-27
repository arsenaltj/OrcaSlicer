"""Replay the native BeautyTargetModelProbe on immutable, hash-pinned local assets.

Manifest: {models: [{id, path, sha256, category, split, stage}], conditions: {...}}.
Only regression/holdout_candidate splits are accepted: this tool cannot certify
that a historical asset was never used for tuning. Paths and output stay private.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import time


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def validate(manifest):
    models = manifest['models']
    if not models:
        raise ValueError('Empty model collection')
    ids, hashes = set(), set()
    for model in models:
        ident, sha = model['id'], model['sha256']
        if not re.fullmatch(r'[a-z0-9][a-z0-9_-]{0,63}', ident) or ident in ids:
            raise ValueError('Unsafe or duplicate model id')
        if not re.fullmatch(r'[0-9a-f]{64}', sha) or sha in hashes:
            raise ValueError('Invalid hash or duplicate content across splits')
        if model['split'] not in ('regression', 'holdout_candidate'):
            raise ValueError('Historical holdout independence must be verified separately')
        if not model['category'] or not model['stage']:
            raise ValueError('Category and artifact stage required')
        ids.add(ident)
        hashes.add(sha)
    return models


def check_probe(record, sha):
    return (record.get('sha256') == sha and record.get('source_unchanged') is True
            and isinstance(record.get('faces'), int) and record['faces'] > 0
            and isinstance(record.get('pieces'), int) and record['pieces'] > 0
            and record.get('changed_faces') == 0)


def run(manifest_path, executable, output, timeout=180):
    manifest_path, executable, output = map(lambda p: Path(p).resolve(),
                                            (manifest_path, executable, output))
    manifest_bytes = manifest_path.read_bytes()
    manifest_sha = hashlib.sha256(manifest_bytes).hexdigest()
    manifest = json.loads(manifest_bytes.decode('utf-8-sig'))
    models = validate(manifest)
    executable_sha = digest(executable)
    output.mkdir(parents=True, exist_ok=False)
    records = []
    for model in models:
        started = time.monotonic()
        record = dict(model, status='failed')
        source = (manifest_path.parent / model['path']).resolve()
        folder = output / model['id']
        folder.mkdir()
        try:
            if source.suffix.lower() not in ('.glb', '.obj'):
                raise ValueError('Only GLB and self-contained vertex-color OBJ supported')
            if digest(source) != model['sha256']:
                raise ValueError('Source hash mismatch')
            if source.suffix.lower() == '.obj':
                with source.open(encoding='utf-8-sig', errors='replace') as stream:
                    if any(line.lstrip().startswith('mtllib ') for line in stream):
                        raise ValueError('External OBJ materials require a separately frozen bundle')
            copy = folder / ('input' + source.suffix.lower())
            shutil.copyfile(source, copy)
            if digest(copy) != model['sha256']:
                raise ValueError('Copy hash mismatch')
            report = folder / 'probe.json'
            env = os.environ.copy()
            env.update(ORCA_TARGET_SOURCE=str(copy), ORCA_TARGET_OUTPUT=str(report))
            with (folder / 'probe.log').open('wb') as log:
                completed = subprocess.run([str(executable), '[BeautyTargetModelProbe]'],
                                           env=env, cwd=executable.parent,
                                           stdout=log, stderr=subprocess.STDOUT, timeout=timeout)
            record['exit_code'] = completed.returncode
            probe = json.loads(report.read_text(encoding='utf-8'))
            record['probe'] = probe
            record['source_unchanged'] = digest(source) == model['sha256']
            record['copy_unchanged'] = digest(copy) == model['sha256']
            if (completed.returncode == 0 and check_probe(probe, model['sha256'])
                    and record['source_unchanged'] and record['copy_unchanged']):
                record['status'] = 'passed'
        except (OSError, ValueError, subprocess.TimeoutExpired) as error:
            record['error'] = str(error)
        record['seconds'] = round(time.monotonic() - started, 3)
        records.append(record)
    stable = digest(executable) == executable_sha
    manifest_stable = digest(manifest_path) == manifest_sha
    result = {'schema': 1, 'manifest_sha256': manifest_sha,
              'manifest_unchanged': manifest_stable,
              'runner_sha256': digest(__file__),
              'executable': str(executable), 'executable_sha256': executable_sha,
              'executable_unchanged': stable,
              'source_version_claim': 'binary fingerprint only; no claim of current checkout equivalence',
              'conditions': manifest.get('conditions', {}), 'models': records,
              'passed': stable and manifest_stable and all(m['status'] == 'passed' for m in records),
              'limits': ['Path consistency only, not visual or print quality',
                         'No certified unseen holdout', 'No generation, GUI, slicing or printing']}
    (output / 'result.json').write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--manifest', required=True)
    parser.add_argument('--executable', required=True)
    parser.add_argument('--output', required=True, help='Fresh output directory; never overwritten')
    parser.add_argument('--timeout', type=int, default=180)
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error('timeout must be positive')
    result = run(args.manifest, args.executable, args.output, args.timeout)
    print(json.dumps({'passed': result['passed'], 'models': len(result['models'])}))
    raise SystemExit(0 if result['passed'] else 1)
