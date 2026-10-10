"""Publish source-bound R5 ablations and a local visual review, without inference."""
import argparse
from collections import Counter
import copy
import hashlib
import html
import json
from pathlib import Path
import shutil
import sys

import numpy as np
from PIL import Image, ImageDraw

from beauty_leaf_domain import LeafKey, digest
from portrait_r5_baseline import sha

DTYPE = np.dtype([('face', '<u4'), ('rgb', '<f4', (3,))])
MODES = ('R4', 'boundary-only', 'color-only', 'R5')


def publish(path, value):
    with path.open('x', encoding='utf-8') as stream:
        json.dump(value, stream, sort_keys=True, separators=(',', ':'), allow_nan=False)


def assignments(path, count):
    if path.stat().st_size % 16:
        raise ValueError('Truncated color assignments')
    rows = np.fromfile(path, dtype=DTYPE)
    if len(rows) and (np.any(np.diff(rows['face'].astype(np.int64)) <= 0) or rows['face'][-1] >= count):
        raise ValueError('Unordered or out-of-range root colors')
    if not np.isfinite(rows['rgb']).all() or np.any((rows['rgb'] < 0) | (rows['rgb'] > 1)):
        raise ValueError('Invalid root RGB')
    return {int(row['face']): row['rgb'].tolist() for row in rows}


def write_assignments(path, roots):
    rows = np.empty(len(roots), dtype=DTYPE)
    for i, (face, rgb) in enumerate(sorted(roots.items())):
        rows[i] = (face, rgb)
    with path.open('xb') as stream:
        rows.tofile(stream)


def child_key(child):
    return LeafKey(child['face_id'], child['path']['depth'], child['path']['value'])


def child(key, rgb):
    return {'face_id': key.source_face_id, 'path': {'depth': key.depth, 'value': key.path},
            'color': rgb, 'confidence': 1.0}


def ablations(old_roots, old_children, roots, children, r4_locks, r5_locks, palette):
    colors = {s['uid']: s['rgb'] for s in palette}
    skin = colors['portrait-skin']
    boundary_roots, boundary_children = dict(old_roots), copy.deepcopy(old_children)
    color_roots, color_children = dict(roots), copy.deepcopy(children)
    touched = {f for lock in r4_locks for f in lock['locked_faces']} | {
        f for lock in r5_locks for f in lock['locked_faces']}
    boundary_children = [c for c in boundary_children if c['face_id'] not in touched]
    color_children = [c for c in color_children if c['face_id'] not in touched]
    for face in touched:
        boundary_roots[face] = skin
        if face in old_roots:
            color_roots[face] = old_roots[face]
        else:
            color_roots.pop(face, None)
    old_by_label = {lock['label']: lock for lock in r4_locks}
    for lock in r5_locks:
        previous = old_by_label[lock['label']]
        majority = Counter(tuple(old_roots[f]) for f in previous['locked_faces'] if f in old_roots).most_common(1)
        if not majority:
            raise ValueError('R4 detail has no color underlayer')
        rgb = list(majority[0][0])
        nested = {LeafKey(*k) for k in lock['nested_leaves']}
        for key in map(lambda k: LeafKey(*k), lock['locked_leaves']):
            target = colors['portrait-dark'] if key in nested else old_roots.get(key.source_face_id, rgb)
            if key.depth:
                boundary_children.append(child(key, target))
            else:
                boundary_roots[key.source_face_id] = target
    for lock in r4_locks:
        label = lock['label']
        role = 'portrait-dark' if label in ('lb', 'rb') else 'portrait-light' if label in ('le', 're') else \
            'portrait-dark' if len(palette) == 3 else 'portrait-lips'
        nested = set(lock['nested_faces'])
        for face in lock['locked_faces']:
            color_roots[face] = colors['portrait-dark' if face in nested else role]
    old_locked = {f for lock in r4_locks for f in lock['locked_faces']}
    color_children += [c for c in old_children if c['face_id'] in touched - old_locked]
    for values in (boundary_children, color_children):
        values.sort(key=child_key)
    return {'R4': (old_roots, old_children), 'boundary-only': (boundary_roots, boundary_children),
            'color-only': (color_roots, color_children), 'R5': (roots, children)}


