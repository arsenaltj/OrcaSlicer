"""Preserved R5 parent repair and pixel provenance; no recognition or model writes."""
import argparse
from collections import Counter, defaultdict
import json
import importlib.util
from pathlib import Path
import shutil

import cv2
import numpy as np

from beauty_leaf_domain import LeafKey, digest
from portrait_r5_baseline import sha
from portrait_r5_review_audit import oklab, plan_labels
from portrait_r5_visual_review import assignments, pixel_groups, publish, remap


def preserve(r5, output, checkpoint):
    if not (checkpoint / 'COMPLETE.json').is_file():
        raise ValueError('Incomplete source checkpoint')
    manifest = json.loads((r5 / 'stage-manifest.json').read_text())
    colors = json.loads((r5 / 'colors-r2/stage-report.json').read_text())
    paths = {
        'baseline/source.glb': r5 / 'baseline/source.glb',
        'baseline/native.bin': r5 / 'baseline/replay/native.bin',
        'baseline/evidence.json': r5 / 'baseline/replay/evidence.json',
        'baseline/shape-locks.json': r5 / 'brows/shape-locks.json',
        'baseline/native-analysis.json': r5 / 'colors-r2/native-analysis.json',
        'baseline/export.json': r5 / 'baseline/export.json',
        'baseline/review-manifest.json': r5 / 'visual-r1/review-manifest.json',
        'baseline/audit.json': r5 / 'audit-r2/audit.json',
    }
    for view in sorted((r5 / 'baseline/replay/face-views').glob('*.npz')):
        paths['baseline/views/' + view.name] = view
    for variant in colors['variants']:
        count = variant['color_count']
        for name, field in (('portrait-color-plan.json', 'plan_sha256'),
                            ('face-colors.bin', 'face_colors_sha256'),
                            ('subface-colors.json', 'subface_colors_sha256')):
            path = r5 / f'colors-r2/colors-{count}' / name
            if sha(path) != variant[field]:
                raise ValueError('R5 variant drift')
            paths[f'baseline/colors-{count}/{name}'] = path
    if sha(paths['baseline/source.glb']) != manifest['source_sha256'] or \
            sha(paths['baseline/evidence.json']) != manifest['evidence_sha256']:
        raise ValueError('R5 source or evidence drift')
    output.mkdir(parents=True, exist_ok=False)
    files = []
    for relative, original in paths.items():
        expected = sha(original)
        target = output / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(original, target)
        if sha(target) != expected or sha(original) != expected:
            raise ValueError('Input changed during preservation')
        files.append({'original': str(original), 'path': relative, 'sha256': expected})
    locks = json.loads((output / 'baseline/shape-locks.json').read_text())
    if len(locks['locks']) != 6 or sum(len(r['locked_leaves']) for r in locks['locks']) != 5593:
        raise ValueError('Reviewed R5 feature locks changed')
    publish(output / 'stage-manifest.json', {
        'schema': 'orca.portrait-r6-run/v1', 'checkpoint': str(checkpoint),
        'source_sha256': manifest['source_sha256'], 'geometry_id': manifest['geometry_id'],
        'evidence_sha256': manifest['evidence_sha256'], 'face_count': manifest['face_count'],
        'boundary_sha256': digest(locks), 'r5_run': str(r5), 'files': files,
        'production_enabled': False, 'material_tree_changed': False,
        'new_recognition_performed': False, 'visual_status': 'PENDING_USER'})
    print(json.dumps({'stage': 1, 'preserved_files': len(files), 'status': 'PASS'}), flush=True)


def verify(run):
    manifest = json.loads((run / 'stage-manifest.json').read_text())
    for entry in manifest['files']:
        path = run / entry['path']
        if not path.resolve().is_relative_to(run.resolve()) or sha(path) != entry['sha256']:
            raise ValueError('Preserved R5 input drift')
    return manifest


