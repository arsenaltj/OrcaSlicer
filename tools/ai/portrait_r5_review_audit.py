"""Measure preserved R5 drafts at native review resolution without changing them."""
import argparse
import json
from pathlib import Path
import sys

import cv2
import numpy as np

from beauty_leaf_domain import LeafKey, digest
from portrait_r5_baseline import sha
from portrait_r5_visual_review import assignments, pixel_groups, publish, remap, sheet, lit

FEATURES = ('lb', 'rb', 'le', 're', 'iris', 'ulip', 'llip')
LABELS = ('unknown', 'cloth', 'hair', 'face', 'skin') + FEATURES


def oklab(rgb):
    rgb = np.asarray(rgb, dtype=float) / 255
    linear = np.where(rgb <= .04045, rgb / 12.92, ((rgb + .055) / 1.055) ** 2.4)
    lms = linear @ np.array(((.4122214708, .2119034982, .0883024619),
                             (.5363325363, .6806995451, .2817188376),
                             (.0514459929, .1073969566, .6299787005)))
    return np.cbrt(lms) @ np.array(((.2104542553, 1.9779984951, .0259040371),
                                  (.793617785, -2.428592205, .7827717662),
                                  (-.0040720468, .4505937099, -.808675766)))


def plan_labels(plan, face_count):
    roots, children = {}, {}
    for item in plan['assignments']:
        label = LABELS.index(item['label'])
        for entry in item['leaves']:
            key = LeafKey(*entry)
            if not key.valid(face_count):
                raise ValueError('Invalid planned leaf')
            if key in children and children[key] != label:
                raise ValueError('Conflicting planned leaf labels')
            children[key] = label
    for key, label in children.items():
        if not key.depth:
            roots[key.source_face_id] = [label / 255] * 3
    return roots, [{'face_id': k.source_face_id, 'path': {'depth': k.depth, 'value': k.path},
                    'color': [v / 255] * 3} for k, v in sorted(children.items()) if k.depth]


def summarize(source, old, current, ids, labels, target, palette):
    valid = ids >= 0
    a, b, original = oklab(old), oklab(current), oklab(source)
    delta = np.linalg.norm(a - b, axis=2)
    error_before = np.linalg.norm(a - original, axis=2)
    error_after = np.linalg.norm(b - original, axis=2)
    scope = valid & target
    non_target = valid & ~target
    skin = scope & np.isin(labels, [LABELS.index('face'), LABELS.index('skin')])
    skin_rgb = np.rint(np.array(next(s['rgb'] for s in palette if s['uid'] == 'portrait-skin')) * 255)
    skin_pixels = np.all(current == skin_rgb, axis=2)
    fragments, usage = {}, {}
    feature = np.isin(labels, [LABELS.index(label) for label in FEATURES])
    for slot in palette:
        rgb = np.rint(np.asarray(slot['rgb']) * 255)
        mask = scope & np.all(current == rgb, axis=2)
        usage[slot['uid']] = int(mask.sum())
        ordinary = (mask & ~feature).astype(np.uint8)
        _, _, stats, _ = cv2.connectedComponentsWithStats(ordinary, connectivity=8)
        sizes = stats[1:, cv2.CC_STAT_AREA]
        fragments[slot['uid']] = {'components': len(sizes), 'under_32_pixels': int((sizes < 32).sum()),
                                  'small_component_pixels': int(sizes[sizes < 32].sum())}
    contrasts = {}
    for label in FEATURES:
        mask = scope & (labels == LABELS.index(label))
        nearby_skin = skin & (cv2.dilate(mask.astype(np.uint8), np.ones((5, 5), np.uint8)) > 0)
        if np.any(mask) and np.any(nearby_skin):
            contrasts[label] = {'visible_pixels': int(mask.sum()), 'skin_reference_pixels': int(nearby_skin.sum()),
                                'oklab_contrast': float(np.linalg.norm(b[mask].mean(axis=0) - b[nearby_skin].mean(axis=0)))}
        else:
            contrasts[label] = {'visible_pixels': int(mask.sum()), 'skin_reference_pixels': int(nearby_skin.sum()),
                                'oklab_contrast': None}
    lips = next((s for s in palette if s['uid'] == 'portrait-lips'), None)
    lip_pixels = np.zeros_like(valid)
    if lips:
        lip_rgb = np.rint(np.asarray(lips['rgb']) * 255)
        lip_pixels = valid & np.all(current == lip_rgb, axis=2)
    outside_lips = lip_pixels & ~np.isin(labels, [LABELS.index('ulip'), LABELS.index('llip')])
    unknown_lips = outside_lips & (labels == 0)
    return {'visible_pixels': int(valid.sum()), 'target_pixels': int(scope.sum()),
            'perceptible_changed_pixels_delta_e_003': int((scope & (delta >= .03)).sum()),
            'changed_visible_pixels_rgb_gt_1': int((valid & (np.max(np.abs(current.astype(int) - old.astype(int)), axis=2) > 1)).sum()),
            'non_target_changed_pixels': int((non_target & np.any(current != old, axis=2)).sum()),
            'mean_target_oklab_change': float(delta[scope].mean()) if scope.any() else None,
            'source_error_before': float(error_before[scope].mean()) if scope.any() else None,
            'source_error_after': float(error_after[scope].mean()) if scope.any() else None,
            'source_error_reduction': float((error_before[scope] - error_after[scope]).mean()) if scope.any() else None,
            'skin_pixels': int(skin.sum()), 'skin_slot_fraction': float(skin_pixels[skin].mean()) if skin.any() else None,
            'lip_slot_pixels_outside_locked_lips': int(outside_lips.sum()),
            'lip_slot_outside_lips_with_unknown_assignment': int(unknown_lips.sum()),
            'palette_visible_pixels': usage, 'ordinary_pixel_components': fragments, 'feature_contrast': contrasts}