def pixel_groups(ids):
    flat = ids.ravel()
    order = np.argsort(flat, kind='stable')
    faces, starts, counts = np.unique(flat[order], return_index=True, return_counts=True)
    return {int(face): order[start:start+count] for face, start, count in zip(faces, starts, counts) if face >= 0}


def remap(source, ids, bary, roots, children, groups, face_count):
    result = source.copy()
    colors = np.zeros((face_count, 3), dtype=np.uint8)
    present = np.zeros(face_count, dtype=bool)
    indices = np.fromiter(roots, dtype=np.int64)
    if len(indices):
        colors[indices] = np.rint(np.asarray(list(roots.values())) * 255).astype(np.uint8)
    present[indices] = True
    valid = ids >= 0
    target = valid.copy()
    target[valid] = present[ids[valid]]
    result[target] = colors[ids[target]]
    for entry in sorted(children, key=child_key):
        key = child_key(entry)
        if not key.valid(face_count) or not key.depth:
            raise ValueError('Invalid visual child identity')
        pixels = groups.get(key.source_face_id)
        if pixels is None:
            continue
        local = bary.reshape(-1, 3)[pixels] @ np.linalg.inv(key.corners())
        inside = pixels[np.min(local, axis=1) >= -1e-7]
        result.reshape(-1, 3)[inside] = np.rint(np.asarray(entry['color']) * 255).astype(np.uint8)
    return result


def basis(direction):
    forward = np.asarray(direction, dtype=float)
    forward /= np.linalg.norm(forward)
    hint = np.array((0, 1, 0) if abs(forward[2]) > .5 else (0, 0, 1), dtype=float)
    right = np.cross(hint, forward)
    right /= np.linalg.norm(right)
    return np.stack((right, np.cross(forward, right), forward))


def lit(rgb, ids, normals, camera):
    light = -camera[2]*.7 + camera[1]*.6 + camera[0]*.3
    light /= np.linalg.norm(light)
    result = rgb.copy()
    mask = ids >= 0
    factor = .55 + .45*np.maximum(0, normals[ids[mask]] @ light)
    result[mask] = np.rint(rgb[mask]*factor[:, None]).astype(np.uint8)
    return result


def sheet(images, labels, path, size=384):
    canvas = Image.new('RGB', (len(images)*size, size+28), 'white')
    draw = ImageDraw.Draw(canvas)
    for i, (image, label) in enumerate(zip(images, labels)):
        canvas.paste(Image.fromarray(image).resize((size, size), Image.Resampling.LANCZOS), (i*size, 28))
        draw.text((i*size+8, 8), label, fill='black')
    canvas.save(path)