def load_render(run):
    from local_semantic_geometry import read, geometry_fingerprint
    manifest = verify(run)
    delivery = json.loads((Path(manifest['r5_run']) / 'delivery-provenance.json').read_text())
    modules = Path(delivery['entry']['runtime']) / 'resources/tools/ai'
    for entry in delivery['evidence']['runtime_files']:
        if entry['path'].endswith(('local_semantic_render.py', 'local_semantic_raster.dll')) and sha(entry['path']) != entry['sha256']:
            raise ValueError('Validated renderer drift')
    spec = importlib.util.spec_from_file_location('r6_validated_renderer', modules / 'local_semantic_render.py')
    renderer = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(renderer)
    vertices, faces, uv, vertex_colors, materials, material_ids = renderer.load(run / 'baseline/source.glb')
    native_vertices, native_faces, geometry = read(run / 'baseline/native.bin', manifest['source_sha256'])
    if geometry != manifest['geometry_id'] or geometry_fingerprint(vertices, faces) != geometry or \
            not np.array_equal(vertices, native_vertices) or not np.array_equal(faces, native_faces):
        raise ValueError('Canonical source projection drift')
    return renderer, vertices, faces, uv, vertex_colors, materials, material_ids


def render_camera(data, camera):
    renderer, vertices, faces, uv, colors, materials, material_ids = data
    basis, center = np.array(camera['basis']), np.array(camera['center'])
    identity = {k: camera[k] for k in ('geometry_id', 'source_sha256', 'scope', 'view',
                                      'basis', 'center', 'half_height', 'width', 'height')}
    if digest(identity) != camera['render_id'] or camera['width'] != 1024 or camera['height'] != 1024:
        raise ValueError('Camera identity drift')
    projected = renderer.project(vertices, basis, center, camera['half_height'], camera['width'])
    ids, depth, bary = renderer.raster(vertices, faces, basis, center, camera['half_height'],
                                     camera['width'], renderer.double_sided_faces(materials, material_ids))
    source = renderer.shade(faces, uv, colors, materials, material_ids, ids, bary, projected)
    return source, ids, depth, bary, pixel_groups(ids)


def pixel_provenance(ids, bary, roots, children, groups):
    """A palette-looking texture pixel remains SOURCE_TEXTURE, never a slot claim."""
    provenance = np.zeros(ids.shape, dtype=np.uint8)
    depths = np.zeros(ids.shape, dtype=np.uint8)
    paths = np.zeros(ids.shape, dtype=np.uint8)
    flat = provenance.ravel()
    for face in roots:
        if face in groups:
            flat[groups[face]] = 1
    for child in sorted(children, key=lambda r: (r['path']['depth'], r['path']['value'])):
        face, depth, path = child['face_id'], child['path']['depth'], child['path']['value']
        pixels = groups.get(face)
        if pixels is None:
            continue
        key = LeafKey(face, depth, path)
        if not key.valid(len(roots) + int(ids.max()) + 1) or not depth:
            raise ValueError('Invalid provenance child')
        local = bary.reshape(-1, 3)[pixels] @ np.linalg.inv(key.corners())
        selected = pixels[np.min(local, axis=1) >= -1e-7]
        flat[selected] = 2
        depths.ravel()[selected] = depth
        paths.ravel()[selected] = path
    return provenance, depths, paths