def boundary_metrics(run, locks, face_areas):
    old = json.loads((run / 'baseline/replay/evidence.json').read_text())['shape_details']
    old = {(r['subject_id'], r['label']): r for r in old if r['accepted_faces']}
    results = []
    rows = {}
    for stage in ('eyes-r4', 'brows'):
        doc = json.loads((run / stage / 'stage-report.json').read_text())
        rows.update({r['label']: r for r in doc['details']})
    for lock in locks['locks']:
        previous = old[lock['subject_id'], lock['label']]
        before, after = set(previous['accepted_faces']), set(lock['locked_faces'])
        details = rows.get(lock['label'], {})
        refined = [r['contour_error_pixels_1024'] for r in details.get('faces', []) if r['status'] == 'BOUNDARY_REFINED']
        results.append({'label': lock['label'], 'r4_root_count': len(before), 'r5_root_count': len(after),
                        'added_roots': sorted(after - before), 'removed_roots': sorted(before - after),
                        'r4_area_mm2': float(face_areas[list(before)].sum()),
                        'r5_area_mm2': sum(float(face_areas[f]) / 4 ** d for f, d, p in lock['locked_leaves']),
                        'leaf_count': len(lock['locked_leaves']), 'nested_leaf_count': len(lock['nested_leaves']),
                        'refined_max_contour_error_pixels_1024': max(refined, default=None),
                        'local_fallback_counts': {k: v for k, v in details.get('status_counts', {}).items() if k != 'BOUNDARY_REFINED'},
                        'contour_metric_scope': 'Newly refined leaves only; local R4 fallbacks may exceed one pixel.'})
    return results


