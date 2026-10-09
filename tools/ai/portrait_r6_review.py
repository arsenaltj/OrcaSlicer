"""Exact-camera R5/R6 comparisons and an audit that retains unresolved residuals."""
import argparse
from collections import Counter
import html
import json
from pathlib import Path

import cv2
import numpy as np
from PIL import Image

from beauty_leaf_domain import LeafKey, digest
from portrait_r5_baseline import sha
from portrait_r5_review_audit import oklab
from portrait_r5_visual_review import assignments, child, lit, publish, remap, sheet
from portrait_r6_repair import load_render, render_camera, verify
from portrait_surface_ownership import validate


def masks(document, locks, ids, bary, groups):
    roots, children = {}, []
    for region in document['regions']:
        value = 1 if region['parent_label'] == 'face' else 2
        for row in region['leaves']:
            key = LeafKey(*row)
            if key.depth:
                children.append(child(key, [value/255, 0, 0]))
            else:
                roots[key.source_face_id] = [value/255, 0, 0]
    for lock in locks['locks']:
        for row in lock['locked_leaves']:
            key = LeafKey(*row)
            if key.depth:
                children.append(child(key, [3/255, 0, 0]))
            else:
                roots[key.source_face_id] = [3/255, 0, 0]
    return remap(np.zeros((*ids.shape, 3), dtype=np.uint8), ids, bary, roots, children,
                 groups, locks['face_count'])[:, :, 0]


