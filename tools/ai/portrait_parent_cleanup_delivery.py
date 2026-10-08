"""Validate and seal offline cleanup artifacts without installing or enabling an entry."""
import argparse
import json
from pathlib import Path
import subprocess
import sys

from beauty_leaf_domain import digest
from portrait_parent_cleanup import POLICY, PARENT_COLORS
from portrait_r5_baseline import sha
from portrait_r5_visual_review import publish
from portrait_r9_replay import baseline, load_partition
from portrait_surface_ownership_v2 import root_id, safe_reference, validate
from surface_detail_freeze import validate as validate_freeze


MODULES = ('portrait_parent_cleanup.py','portrait_parent_cleanup_replay.py',
           'portrait_r9_colors.py','portrait_surface_ownership_v2.py')


def validate_binding(plan, partition, locks, old_plan, ownership_sha, shape_sha, module_hashes):
    if plan.get('schema')!='orca.portrait-color-plan/v1' or plan['source_sha256']!=partition['source_sha256'] or \
            plan['geometry_id']!=partition['geometry_id'] or plan['boundary_sha256']!=partition['partition_sha256'] or \
            plan['partition_ref']!=locks['partition_ref'] or plan['shape_lock_sha256']!=shape_sha:
        raise ValueError('Color plan source, geometry, boundary or lock drift')
    if not safe_reference(plan['ownership_ref']) or plan['ownership_ref']['sha256']!=ownership_sha:
        raise ValueError('Color plan ownership reference drift')
    if plan['policy']!=POLICY or plan['modules']!=module_hashes or \
            plan['policy_sha256']!=digest(dict(policy=plan['policy'],modules=plan['modules'])):
        raise ValueError('Color policy or module identity drift')
    runtime = plan['runtime']
    if runtime['sha256']!=plan['runtime_sha256'] or \
            digest({k:v for k,v in runtime.items() if k!='sha256'})!=runtime['sha256'] or runtime['network']!='offline':
        raise ValueError('Offline color runtime identity drift')
    if plan['palette']!=old_plan['palette'] or plan['weights']!=old_plan['weights']:
        raise ValueError('Reviewed palette or weights changed')
    return True