def audit(run, visual, output, renderer_dir):
    if output.exists():
        raise ValueError('Audit output already exists')
    review = json.loads((visual / 'review-manifest.json').read_text())
    manifest = json.loads((run / 'stage-manifest.json').read_text())
    for row in manifest['files'] + json.loads((run / 'stage-baseline-variants.json').read_text())['files']:
        if sha(run / row['path']) != row['sha256']:
            raise ValueError('R4 input drift')
    for row in review['images']:
        if sha(visual / row['path']) != row['sha256']:
            raise ValueError('Visual artifact drift')
    locks = json.loads((run / 'brows/shape-locks.json').read_text())
    if digest(locks) != review['boundary_sha256']:
        raise ValueError('Boundary drift')
    sys.path.insert(0, str(renderer_dir))
    import local_semantic_render as renderer
    from local_semantic_geometry import geometry_fingerprint, read
    vertices, faces, uv, vertex_colors, materials, material_ids = renderer.load(run / 'baseline/source.glb')
    native_vertices, native_faces, geometry = read(run / 'baseline/replay/native.bin', manifest['source_sha256'])
    if geometry != review['geometry_id'] or geometry_fingerprint(vertices, faces) != geometry or \
            not np.array_equal(faces, native_faces) or not np.array_equal(vertices, native_vertices):
        raise ValueError('Canonical render mapping drift')
    points = vertices[faces].astype(float)
    normals = np.cross(points[:, 1] - points[:, 0], points[:, 2] - points[:, 0])
    lengths = np.linalg.norm(normals, axis=1)
    face_areas = lengths / 2
    normals /= np.maximum(lengths, 1e-15)[:, None]
    variants, plans, label_maps = {}, {}, {}
    touched = {f for r in locks['locks'] for f in r['locked_faces']}
    evidence = json.loads((run / 'baseline/replay/evidence.json').read_text())
    touched.update(f for r in evidence['shape_details'] for f in r['accepted_faces'])
    for count in (3, 4, 5, 6):
        plan_path = run / f'colors-r2/colors-{count}/portrait-color-plan.json'
        plans[count] = json.loads(plan_path.read_text())
        if plans[count]['boundary_sha256'] != review['boundary_sha256']:
            raise ValueError('Plan boundary drift')
        label_maps[count] = plan_labels(plans[count], len(faces))
        variants[count] = {}
        for mode in ('R4', 'R5'):
            directory = visual / f'colors-{count}' / mode
            tree = json.loads((directory / 'material-tree.json').read_text())
            if tree['source_sha256'] != manifest['source_sha256'] or tree['geometry_id'] != geometry or \
                    tree['face_count'] != len(faces) or \
                    [{k: slot[k] for k in ('uid', 'rgb')} for slot in tree['palette']] != plans[count]['palette']:
                raise ValueError('Candidate tree identity drift')
            for name, field in (('face-colors.bin', 'face_colors_sha256'), ('subface-colors.json', 'subface_colors_sha256')):
                if sha(directory / name) != tree[field]:
                    raise ValueError('Candidate assignment drift')
            variants[count][mode] = (assignments(directory / 'face-colors.bin', len(faces)),
                                      json.loads((directory / 'subface-colors.json').read_text()))
    output.mkdir(parents=True)
    cameras = {(r['scope'], r['view']): r for r in review['images']}
    metrics, images = [], []
    for (scope, name), row in cameras.items():
        camera, center = np.array(row['basis']), np.array(row['center'])
        extent, size = row['half_height'], row['width']
        identity = {k: row[k] for k in ('geometry_id', 'source_sha256', 'scope', 'view', 'basis', 'center', 'half_height', 'width', 'height')}
        if size != 1024 or row['height'] != size or digest(identity) != row['render_id']:
            raise ValueError('Render camera identity drift')
        projected = renderer.project(vertices, camera, center, extent, size)
        ids, _, bary = renderer.raster(vertices, faces, camera, center, extent, size,
                                       renderer.double_sided_faces(materials, material_ids))
        source = renderer.shade(faces, uv, vertex_colors, materials, material_ids, ids, bary, projected)
        groups = pixel_groups(ids)
        current_images = [source]
        for count in (3, 4, 5, 6):
            old = remap(source, ids, bary, *variants[count]['R4'], groups, len(faces))
            current = remap(source, ids, bary, *variants[count]['R5'], groups, len(faces))
            labels = remap(np.zeros_like(source), ids, bary, *label_maps[count], groups, len(faces))[:, :, 0]
            target_roots = dict(label_maps[count][0])
            target_roots.update({f: [1, 1, 1] for f in touched})
            target = remap(np.zeros_like(source), ids, bary, target_roots, label_maps[count][1], groups, len(faces))[:, :, 0] > 0
            metrics.append({'scope': scope, 'view': name, 'color_count': count, 'render_id': row['render_id'],
                            **summarize(source, old, current, ids, labels, target, plans[count]['palette'])})
            current_images.append(current)
        for lighting in ('flat', 'lit'):
            values = current_images if lighting == 'flat' else [lit(rgb, ids, normals, camera) for rgb in current_images]
            path = f'{scope}-{name}-{lighting}.png'
            sheet(values, ['Source', 'R5 3 colors', 'R5 4 colors', 'R5 5 colors', 'R5 6 colors'], output / path,
                  size=512 if scope == 'face' else 384)
            images.append({'path': path, 'sha256': sha(output / path), 'render_id': row['render_id'], 'lighting': lighting})
        print(json.dumps({'audited': scope + '-' + name}), flush=True)
    budgets = []
    for count, plan in plans.items():
        skin = [r for r in plan['assignments'] if r['label'] in ('skin', 'face')]
        total = sum(r['area'] for r in skin)
        budgets.append({'color_count': count, 'plan_sha256': sha(run / f'colors-r2/colors-{count}/portrait-color-plan.json'),
                        'skin_slot_area_fraction': sum(r['area'] for r in skin if r['applied_uid'] == 'portrait-skin') / total if total else None,
                        'component_count': len(plan['assignments']),
                        'ordinary_components_under_05_mm2': sum(r['label'] not in FEATURES and r['area'] < .5 for r in plan['assignments']),
                        'unused_decision_slots': plan['unused_slots'], 'initial_terms': plan['initial_terms'], 'final_terms': plan['terms'],
                        'fixed_weights': plan['policy']['weights']})
    report = {'schema': 'orca.r5-review-audit/v1', 'visual_status': 'PENDING_USER',
              'source_sha256': manifest['source_sha256'], 'geometry_id': geometry,
              'boundary_sha256': digest(locks), 'review_manifest_sha256': sha(visual / 'review-manifest.json'),
              'boundaries': boundary_metrics(run, locks, face_areas), 'budgets': budgets, 'metrics': metrics, 'images': images,
              'non_target_pixels_unchanged': all(r['non_target_changed_pixels'] == 0 for r in metrics),
              'production_enabled': False, 'original_material_tree_changed': False,
              'metric_notes': ['Source Oklab distance is a reference, not a visual acceptance score.',
                               'Skin slot fraction covers confirmed skin assignments only, not every visible face pixel.',
                               'Pixel components are view-dependent 8-connected masks; semantic plan components are separate.',
                               'An unused decision slot can remain in preserved legacy children.',
                               'Offline fixed diffuse lit is not software lighting.']}
    publish(output / 'audit.json', report)
    page = ['<!doctype html><meta charset="utf-8"><title>R5 budgets side by side</title>',
            '<style>body{font:15px system-ui;margin:20px;background:#f4f5f6;color:#202124}img{max-width:100%;height:auto}</style>',
            '<h1>R5 3 / 4 / 5 / 6 colors</h1><p>Source | 3 | 4 | 5 | 6. Visual acceptance pending. Lit is offline diagnostic.</p>',
            '<p><a href="../visual-r1/index.html">R4 / boundary / color ablations</a> | <a href="audit.json">Audit metrics</a></p>']
    for item in images:
        page.append(f'<details {"open" if item["path"] == "face-front-flat.png" else ""}><summary>{item["path"]}</summary><img loading="lazy" src="{item["path"]}"></details>')
    with (output / 'index.html').open('x', encoding='utf-8') as stream:
        stream.write('\n'.join(page))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run', type=Path, required=True)
    parser.add_argument('--visual', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--renderer', type=Path, required=True)
    args = parser.parse_args()
    audit(args.run.resolve(), args.visual.resolve(), args.output.resolve(), args.renderer.resolve())
