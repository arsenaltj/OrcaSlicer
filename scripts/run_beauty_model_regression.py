"""Replay the native BeautyTargetModelProbe on immutable, hash-pinned local assets.

Manifest: {models: [{id, path, sha256, category, split, stage}], conditions: {...}}.
Only regression/holdout_candidate splits are accepted: this tool cannot certify
that a historical asset was never used for tuning. Paths and output stay private.
"""
import argparse
import hashlib
import json
import os
import math
import sys
from pathlib import Path
import re
import shutil
import subprocess
import statistics
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


STAGES = ('load_decode', 'surface_adjacency', 'source_face_colors', 'partition',
          'puzzle_copies', 'direct_match', 'restrictive_match', 'json_encode', 'json_decode', 'rematch')


def performance_summary(probe, repeats):
    measurements = probe.get('measurements', [])
    expected_phases = ['first_process_pass', 'warmup'] + ['warm'] * repeats
    if [m.get('phase') for m in measurements] != expected_phases:
        raise ValueError('Missing or incomplete repeated native measurements')
    signatures = {m.get('output_sha256') for m in measurements}
    if len(signatures) != 1 or not re.fullmatch(r'[0-9a-f]{64}', next(iter(signatures)) or ''):
        raise ValueError('Nondeterministic or missing output signature')
    for measurement in measurements:
        stages = measurement.get('stages_seconds', {})
        if set(stages) != set(STAGES) or measurement.get('changed_faces') != 0:
            raise ValueError('Incomplete stages or changed assignments')
        for value in [*stages.values(), measurement.get('operation_seconds')]:
            if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value) or value < 0:
                raise ValueError('Invalid timing measurement')
    warm = measurements[2:]
    def stats(values):
        return {'median': statistics.median(values), 'min': min(values), 'max': max(values)}
    stages = {name: stats([m['stages_seconds'][name] for m in warm]) for name in STAGES}
    return {'first_process_pass_seconds': measurements[0]['operation_seconds'],
            'warm_operation_seconds': stats([m['operation_seconds'] for m in warm]),
            'warm_stages_seconds': stages,
            'bottlenecks': sorted(STAGES, key=lambda name: stages[name]['median'], reverse=True),
            'process_lifetime_peak_working_set_bytes': max(
                (m['memory']['process_lifetime_peak_working_set_bytes'] for m in measurements if m.get('memory')), default=None),
            'output_sha256': next(iter(signatures))}


def run(manifest_path, executable, output, timeout=180, repeats=0, quality=False, quality_stages=False):
    if quality_stages and not quality:
        raise ValueError('Stage replay requires quality export')
    if quality and repeats:
        raise ValueError('Quality export and performance repeats must run separately')
    if not 0 <= repeats <= 20:
        raise ValueError('repeats must be between 0 and 20')
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
            env.update(ORCA_TARGET_SOURCE=str(copy), ORCA_TARGET_OUTPUT=str(report), ORCA_TARGET_REPEATS=str(repeats))
            env.pop('ORCA_TARGET_QUALITY_DIR', None)
            env.pop('ORCA_TARGET_QUALITY_STAGES', None)
            if quality_stages:
                env['ORCA_TARGET_QUALITY_STAGES'] = '1'
            if quality:
                env['ORCA_TARGET_QUALITY_DIR'] = str(folder / 'quality')
            with (folder / 'probe.log').open('wb') as log:
                completed = subprocess.run([str(executable), '[BeautyTargetModelProbe]'],
                                           env=env, cwd=executable.parent,
                                           stdout=log, stderr=subprocess.STDOUT, timeout=timeout)
            record['exit_code'] = completed.returncode
            probe = json.loads(report.read_text(encoding='utf-8'))
            record['probe'] = probe
            if repeats:
                record['performance'] = performance_summary(probe, repeats)
            if quality:
                exported = json.loads((folder / 'quality/manifest.json').read_text(encoding='utf-8'))
                if exported['source_sha256'] != model['sha256']:
                    raise ValueError('Quality export source mismatch')
                record['quality_manifest'] = str(folder / 'quality/manifest.json')
                if quality_stages:
                    paths = [folder / 'quality' / stage / 'manifest.json' for stage in
                             ('01-create28', '02-regularize', '03-smooth')]
                    for path in paths:
                        stage_meta = json.loads(path.read_text(encoding='utf-8'))
                        if any(stage_meta[key] != exported[key] for key in
                               ('source_sha256', 'loaded_mesh_colors_sha256', 'geometry_id', 'palette')):
                            raise ValueError('Stage replay input mismatch')
                    if stage_meta['output_sha256'] != exported['output_sha256']:
                        raise ValueError('Stage replay differs from workbench fallback')
                    record['quality_stages'] = [str(path) for path in paths]
                    ablation = folder / 'quality/04-ablation-no-regularize/manifest.json'
                    candidate = json.loads(ablation.read_text(encoding='utf-8'))
                    if any(candidate[key] != exported[key] for key in
                           ('source_sha256', 'loaded_mesh_colors_sha256', 'geometry_id', 'palette')):
                        raise ValueError('Ablation input mismatch')
                    record['quality_ablation'] = str(ablation)
            record['source_unchanged'] = digest(source) == model['sha256']
            record['copy_unchanged'] = digest(copy) == model['sha256']
            if (completed.returncode == 0 and check_probe(probe, model['sha256'])
                    and record['source_unchanged'] and record['copy_unchanged']):
                record['status'] = 'passed'
        except (OSError, ValueError, KeyError, TypeError, subprocess.TimeoutExpired) as error:
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
              'measurement_conditions': {'warm_repeats': repeats, 'os': sys.platform,
                                         'windows_version': list(sys.getwindowsversion()) if sys.platform == 'win32' else None,
                                         'processor': os.environ.get('PROCESSOR_IDENTIFIER', 'unavailable'), 'logical_cpus': os.cpu_count(),
                                         'cache': 'OS/allocator caches uncontrolled; input hashing/copying precedes first pass',
                                         'wall_seconds': 'includes copies, hash verification, process launch and validation'},
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
    parser.add_argument('--repeats', type=int, default=0,
                        help='0: single check; 1–20: first pass, warmup, then N warm passes in one process')
    parser.add_argument('--quality', action='store_true', help='Export native face colors and geometry for offline quality diagnosis')
    parser.add_argument('--quality-stages', action='store_true', help='With --quality, replay partition stages and verify final fallback equivalence')
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error('timeout must be positive')
    result = run(args.manifest, args.executable, args.output, args.timeout, args.repeats, args.quality, args.quality_stages)
    print(json.dumps({'passed': result['passed'], 'models': len(result['models'])}))
    raise SystemExit(0 if result['passed'] else 1)