def trace(run, output):
    manifest = verify(run)
    data = load_render(run)
    cameras = json.loads((run / 'baseline/review-manifest.json').read_text())['images']
    camera = next(r for r in cameras if r['scope'] == 'face' and r['view'] == 'front')
    source, ids, _, bary, groups = render_camera(data, camera)
    folder = run / 'baseline/colors-5'
    roots = assignments(folder / 'face-colors.bin', manifest['face_count'])
    children = json.loads((folder / 'subface-colors.json').read_text())
    plan = json.loads((folder / 'portrait-color-plan.json').read_text())
    current = remap(source, ids, bary, roots, children, groups, manifest['face_count'])
    labels = remap(np.zeros_like(source), ids, bary, *plan_labels(plan, manifest['face_count']),
                   groups, manifest['face_count'])[:, :, 0]
    lip_rgb = np.rint(np.array(next(s['rgb'] for s in plan['palette'] if s['uid'] == 'portrait-lips')) * 255)
    pink = (ids >= 0) & (labels == 0) & np.all(current == lip_rgb, axis=2)
    if int(pink.sum()) != 22748:
        raise ValueError('Fixed R5 pink-pixel baseline differs from the recorded 22748')
    lab = oklab(current)
    warm = (ids >= 0) & (labels == 0) & (lab[:, :, 0] < .75) & (lab[:, :, 1] > .005) & (lab[:, :, 2] > .01)
    provenance, depths, paths = pixel_provenance(ids, bary, roots, children, groups)
    native = json.loads((run / 'baseline/native-analysis.json').read_text())
    native_labels = np.array([int(v, 16) for v in native['labels']], dtype=np.uint8)
    confidences = np.frombuffer(bytes.fromhex(native['confidence_f32']), dtype='<f4')
    mixed = {r[0] for r in native['subfaces']} | {r['face_id'] for r in children}
    names = ('SOURCE_TEXTURE', 'OLD_ROOT_SLOT', 'OLD_SUBFACE_COLOR', 'MANUAL_COLOR')
    records = []
    for kind, mask in (('PINK_SLOT_RGB', pink), ('WARM_DARK_DIAGNOSTIC', warm)):
        _, component_ids, stats, _ = cv2.connectedComponentsWithStats(mask.astype(np.uint8), connectivity=8)
        for component in range(1, len(stats)):
            component_mask = component_ids == component
            pixel_indices = np.flatnonzero(component_mask)
            records_by_leaf = defaultdict(list)
            for pixel in pixel_indices:
                key = (int(ids.ravel()[pixel]), int(depths.ravel()[pixel]), int(paths.ravel()[pixel]),
                       int(provenance.ravel()[pixel]))
                records_by_leaf[key].append(int(pixel))
            leaves = []
            for (face, depth, path, origin), pixels in sorted(records_by_leaf.items()):
                weights = bary.reshape(-1, 3)[pixels]
                uv = weights @ data[3][data[2][face]]
                reasons = []
                if face in mixed:
                    reasons.append('MIXED_ROOT_SKIPPED_OR_UNEVIDENCED_SIBLING')
                if float(confidences[face]) < .7:
                    reasons.append('NATIVE_CONFIDENCE_BELOW_PLAN_GATE')
                if face not in roots:
                    reasons.append('NO_PREVIOUS_ROOT_SLOT')
                leaves.append({'leaf': [face, depth, path], 'pixels': len(pixels), 'source': names[origin],
                               'native_label': int(native_labels[face]), 'native_confidence': float(confidences[face]),
                               'uv_min': uv.min(0).tolist(), 'uv_max': uv.max(0).tolist(),
                               'barycentric_min': weights.min(0).tolist(), 'barycentric_max': weights.max(0).tolist(),
                               'plan_omission_reasons': reasons or ['OUTSIDE_R5_PLANNED_PARENT_COMPONENT']})
            records.append({'kind': kind, 'component': component, 'pixels': int(stats[component, cv2.CC_STAT_AREA]),
                            'bbox': stats[component, :4].tolist(), 'leaves': leaves})
    output.mkdir(parents=True, exist_ok=False)
    marked = current.copy()
    marked[pink] = [0, 220, 220]
    marked[warm & ~pink] = [230, 180, 0]
    from PIL import Image
    Image.fromarray(marked).save(output / 'residual-locations.png')
    for name, rgb in (('source.png', source), ('r5.png', current)):
        Image.fromarray(rgb).save(output / name)
    report = {'schema': 'orca.portrait-residual-audit/v1', 'source_sha256': manifest['source_sha256'],
              'geometry_id': manifest['geometry_id'], 'boundary_sha256': manifest['boundary_sha256'],
              'render_id': camera['render_id'], 'pink_pixels': int(pink.sum()),
              'pink_provenance': {names[k]: int((pink & (provenance == k)).sum()) for k in range(4)},
              'warm_diagnostic_pixels': int(warm.sum()), 'components': records,
              'note': 'Warm/dark RGB identifies audit samples, not mistaken ownership or repair authorization.',
              'new_recognition_performed': False, 'production_enabled': False}
    publish(output / 'audit.json', report)
    print(json.dumps({k: report[k] for k in ('pink_pixels', 'pink_provenance', 'warm_diagnostic_pixels')}), flush=True)