def main(args):
    run, output = args.run.resolve(), args.output.resolve()
    if output.exists():
        raise ValueError('Visual run already exists')
    manifest = json.loads((run/'stage-manifest.json').read_text())
    for entry in manifest['files'] + json.loads((run/'stage-baseline-variants.json').read_text())['files']:
        if sha(run/entry['path']) != entry['sha256']:
            raise ValueError('Preserved R4 input changed')
    baseline = json.loads((run/'baseline/export.json').read_text())
    report = json.loads((run/'colors-r2/stage-report.json').read_text())
    locks_file = run/'brows/shape-locks.json'
    locks = json.loads(locks_file.read_text())
    if digest(locks) != report['boundary_sha256']:
        raise ValueError('Color boundary identity drift')
    old_file = Path(baseline['shape_locks_file'])
    if not old_file.is_absolute():
        old_file = Path(baseline['source_glb']).parents[0] / old_file if (Path(baseline['source_glb']).parents[0]/old_file).exists() else \
            run.parents[0]/'portrait-export-r4-b'/old_file
    r4_locks = json.loads(old_file.read_text())['locks']
    sys.path.insert(0, str(args.renderer.resolve()))
    import local_semantic_render as renderer
    from local_semantic_geometry import geometry_fingerprint, read
    vertices, faces, uv, vertex_colors, materials, material_ids = renderer.load(run/'baseline/source.glb')
    native_vertices, native_faces, geometry = read(run/'baseline/replay/native.bin', manifest['source_sha256'])
    if geometry != manifest['geometry_id'] or geometry_fingerprint(vertices, faces) != geometry or \
            not np.array_equal(faces, native_faces) or not np.array_equal(vertices, native_vertices):
        raise ValueError('Render face/corner identity differs from the canonical surface')
    face_count = len(faces)
    output.mkdir(parents=True)
    variants, assignments_report = {}, []
    for card, row in zip(baseline['palettes'], report['variants']):
        count = card['color_count']
        directory = run/f'colors-r2/colors-{count}'
        for name, field in (('face-colors.bin', 'face_colors_sha256'), ('subface-colors.json', 'subface_colors_sha256')):
            if sha(directory/name) != row[field]:
                raise ValueError('Candidate color assignments changed')
        old_roots = assignments(run/f'baseline/variants/colors-{count}-merged.overrides.bin', face_count)
        old_children = json.loads((run/f'baseline/variants/colors-{count}-merged.subfaces.json').read_text())
        current = ablations(old_roots, old_children, assignments(directory/'face-colors.bin', face_count),
                            json.loads((directory/'subface-colors.json').read_text()), r4_locks, locks['locks'], card['palette'])
        variants[count] = current
        for mode, (roots, children) in current.items():
            folder = output/f'colors-{count}'/mode
            folder.mkdir(parents=True)
            write_assignments(folder/'face-colors.bin', roots)
            publish(folder/'subface-colors.json', children)
            publish(folder/'material-tree.json', {'schema': 'orca.offline-portrait-tree/v1',
                'source_sha256': manifest['source_sha256'], 'geometry_id': geometry, 'face_count': face_count,
                'palette': card['palette'], 'mode': mode, 'boundary_sha256': digest(locks) if mode in ('R5', 'boundary-only') else sha(old_file),
                'face_colors_sha256': sha(folder/'face-colors.bin'), 'subface_colors_sha256': sha(folder/'subface-colors.json'),
                'source_appearance_for_unassigned_faces': True, 'production_enabled': False})
        assignments_report.append({'color_count': count, 'unused_decision_slots': row['unused_decision_slots'],
                                   'component_count': row['component_count'], 'changed_roots': row['changed_roots']})
    points = vertices[faces]
    normals = np.cross(points[:, 1]-points[:, 0], points[:, 2]-points[:, 0]).astype(float)
    normals /= np.maximum(np.linalg.norm(normals, axis=1)[:, None], 1e-15)
    center = (vertices.min(axis=0).astype(float)+vertices.max(axis=0).astype(float))/2
    radius = float(np.max(np.linalg.norm(vertices-center, axis=1)))*1.05
    feature_faces = sorted({f for lock in r4_locks for f in lock['locked_faces']})
    face_points = vertices[faces[feature_faces]].reshape(-1, 3)
    face_center = (face_points.min(axis=0)+face_points.max(axis=0)).astype(float)/2
    face_radius = float(np.max(np.linalg.norm(face_points-face_center, axis=1)))*1.55
    views = {'front': (1,0,0), 'back': (-1,0,0), 'left': (0,-1,0), 'right': (0,1,0), 'top': (0,0,1), 'bottom': (0,0,-1)}
    crop_views = {'front': (1,0,0), 'front-left': (1,-.6,0), 'front-right': (1,.6,0), 'side': (0,1,0)}
    measures, images = [], []
    for scope, cameras in (('body', views), ('face', crop_views)):
        for name, direction in cameras.items():
            camera = basis(direction)
            origin, extent = (center, radius) if scope == 'body' else (face_center, face_radius)
            projected = renderer.project(vertices, camera, origin, extent, args.size)
            ids, _, bary = renderer.raster(vertices, faces, camera, origin, extent, args.size,
                                           renderer.double_sided_faces(materials, material_ids))
            source = renderer.shade(faces, uv, vertex_colors, materials, material_ids, ids, bary, projected)
            groups = pixel_groups(ids)
            identity = {'geometry_id': geometry, 'source_sha256': manifest['source_sha256'], 'scope': scope,
                        'view': name, 'basis': camera.tolist(), 'center': origin.tolist(), 'half_height': extent,
                        'width': args.size, 'height': args.size}
            render_id = digest(identity)
            for count, current in variants.items():
                flat = [source]+[remap(source, ids, bary, *current[mode], groups, face_count) for mode in MODES]
                for lighting in ('flat', 'lit'):
                    values = flat if lighting == 'flat' else [lit(rgb, ids, normals, camera) for rgb in flat]
                    relative = f'colors-{count}/{scope}-{name}-{lighting}.png'
                    sheet(values, ['Source', 'R4', 'Boundary only', 'Color only', 'R5'], output/relative,
                          size=512 if scope == 'face' else 384)
                    images.append({'path': relative, 'sha256': sha(output/relative), 'render_id': render_id,
                                   'color_count': count, 'lighting': lighting, **identity})
                difference = np.max(np.abs(flat[-1].astype(int)-flat[1].astype(int)), axis=2)
                measures.append({'scope': scope, 'view': name, 'color_count': count,
                    'visible_pixels': int((ids>=0).sum()), 'changed_visible_pixels': int((difference>1).sum()),
                    'mean_flat_rgb_delta': float(difference[ids>=0].mean()) if np.any(ids>=0) else 0,
                    'render_id': render_id})
                if scope == 'face' and name == 'front':
                    Image.fromarray(flat[-1]).save(output/f'colors-{count}/front-r5-unlabeled.png')
            print(json.dumps({'rendered': scope+'-'+name, 'render_id': render_id}), flush=True)
    publish(output/'review-manifest.json', {'schema': 'orca.r5-visual-review/v1', 'code_status': 'PASS',
        'visual_status': 'PENDING_USER', 'source_sha256': manifest['source_sha256'], 'geometry_id': geometry,
        'boundary_sha256': digest(locks), 'images': images, 'metrics': measures, 'budgets': assignments_report,
        'new_recognition_performed': False, 'material_tree_changed': False, 'production_enabled': False,
        'lit_model': 'OFFLINE_FIXED_DIFFUSE_DIAGNOSTIC_NOT_SOFTWARE_LIGHTING',
        'ablations': 'Boundary-only retains R4 per-root detail RGB with modal fallback and skin underlayer; color-only uses R4 whole-face locks.',
        'historical_raw_frozen_tree_byte_hashes_verified': False})
    pages = ['<!doctype html><meta charset="utf-8"><title>R5 portrait review</title>',
             '<style>body{font:15px system-ui;margin:20px;background:#f4f5f6;color:#202124}img{max-width:100%;height:auto}h2{font-size:20px}a{color:#126649}.palette{display:flex;gap:16px;margin:12px 0}.swatch{width:22px;height:22px;border:1px solid #777;display:inline-block;vertical-align:middle}</style>',
             '<h1>R5 3-6 color review</h1><p>Source | R4 | Boundary only | Color only | R5. Visual acceptance pending. Offline lit is not software lighting.</p>']
    for card in baseline['palettes']:
        count = card['color_count']
        pages += [f'<h2>{count} colors</h2><div class="palette">']
        for slot in card['palette']:
            hex_color = '#'+''.join(f'{round(c*255):02x}' for c in slot['rgb'])
            pages += [f'<span><i class="swatch" style="background:{hex_color}"></i> {html.escape(slot["uid"])} {hex_color}</span>']
        pages += ['</div>']
        for scope, cameras in (('face', crop_views), ('body', views)):
            for name in cameras:
                for lighting in ('flat', 'lit'):
                    pages += [f'<details {"open" if name=="front" and lighting=="flat" else ""}><summary>{scope} {name} {lighting}</summary><img loading="lazy" src="colors-{count}/{scope}-{name}-{lighting}.png"></details>']
    with (output/'index.html').open('x', encoding='utf-8') as stream:
        stream.write('\n'.join(pages))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--renderer', type=Path, required=True)
    parser.add_argument('--size', type=int, default=1024)
    args = parser.parse_args()
    if args.size != 1024:
        parser.error('The R5 visual camera contract uses 1024 pixels')
    main(args)