def review(run, colors, ownership_folder, output):
    manifest = verify(run)
    locks = json.loads((run/'baseline/shape-locks.json').read_text())
    ownership = validate(json.loads((ownership_folder/'surface-ownership.json').read_text()), locks)
    ownership_report = json.loads((ownership_folder/'stage-report.json').read_text())
    stage = json.loads((colors/'stage-report.json').read_text())
    if stage['ownership_sha256'] != sha(ownership_folder/'surface-ownership.json'):
        raise ValueError('Color decisions belong to another ownership')
    variants = {}
    for count in (3, 4, 5, 6):
        row = next(r for r in stage['variants'] if r['color_count'] == count)
        folder = colors/f'colors-{count}'
        for name, field in (('portrait-color-plan.json', 'plan_sha256'),
                            ('face-colors.bin', 'face_colors_sha256'), ('subface-colors.json', 'subface_colors_sha256')):
            if sha(folder/name) != row[field]:
                raise ValueError('Candidate color file changed')
        plan = json.loads((folder/'portrait-color-plan.json').read_text())
        old_plan = json.loads((run/f'baseline/colors-{count}/portrait-color-plan.json').read_text())
        if plan['palette'] != old_plan['palette'] or plan['boundary_sha256'] != digest(locks):
            raise ValueError('Reviewed palette or feature boundary changed')
        variants[count] = (plan, *( (assignments(path/'face-colors.bin', manifest['face_count']),
                                     json.loads((path/'subface-colors.json').read_text()))
                                   for path in (run/f'baseline/colors-{count}', folder)))
    output.mkdir(parents=True, exist_ok=False)
    data = load_render(run)
    triangles = data[1][data[2]]
    normals = np.cross(triangles[:,1]-triangles[:,0], triangles[:,2]-triangles[:,0]).astype(float)
    normals /= np.maximum(np.linalg.norm(normals, axis=1)[:,None], 1e-15)
    cameras = {(r['scope'], r['view']): r for r in json.loads((run/'baseline/review-manifest.json').read_text())['images']}
    images, metrics = [], []
    scope_roots = np.zeros(manifest['face_count'], dtype=bool)
    scope_roots[ownership_report['scope_face_ids']] = True
    for (scope, view), camera in cameras.items():
        source, ids, _, bary, groups = render_camera(data, camera)
        labels = masks(ownership, locks, ids, bary, groups)
        valid = ids >= 0
        target = (labels == 1) | (labels == 2)
        scope_mask = valid.copy()
        scope_mask[valid] = scope_roots[ids[valid]]
        scope_mask |= labels == 3
        front = []
        for count, (plan, before, after) in variants.items():
            folder = output/f'colors-{count}'
            folder.mkdir(exist_ok=True)
            old = remap(source, ids, bary, *before, groups, manifest['face_count'])
            new = remap(source, ids, bary, *after, groups, manifest['face_count'])
            difference = np.max(np.abs(new.astype(int)-old.astype(int)), axis=2)
            if np.any((difference > 1) & (labels == 3)):
                raise ValueError('Visible reviewed feature pixels changed')
            for lighting in ('flat', 'lit'):
                rgb = [source, old, new]
                if lighting == 'lit':
                    rgb = [lit(image, ids, normals, np.array(camera['basis'])) for image in rgb]
                path = folder/f'{scope}-{view}-{lighting}.png'
                sheet(rgb, ['Source', 'R5', 'R6'], path, 512 if scope == 'face' else 384)
                images.append(dict(path=str(path.relative_to(output)), sha256=sha(path), color_count=count,
                                   lighting=lighting, render_id=camera['render_id'], scope=scope, view=view))
            palette = {r['uid']: np.rint(np.array(r['rgb'])*255).astype(np.uint8) for r in plan['palette']}
            old_pink = np.all(old == palette.get('portrait-lips', [-1,-1,-1]), axis=2) & valid & (labels != 3)
            new_pink = np.all(new == palette.get('portrait-lips', [-1,-1,-1]), axis=2) & valid & (labels != 3)
            lab = oklab(new)
            warm = valid & (labels != 3) & (lab[:,:,0] < .75) & (lab[:,:,1] > .005) & (lab[:,:,2] > .01)
            if np.any(new_pink & (labels == 1)):
                raise ValueError('Confirmed skin retains the erroneous lip slot')
            components = []
            for kind, mask in (('UNRESOLVED_PINK_RGB', new_pink & ~target),
                               ('UNRESOLVED_WARM_DIAGNOSTIC', warm & ~target)):
                _, component_ids, stats, _ = cv2.connectedComponentsWithStats(mask.astype(np.uint8), 8)
                for c in range(1, len(stats)):
                    pixels = component_ids == c
                    components.append(dict(kind=kind, pixels=int(stats[c, cv2.CC_STAT_AREA]),
                                           bbox=stats[c,:4].tolist(), source_faces=np.unique(ids[pixels]).tolist(),
                                           retain_reason='UNCONFIRMED_PARENT_KEEP_R5'))
            metric = dict(color_count=count, scope=scope, view=view, render_id=camera['render_id'],
                          changed_visible_pixels=int((difference > 1).sum()),
                          outside_target_changed_pixels=int(((difference > 1) & ~target).sum()),
                          locked_changed_pixels=int(((difference > 1) & (labels == 3)).sum()),
                          face_scope_pixels=int(scope_mask.sum()), confirmed_skin_pixels=int((labels == 1).sum()),
                          face_parent_coverage=float(((labels == 1) & scope_mask).sum()/max(1,(scope_mask & (labels != 3)).sum())),
                          r5_lip_rgb_outside_locks=int(old_pink.sum()), r6_lip_rgb_outside_locks=int(new_pink.sum()),
                          corrected_lip_rgb_pixels=int((old_pink & ~new_pink).sum()),
                          confirmed_skin_lip_pixels=int((new_pink & (labels == 1)).sum()),
                          unresolved_face_pink_pixels=int((new_pink & scope_mask & ~target).sum()),
                          unresolved_warm_diagnostic_pixels=int((warm & ~target).sum()), components=components)
            metrics.append(metric)
            diagnostic = new.copy()
            diagnostic[(labels == 1) & (difference > 1)] = [65, 175, 95]
            diagnostic[new_pink & ~target] = [0, 205, 215]
            diagnostic[warm & ~target & ~new_pink] = [230, 175, 0]
            diagnostic[labels == 3] = [95, 115, 230]
            Image.fromarray(diagnostic).save(folder/f'{scope}-{view}-residual-locations.png')
            if scope == 'face' and view == 'front':
                Image.fromarray(new).save(folder/'front-r6-unlabeled.png')
                front.append(new)
        if front:
            sheet(front, ['R6 3 colors', 'R6 4 colors', 'R6 5 colors', 'R6 6 colors'], output/'front-four-budgets.png', 512)
        print(json.dumps(dict(rendered=scope+'-'+view)), flush=True)
    front_five = next(r for r in metrics if r['color_count'] == 5 and r['scope'] == 'face' and r['view'] == 'front')
    report = dict(schema='orca.portrait-r6-review/v1', code_status='PASS', visual_status='PENDING_USER',
                  source_sha256=manifest['source_sha256'], geometry_id=manifest['geometry_id'],
                  boundary_sha256=manifest['boundary_sha256'], ownership_sha256=stage['ownership_sha256'],
                  images=images, metrics=metrics, five_color_front=front_five,
                  face_scope_definition='All multiview head envelope or verified face/nose roots, including unresolved roots; locked features excluded only from parent denominator.',
                  warm_rgb_is_diagnostic_not_wrong_ownership=True,
                  clothing_status=ownership_report['clothing_scope_status'],
                  historical_raw_frozen_tree_byte_hashes_verified=False,
                  lit_model='OFFLINE_FIXED_DIFFUSE_DIAGNOSTIC_NOT_SOFTWARE_LIGHTING',
                  new_recognition_performed=False, production_enabled=False)
    publish(output/'review-manifest.json', report)
    pages = ['<!doctype html><meta charset="utf-8"><title>R6 portrait review</title>',
             '<style>body{font:15px system-ui;margin:24px;background:#f4f5f6;color:#202124}img{max-width:100%}h2{font-size:20px}</style>',
             '<h1>R6 3-6 color review</h1><p>Source | R5 | R6. Visual acceptance pending. Offline lit differs from software lighting.</p>',
             '<img src="front-four-budgets.png">',
             '<p>Diagnostic: green = parent repair; cyan = unresolved pink; gold = warm audit sample; blue = immutable feature lock.</p>']
    for count in variants:
        pages += [f'<h2>{count} colors</h2>']
        for (scope, view) in cameras:
            for lighting in ('flat', 'lit'):
                pages += [f'<details {"open" if scope=="face" and view=="front" and lighting=="flat" else ""}><summary>{scope} {view} {lighting}</summary><img loading="lazy" src="colors-{count}/{scope}-{view}-{lighting}.png"></details>']
        pages += [f'<details><summary>Residual audit</summary><img src="colors-{count}/face-front-residual-locations.png"></details>']
    with (output/'index.html').open('x', encoding='utf-8') as stream:
        stream.write('\n'.join(pages))
    print(json.dumps({k:v for k,v in front_five.items() if k != 'components'}), flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('run', 'colors', 'ownership', 'output'):
        parser.add_argument('--'+name, type=Path, required=True)
    args = parser.parse_args()
    review(args.run.resolve(), args.colors.resolve(), args.ownership.resolve(), args.output.resolve())