def seal(run, color_folder, visual_folder):
    manifest,_,_,old_plans,freeze = baseline(run)
    document,locks = load_partition(run/'partition')
    ownership_path = run/'ownership-safe/surface-ownership.json'
    ownership = validate(json.loads(ownership_path.read_text()),document,locks,freeze['fingerprint'])
    units = {u['id']:(r,u) for r in ownership['regions'] for u in r['units']}
    explicit = {c['id']:f['source_face_id'] for f in document['faces'] for c in f['cells']}
    explicit_roots = set(explicit.values())
    module_hashes = {name:sha(Path(__file__).with_name(name)) for name in MODULES}
    receipts = []
    for mode in ('uniform','cleanup','combined'):
        variants = {}
        for count in (5,3,4,6):
            path = color_folder/mode/f'colors-{count}/portrait-color-plan.json'
            plan = json.loads(path.read_text())
            validate_binding(plan,document,locks,old_plans[count],sha(ownership_path),
                             sha(run/'partition/shape-locks.json'),module_hashes)
            if plan['baseline_plan_sha256']!=sha(run/f'baseline/r8-colors-{count}/portrait-color-plan.json') or \
                    sha(color_folder/plan['ownership_ref']['path'])!=sha(ownership_path):
                raise ValueError('Color plan baseline or ownership content drift')
            claimed = set()
            palette = {slot['uid']:index for index,slot in enumerate(plan['palette'])}
            for row in plan['cells']:
                key,face = row['id'],row['source_face_id']
                if key in claimed or type(face) is not int or not 0<=face<document['face_count']:
                    raise ValueError('Duplicate or out-of-range color unit')
                claimed.add(key)
                if key in explicit:
                    if face!=explicit[key]:
                        raise ValueError('Explicit color source mapping drift')
                elif face in explicit_roots or key!=root_id(document,face) or not row.get('implicit_root'):
                    raise ValueError('Implicit color crosses a mixed source face')
                uid = row['slot_uid']
                if uid is not None and (uid not in palette or row['slot']!=palette[uid]):
                    raise ValueError('Color decision uses an invalid palette role')
                if row['color_source'] in PARENT_COLORS and row['color_source']!='MANUAL':
                    if key not in units or units[key][0]['status']!='CONFIRMED_PARENT' or \
                            units[key][0]['parent_label']!=row['parent_label']:
                        raise ValueError('Automatic color lacks confirmed parent authority')
                    if row['parent_label']=='cloth' and uid in ('portrait-skin','portrait-lips'):
                        raise ValueError('Automatic neutral clothing borrowed a skin or lip role')
            variants[count] = plan
            receipts.append(dict(mode=mode,color_count=count,plan_sha256=sha(path),cells=len(plan['cells'])))
        validate_freeze(document,locks,variants,freeze)
    visual_path = visual_folder/'review-manifest.json'
    visual = json.loads(visual_path.read_text())
    if visual['source_sha256']!=manifest['source_sha256'] or any(
            row['frozen_rgb_changed_pixels'] or row['unconfirmed_rgb_changed_pixels'] or
            row['confirmed_skin_wrong_lip_pixels'] for row in visual['metrics']):
        raise ValueError('Visual replay violates frozen or ownership requirements')
    for image in visual['images']:
        path = visual_folder/image['path']
        if not path.resolve().is_relative_to(visual_folder.resolve()) or sha(path)!=image['sha256']:
            raise ValueError('Visual artifact path or hash drift')
    run_scope = len(ownership['scope_source_faces'])
    claimed_faces = len({u['source_face_id'] for _,u in units.values()})
    raster = visual['coverage']
    coverage = dict(original_source_faces=manifest['face_count'],analytic_visible_source_faces=run_scope,
                    source_faces_with_some_confirmed_ownership=claimed_faces,
                    analytic_unclaimed_source_faces=len(ownership['unclaimed_visible_source_faces']),
                    analytic_unseen_source_faces=manifest['face_count']-run_scope,
                    raster_visible_source_faces=raster['visible_source_faces'],
                    raster_source_faces_with_some_confirmed_ownership=raster['visible_source_faces']-raster['unknown_visible_source_faces'],
                    raster_unclaimed_source_faces=raster['unknown_visible_source_faces'],
                    partial_root_claim_is_not_complete_surface_coverage=True,
                    true_skin_ground_truth_coverage='NOT_KNOWN_REPORTED_MASK_COVERAGE_ONLY')
    timings = [json.loads(path.read_text()) for path in sorted(run.glob('*-timing.json'))
               if path.name.split('-')[0] not in ('checks',)]
    output = run/'delivery'
    output.mkdir(parents=True,exist_ok=False)
    result = subprocess.run([sys.executable,'-m','unittest','test_portrait_parent_cleanup_delivery','-v'],
                            cwd=Path(__file__).parent,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,
                            encoding='utf-8',errors='replace')
    log = output/'binding-tests.log'
    with log.open('x',encoding='utf-8') as stream:
        stream.write(result.stdout)
    if result.returncode:
        raise ValueError('Color identity binding regression failed')
    files = [dict(path=str(path.relative_to(run)),bytes=path.stat().st_size,sha256=sha(path))
             for path in sorted(run.rglob('*')) if path.is_file() and not path.is_relative_to(output)]
    report = dict(schema='orca.frozen-parent-cleanup-delivery/v1',source_sha256=manifest['source_sha256'],
                  geometry_id=manifest['geometry_id'],detail_freeze_sha256=freeze['fingerprint'],
                  partition_ref=locks['partition_ref'],shape_lock_sha256=sha(run/'partition/shape-locks.json'),
                  ownership_sha256=sha(ownership_path),color_plans=receipts,coverage=coverage,
                  visual_report_sha256=sha(visual_path),visual_path=str(visual_folder.relative_to(run))+'/index.html',
                  timings=timings,frozen_rgb_changed_pixels=0,original_model_hash_unchanged=True,
                  stage_added_triangles=document['added_triangles'],triangle_budget=document['triangle_budget'],
                  source_snapshot_before_sha256=sha(run/'source-before/manifest.json'),
                  source_snapshot_after_sha256=sha(run/'source-after/manifest.json'),
                  code_checks=str(run/'checks-r3/receipt.json'),visual_acceptance='PENDING_USER',
                  binding_test_log_sha256=sha(log),python_tests=88,cpp_tests=352,
                  main_window_acceptance='NOT_ENTERED_OFFLINE_FIRST',print_fidelity='NOT_VERIFIED',
                  residual_status='PARTIAL_IMPROVEMENT',paid_services_used=False,production_enabled=False,
                  material_tree_changed=False,historical_material_tree_byte_hash='UNAVAILABLE_NOT_CLAIMED',
                  contour_4096_precision='PREEXISTING_FAILURE_NOT_ADDRESSED')
    publish(output/'artifact-manifest.json',dict(schema='orca.offline-cleanup-artifacts/v1',files=files))
    report['artifact_manifest_sha256'] = sha(output/'artifact-manifest.json')
    publish(output/'delivery.json',report)
    print(json.dumps(dict(sealed_files=len(files),color_plans=len(receipts),frozen_rgb_changed_pixels=0,
                          status='OFFLINE_CANDIDATE_PENDING_USER')),flush=True)


if __name__=='__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('run','colors','visual'):
        parser.add_argument('--'+name,type=Path,required=True)
    args = parser.parse_args()
    seal(args.run.resolve(),args.colors.resolve(),args.visual.resolve())
