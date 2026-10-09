"""Local-only parent evidence on preserved source renders, never on candidate colors."""
import argparse
import importlib.metadata
import json
from pathlib import Path
import shutil

import numpy as np

from beauty_leaf_domain import digest
import local_body_regions as body
from local_semantic_worker import restrict_network
from portrait_r5_baseline import sha
from portrait_r5_visual_review import basis, publish
from portrait_r6_repair import load_render, verify
from portrait_r7_review import camera_identity, render


def camera_family(camera):
    direction = np.asarray(camera['basis'], dtype=float)[2]
    direction /= np.linalg.norm(direction)
    return 'camera-'+digest(np.round(direction, 4).tolist())[:16]


def package_versions():
    packages = {name: importlib.metadata.version(name) for name in ('mediapipe', 'numpy')}
    for name in ('opencv-python-headless', 'opencv-python', 'opencv-contrib-python'):
        try:
            packages[name] = importlib.metadata.version(name)
            break
        except importlib.metadata.PackageNotFoundError:
            continue
    else:
        raise ValueError('Installed OpenCV distribution is missing')
    return packages


def supplemental_cameras(manifest):
    cameras = []
    # Keep head/neck and whole-body context: the two old crops of one direction
    # are one vote, whereas these four directions add independent evidence.
    for scope, angles, center, extent, size in (
            ('face', (15, 45), [9., 9., 64.], 22., 1024),
            ('body', (-60, -120), [0., -15., 44.], 44., 1536)):
        for angle in angles:
            yaw = np.deg2rad(angle)
            identity = dict(geometry_id=manifest['geometry_id'], source_sha256=manifest['source_sha256'],
                scope=scope, view=f'supplement-yaw-{angle:+d}',
                basis=basis([np.cos(yaw), np.sin(yaw), 0.]).tolist(),
                center=center, half_height=extent, width=size, height=size)
            cameras.append(dict(identity, render_id=digest(identity)))
    return cameras


def coverage_cameras(manifest):
    cameras=[]
    for scope,view,yaw,pitch,center,extent,size in (
            ('body','supplement-front-arm',15.,0.,[0.,-15.,44.],44.,1536),
            ('body','supplement-neck-low',-31.,-15.,[0.,3.,50.],16.,1024)):
        yaw,pitch=np.deg2rad([yaw,pitch])
        identity=dict(geometry_id=manifest['geometry_id'],source_sha256=manifest['source_sha256'],
            scope=scope,view=view,basis=basis([np.cos(yaw)*np.cos(pitch),np.sin(yaw)*np.cos(pitch),
                                            np.sin(pitch)]).tolist(),
            center=center,half_height=extent,width=size,height=size)
        cameras.append(dict(identity,render_id=digest(identity)))
    return cameras


def coverage_records(records):
    redundant={('body','front-right'),('face','front-left')}
    omitted=[row for row in records if (row['camera']['scope'],row['camera']['view']) in redundant]
    kept=[row for row in records if (row['camera']['scope'],row['camera']['view']) not in redundant]
    if len(omitted)!=len(redundant) or any(row['family'] not in {r['family'] for r in kept} for row in omitted):
        raise ValueError('Coverage completion cannot discard an independent direction')
    return kept,omitted


def raw_records(previous, manifest):
    from portrait_r9_replay import observations
    report = json.loads((previous/'evidence.json').read_text())
    if report.get('source_refinement_performed') or not report.get('local_inference_performed') or \
            report.get('model_sha256') != body.SHA256 or \
            report.get('detail_freeze_sha256') != manifest['detail_freeze_sha256']:
        raise ValueError('Supplement requires original same-source body model observations')
    records = []
    for row, observation in observations(previous, manifest):
        if any(key.startswith('model_') for key in observation) or \
                set(observation) != {'rgb', 'ids', 'bary', 'labels', 'confidence'}:
            raise ValueError('Refined masks cannot become fresh model confidence')
        records.append(row)
    return records


