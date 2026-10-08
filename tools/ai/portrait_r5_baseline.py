"""Preserve and verify the user-approved R4 inputs without altering them."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil


def sha(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def preserve_variants(repo, output):
    export = repo / '.tmp/dev/reports/portrait-export-r4-b/export.json'
    report = json.loads(export.read_text())
    destination = output / 'baseline/variants'
    destination.mkdir(parents=True, exist_ok=False)
    entries = []
    for variant in report['variants']:
        for field in ('face_colors_path', 'subface_colors_path'):
            name = variant[field]
            if Path(name).name != name:
                raise ValueError('Unsafe R4 variant path')
            original, copied = export.parent / name, destination / name
            digest = sha(original)
            shutil.copyfile(original, copied)
            if sha(copied) != digest or sha(original) != digest:
                raise ValueError('R4 variant changed while preserving it')
            entries.append({'original': str(original), 'path': str(copied.relative_to(output)), 'sha256': digest})
    with (output / 'stage-baseline-variants.json').open('x') as stream:
        json.dump({'status': 'PASS', 'files': entries, 'export_sha256': sha(export)}, stream, indent=2, sort_keys=True)
    print(json.dumps({'stage': 1, 'variant_files': len(entries), 'status': 'PASS'}))


def capture(repo, output, checkpoint):
    baseline = repo / '.tmp/dev/reports/brow-eye-repair-r4/replay-b'
    evidence = baseline / 'evidence.json'
    expected = 'b17cc405ef3dbe82930b86ed7bca478a44ae219b148f2e3686803b48fedbbd81'
    if sha(evidence) != expected:
        raise ValueError('R4 evidence changed')
    request = json.loads((baseline / 'request.json').read_text())
    export = repo / '.tmp/dev/reports/portrait-export-r4-b/export.json'
    report = json.loads(export.read_text())
    source = Path(request['source_path'])
    if sha(source) != request['source_sha256'] or report['evidence_sha256'] != expected:
        raise ValueError('R4 source/export identity changed')
    if report['face_count'] != 976825 or report['lock_count'] != 6:
        raise ValueError('R4 face mapping changed')
    if sum(item['face_count'] for item in report['lock_details']) != 1869:
        raise ValueError('R4 locks changed')
    if not (checkpoint / 'COMPLETE.json').is_file():
        raise ValueError('Source checkpoint is incomplete')
    output.mkdir(parents=True, exist_ok=False)
    inputs = output / 'baseline'
    inputs.mkdir()
    files = [(source, inputs / 'source.glb'), (export, inputs / 'export.json')]
    files += [(path, inputs / 'replay' / path.relative_to(baseline))
              for path in sorted(baseline.rglob('*')) if path.is_file()]
    manifest = []
    for original, copied in files:
        digest = sha(original)
        copied.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(original, copied)
        if sha(copied) != digest or sha(original) != digest:
            raise ValueError('Input changed while preserving baseline')
        manifest.append({'original': str(original), 'path': str(copied.relative_to(output)), 'sha256': digest})
    document = {'schema': 'orca.portrait-r5-run/v1', 'baseline_status': 'USER_ACCEPTED_BOUNDARIES',
                'visual_status': 'PENDING_USER', 'geometry_id': request['geometry_id'],
                'source_sha256': request['source_sha256'], 'evidence_sha256': expected,
                'face_count': 976825, 'lock_count': 6, 'locked_faces': 1869,
                'palettes': report['palettes'], 'checkpoint': str(checkpoint), 'files': manifest,
                'stages': {str(i): ('PASS' if i == 1 else 'NOT_RUN') for i in range(1, 10)},
                'production_enabled': False, 'material_write_authorized': False,
                'missing_historical_material_tree_hashes': True}
    with (output / 'stage-manifest.json').open('x') as stream:
        json.dump(document, stream, indent=2, sort_keys=True)
    print(json.dumps({'stage': 1, 'status': 'PASS', 'files': len(files), 'output': str(output)}))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo', type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--checkpoint', type=Path)
    parser.add_argument('--variants-only', action='store_true')
    args = parser.parse_args()
    if not args.variants_only:
        if args.checkpoint is None:
            parser.error('--checkpoint is required for baseline capture')
        capture(args.repo.resolve(), args.output.resolve(), args.checkpoint.resolve())
    preserve_variants(args.repo.resolve(), args.output.resolve())