def ownership(run, output):
    from local_semantic_geometry import read
    from portrait_surface_ownership import build
    manifest = verify(run)
    vertices, faces, geometry = read(run / 'baseline/native.bin', manifest['source_sha256'])
    if geometry != manifest['geometry_id']:
        raise ValueError('Preserved geometry changed')
    locks = json.loads((run / 'baseline/shape-locks.json').read_text())
    evidence = json.loads((run / 'baseline/evidence.json').read_text())
    native = json.loads((run / 'baseline/native-analysis.json').read_text())
    children = [json.loads((run / f'baseline/colors-{count}/subface-colors.json').read_text())
                for count in (3, 4, 5, 6)]
    document, report = build(vertices, faces, evidence, locks, run / 'baseline/views', native, children)
    output.mkdir(parents=True, exist_ok=False)
    publish(output / 'surface-ownership.json', document)
    report.update(ownership_sha256=sha(output / 'surface-ownership.json'),
                  boundary_sha256=manifest['boundary_sha256'], visual_status='PENDING_USER')
    publish(output / 'stage-report.json', report)
    print(json.dumps({k: v for k, v in report.items() if k not in ('explicit_conflict_roots', 'scope_face_ids')}), flush=True)


def trees(run, colors, output):
    manifest = verify(run)
    stage = json.loads((colors/'stage-report.json').read_text())
    output.mkdir(parents=True, exist_ok=False)
    entries = []
    for row in stage['variants']:
        count = row['color_count']
        folder = colors/f'colors-{count}'
        for name, field in (('portrait-color-plan.json','plan_sha256'),('face-colors.bin','face_colors_sha256'),
                            ('subface-colors.json','subface_colors_sha256')):
            if sha(folder/name) != row[field]:
                raise ValueError('Draft colors changed')
        plan = json.loads((folder/'portrait-color-plan.json').read_text())
        palette = [tuple(np.asarray(s['rgb'],dtype=np.float32)) for s in plan['palette']]
        destination = output/f'colors-{count}'
        destination.mkdir()
        slots = np.full(manifest['face_count'],255,dtype=np.uint8)
        for face, rgb in assignments(folder/'face-colors.bin', manifest['face_count']).items():
            slots[face] = palette.index(tuple(np.asarray(rgb,dtype=np.float32)))
        with (destination/'material-faces.u8').open('xb') as stream:
            slots.tofile(stream)
        children = []
        for child in json.loads((folder/'subface-colors.json').read_text()):
            slot = palette.index(tuple(np.asarray(child['color'],dtype=np.float32)))
            children.append(dict(source_face_id=child['face_id'],depth=child['path']['depth'],
                                 path=child['path']['value'],slot=slot,slot_uid=plan['palette'][slot]['uid']))
        publish(destination/'material-subfaces.json',children)
        tree = dict(schema='orca.offline-portrait-tree/v1',iteration='R6',face_count=manifest['face_count'],
                    geometry_id=manifest['geometry_id'],source_sha256=manifest['source_sha256'],
                    boundary_sha256=manifest['boundary_sha256'],palette=plan['palette'],
                    ownership_ref=plan['ownership_ref'],color_plan_sha256=row['plan_sha256'],
                    material_faces_sha256=sha(destination/'material-faces.u8'),
                    material_subfaces_sha256=sha(destination/'material-subfaces.json'),
                    source_appearance_slot=255,material_write_authorized=False,production_enabled=False)
        publish(destination/'material-tree.json',tree)
        entries.append(dict(color_count=count,tree_sha256=sha(destination/'material-tree.json')))
    publish(output/'manifest.json',dict(variants=entries,production_enabled=False))
    print(json.dumps(dict(candidate_trees=len(entries),status='OFFLINE_VISUAL_PENDING')),flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('stage', choices=('preserve', 'trace', 'ownership', 'trees'))
    parser.add_argument('--run', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--checkpoint', type=Path)
    parser.add_argument('--colors', type=Path)
    args = parser.parse_args()
    if args.stage == 'preserve':
        if args.checkpoint is None:
            parser.error('--checkpoint is required')
        preserve(args.run.resolve(), args.output.resolve(), args.checkpoint.resolve())
    elif args.stage == 'trace':
        trace(args.run.resolve(), args.output.resolve())
    elif args.stage == 'ownership':
        ownership(args.run.resolve(), args.output.resolve())
    else:
        if args.colors is None:
            parser.error('--colors is required')
        trees(args.run.resolve(),args.colors.resolve(),args.output.resolve())