def observe(run, weights, output, previous=None, complete_coverage=False):
    manifest = verify(run)
    model_path = weights/body.MODEL
    if model_path.stat().st_size != body.SIZE or sha(model_path) != body.SHA256:
        raise ValueError('Installed body model missing or identity drift')
    restrict_network()
    data = load_render(run)
    records = raw_records(previous, manifest) if previous else []
    omitted=[]
    if complete_coverage and previous is None:
        raise ValueError('Coverage completion requires preserved model observations')
    if previous:
        if complete_coverage:
            records,omitted=coverage_records(records)
            jobs=coverage_cameras(manifest)
        else:
            jobs = supplemental_cameras(manifest)
            old_families = {row['family'] for row in records}
            if any(camera_family(camera) in old_families for camera in jobs):
                raise ValueError('Supplemental camera repeats a previous direction')
    else:
        cameras = {(r['scope'], r['view']): r for r in
                   json.loads((run/'baseline/review-manifest.json').read_text())['images']}
        jobs = [c for (scope, _), c in cameras.items() if scope=='body']
        for side in ('front-left', 'front-right'):
            camera = dict(cameras['body', 'front'])
            camera['basis'] = cameras['face', side]['basis']
            camera['view'] = side
            identity = {k: camera[k] for k in ('geometry_id', 'source_sha256', 'scope', 'view',
                                             'basis', 'center', 'half_height', 'width', 'height')}
            camera['render_id'] = digest(identity)
            jobs.append(camera)
        # Close-up context improves junctions but is correlated with its full camera.
        jobs += [c for (scope, _), c in cameras.items() if scope=='face']
    if len(records)+len(jobs)>16:
        raise ValueError('Parent visibility supports at most 16 views')
    additional_families={camera_family(camera) for camera in jobs}-{row['family'] for row in records}
    output.mkdir(parents=True, exist_ok=False)
    for row in records:
        shutil.copyfile(previous/row['path'], output/row['path'])
        if sha(output/row['path']) != row['sha256']:
            raise ValueError('Preserved model observation changed during copy')
    start = len(records)
    with body.load(model_path) as model:
        for index, original in enumerate(jobs, start=start):
            camera = camera_identity(original, original['width'] if previous else 1024)
            rgb, ids, bary, _ = render(data, camera)
            family, _, labels, confidence = body.observe(model, rgb, ids, camera_family(camera))
            path = output/f'{index:02d}.npz'
            while path.exists():
                index+=1;path=output/f'{index:02d}.npz'
            with path.open('xb') as stream:
                np.savez_compressed(stream, rgb=rgb, ids=ids, bary=bary, labels=labels,
                                    confidence=confidence.astype(np.float32))
            records.append(dict(path=path.name, sha256=sha(path), camera=camera, family=family,
                class_pixels={str(k): int(((ids>=0)&(labels==k)&(confidence>=.9)).sum()) for k in range(6)}))
            print(json.dumps(dict(view=camera['view'], scope=camera['scope'], family=family,
                                  class_pixels=records[-1]['class_pixels'])), flush=True)
    policy = dict(algorithm='r9-local-parent-supplement/v4', model_sha256=body.SHA256,
        source_texture_only=True, body_skin_uses_whole_body=True, narrow_neck_rule_reused=False,
        minimum_independent_views=2, confidence=.9, minimum_coverage=.9,
        frozen_feature_predictions_consumed=False, correlated_crops_share_camera=True,
        modules={name: sha(Path(__file__).with_name(name)) for name in ('portrait_r9_evidence.py', 'local_body_regions.py')},
        packages=package_versions())
    if previous:
        policy.update(previous_model_cache_sha256=sha(previous/'evidence.json'),
                      preserved_model_observations=start,
                      new_independent_cameras=len(additional_families),
                      supplemental_cameras=jobs, refined_masks_reused_as_model=False)
    if complete_coverage:
        policy.update(coverage_completion=True,omitted_correlated_records=omitted,
                      omitted_cache_preserved=str(previous),maximum_observations=16,
                      same_family_body_and_head_are_one_vote=True)
    publish(output/'evidence.json', dict(schema='orca.portrait-parent-observations/v1',
        identity={k: manifest[k] for k in ('source_sha256', 'geometry_id', 'face_count', 'evidence_sha256')},
        detail_freeze_sha256=manifest['detail_freeze_sha256'], model_path=str(model_path), model_sha256=body.SHA256,
        policy=policy, policy_sha256=digest(policy), observations=records,
        local_inference_performed=True, paid_generation_performed=False, production_enabled=False))


if __name__=='__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('run', 'weights', 'output'):
        parser.add_argument('--'+name, type=Path, required=True)
    parser.add_argument('--previous', type=Path)
    parser.add_argument('--complete-coverage',action='store_true')
    args = parser.parse_args()
    observe(args.run.resolve(), args.weights.resolve(), args.output.resolve(),
            args.previous.resolve() if args.previous else None,args.complete_coverage)
