"""Serial R7 contour replay from frozen source evidence, without recognition."""
import argparse
from collections import Counter, defaultdict
import json
from pathlib import Path
import shutil
import subprocess

import cv2
import numpy as np

from beauty_leaf_domain import LeafKey, digest
from local_face_landmarks import surface_neighbors
from local_leaf_boundaries import boundary_band
from local_semantic_geometry import read
from local_surface_contours import POLICY, independent_views, proposals
from portrait_r5_baseline import sha
from portrait_r5_boundaries import load_views
from portrait_r5_visual_review import publish
from portrait_r6_repair import verify
from portrait_surface_ownership import trusted_regions


def topology_scope(accepted, neighbors, legal, blocked, rings):
    allowed=set(legal)-set(blocked)
    scope=set(accepted)&allowed
    for _ in range(rings):
        scope |= {int(n) for f in scope for n in neighbors[f] if int(n)>=0}&allowed
    return scope


def base_cells(face, keys, locks, owners, subject, redefine):
    cells=[]
    for key in keys:
        label='R6'
        for lock in locks['locks']:
            if any(old.contains(key) for old in owners['locks'][lock['label']].get(face,[])):
                label=lock['label']
                if any(old.contains(key) for old in owners['nested'].get(face,[])):
                    label='iris-'+label
                break
        if label=='R6':
            for old,parent in owners['parents'].get(face,[]):
                if old.contains(key): label=parent; break
        fallback=label
        if redefine and (label in ('le','re','lb','rb','ulip','llip') or label.startswith('iris-')):
            label='face'
        cells.append(dict(polygon=key.corners().tolist(),holes=[],label=label,
                          fallback_label=fallback,parent_label='face' if label=='R6' else label,
                          subject_id=subject,kind='R6_PARENT',source_leaf=key.encode()))
    return cells


def prepare(run, output):
    manifest=verify(run)
    vertices,faces,geometry=read(run/'baseline/native.bin',manifest['source_sha256'])
    if geometry!=manifest['geometry_id']:
        raise ValueError('Canonical source mapping changed')
    locks=json.loads((run/'baseline/shape-locks.json').read_text())
    evidence=json.loads((run/'baseline/evidence.json').read_text())
    ownership=json.loads((run/'baseline/surface-ownership.json').read_text())
    if digest(locks)!=manifest['boundary_sha256'] or digest(ownership)!=manifest['ownership_sha256']:
        raise ValueError('R6 boundary or parent identity drift')
    neighbors=surface_neighbors(vertices,faces)
    subject=evidence['subjects'][0]
    if evidence['subjects']!=[subject]:
        raise ValueError('R7 requires the single preserved subject')
    records,conflicts=trusted_regions(evidence)
    parts=load_views(run/'baseline/views')
    independent=independent_views(parts,vertices,faces)
    output.mkdir(parents=True,exist_ok=False)
    original={r['label']:r for r in evidence['shape_details'] if r['accepted_faces']}
    old_roots={r['label']:set(r['locked_faces']) for r in locks['locks']}
    owners=dict(locks={},nested=defaultdict(list),parents=defaultdict(list))
    for lock in locks['locks']:
        owners['locks'][lock['label']]=defaultdict(list)
        for row in lock['locked_leaves']:
            key=LeafKey(*row); owners['locks'][lock['label']][key.source_face_id].append(key)
        for row in lock['nested_leaves']:
            key=LeafKey(*row); owners['nested'][key.source_face_id].append(key)
    base=defaultdict(list)
    for row in ownership['editing_domain']['split_leaves']:
        key=LeafKey(*row); base[key.source_face_id].append(key)
    for region in ownership['regions']:
        for row in region['leaves']:
            key=LeafKey(*row); owners['parents'][key.source_face_id].append((key,region['parent_label']))
    per_face=defaultdict(list)
    detail_reports=[]
    saved_curves={}
    curve_library={}
    for label in ('le','re','lb','rb','ulip','llip','periocular-le','periocular-re','imouth-preserve'):
        primary='ulip' if label=='imouth-preserve' else label.removeprefix('periocular-')
        accepted=set(original[primary]['accepted_faces']) | old_roots[primary]
        if label=='imouth-preserve': accepted |= set(original['llip']['accepted_faces']) | old_roots['llip']
        legal={face for face,labels in records.items() if labels.get('face',labels.get(primary))==subject}
        confirmed_parent_roots={face for face,rows in owners['parents'].items() if any(parent=='face' for _,parent in rows)}
        legal |= confirmed_parent_roots
        # Existing accepted features carry validated ownership even when their
        # coarse root did not meet the stronger parent summary confidence.
        legal |= accepted
        others={f for side,roots in old_roots.items() if side!=primary for f in roots}
        if label=='imouth-preserve': others -= old_roots['llip']
        rejected=set(original[primary]['rejected_faces']) | conflicts | others
        prohibited={face for face,labels in records.items() if set(labels)&{'hair','cloth','teeth','imouth'} and face not in accepted}
        rings=2 if label in ('lb','rb') else 1
        scope=topology_scope(accepted,neighbors,legal,rejected|prohibited,rings)
        boundary,_=boundary_band(accepted,neighbors)
        views,audit=proposals(label,independent,vertices,faces,accepted,accepted-boundary,scope,legal-prohibited)
        saved_curves[label]=[dict(family=v.family,transform=v.transform.tolist(),scale=v.scale,
                                 contours=[p.tolist() for p in v.contours],iris=[p.tolist() for p in v.iris or []],
                                 holes=[p.tolist() for p in v.holes or []],
                                 opening=[p.tolist() for p in v.opening or []]) for v in views]
        retained=[]
        support_counts=Counter()
        for face in sorted(scope):
            world=vertices[faces[face]]
            witnesses=[]
            iris_witnesses=[]
            for view in views:
                if not view.supported(face,world):
                    continue
                try:
                    witness=view.references(world,curve_library,label)
                    witnesses.append(witness)
                    if primary in ('le','re') and not label.startswith('periocular-'):
                        iris_witnesses.append(view.references(world,curve_library,'iris-'+label,view.iris))
                except ValueError as error:
                    retained.append(dict(face=face,reason=str(error),family=view.family))
            support_counts[len(witnesses)]+=1
            if len(witnesses)<2:
                if face in accepted:
                    retained.append(dict(face=face,reason='R6_LOCAL_VIEW_OR_OCCLUSION_FALLBACK'))
                    per_face[face]
                continue
            if primary in ('le','re') and not label.startswith('periocular-'):
                iris_layer=dict(label='iris-'+primary,parent_label=primary,subject_id=subject,
                    views=iris_witnesses,envelope_views=witnesses,kind='CLIPPED_IRIS')
                if face not in accepted:
                    iris_layer['parent_polygons']=[dict(polygon=k.corners().tolist(),holes=[]) for k,p in owners['parents'].get(face,[]) if p=='face']
                per_face[face].append(iris_layer)
            final_label='imouth' if label=='imouth-preserve' else label
            layer=dict(label=final_label,parent_label='imouth' if label=='imouth-preserve' else primary,
                subject_id=subject,views=witnesses,kind='R6_ORAL_PASSTHROUGH' if label=='imouth-preserve' else
                'SOURCE_RESTORED_EYE_LINE' if label.startswith('periocular-') else 'CLIPPED_FEATURE')
            if face not in accepted:
                layer['parent_polygons']=[dict(polygon=k.corners().tolist(),holes=[]) for k,p in owners['parents'].get(face,[]) if p=='face']
            per_face[face].append(layer)
        detail_reports.append(dict(label=label,scope_roots=len(scope),r6_roots=len(old_roots[primary]),
                                   legal_seed_roots=len(accepted),topology_rings=rings,
                                   independent_view_count=len(views),view_audit=audit,
                                   support_counts=dict(support_counts),retained=retained))
        print(json.dumps(dict(label=label,scope_roots=len(scope),independent_views=len(views))),flush=True)
    for roots in old_roots.values():
        for face in roots: per_face[face]
    face_requests=[]
    for face,layers in sorted(per_face.items()):
        # Iris/eye opening have precedence over their attached exterior lines.
        layers.sort(key=lambda r: (0 if r['label'].startswith('iris-') else
                                  1 if r['label'].startswith('periocular-') or r['label']=='imouth' else 2,r['label']))
        keys=base.get(face,[LeafKey(face)])
        clipped=any(r['label'] in ('le','re','lb','rb','ulip','llip') for r in layers)
        cells=base_cells(face,keys,locks,owners,subject,clipped)
        face_requests.append(dict(source_face_id=face,baseline_triangle_count=len(keys),base=cells,
                                  layers=layers,reasons=[] if layers else ['R6_LOCAL_VIEW_OR_OCCLUSION_FALLBACK']))
    import importlib.metadata
    policy=dict(POLICY,dependencies={k:importlib.metadata.version(k) for k in ('numpy','scipy','opencv-python')},
                confirmed_parent_ownership_sha256=manifest['ownership_sha256'],
                parent_authority='same-subject confirmed R6 face leaves; mixed siblings excluded',
                modules={name:sha(Path(__file__).with_name(name)) for name in
                         ('local_surface_contours.py','local_leaf_boundaries.py','portrait_r7_replay.py')})
    identity={key:locks[key] for key in ('source_sha256','geometry_id','evidence_sha256','runtime_sha256','policy_sha256','face_count')}
    identity.update(baseline_sha256=manifest['boundary_sha256'],boundary_policy_sha256=digest(policy))
    request=dict(schema='orca.surface-partition-request/v1',identity=identity,
                 existing_added_triangles=manifest['r6_added_triangles'],triangle_budget=manifest['triangle_budget'],
                 curve_library=curve_library,faces=face_requests)
    publish(output/'request.json',request)
    publish(output/'fitted-contours.json',saved_curves)
    report=dict(schema='orca.r7-contour-proposal-report/v1',identity=identity,details=detail_reports,policy=policy,
                root_count=len(face_requests),independent_cameras=[v.family for v,_,_,_ in independent],
                new_recognition_performed=False,material_tree_changed=False,production_enabled=False)
    publish(output/'stage-report.json',report)
    print(json.dumps(dict(root_count=len(face_requests),details=[{k:r[k] for k in
        ('label','scope_roots','independent_view_count','support_counts')} for r in detail_reports])),flush=True)


def lock(run, folder, output, tool):
    manifest=verify(run)
    request=json.loads((folder/'request.json').read_text())
    output.mkdir(parents=True,exist_ok=False)
    partition_path=output/'partition.json'
    subprocess.run([str(tool),str(folder/'request.json'),str(partition_path)],check=True)
    partition=json.loads(partition_path.read_text())
    if partition['source_sha256']!=manifest['source_sha256'] or partition['geometry_id']!=manifest['geometry_id']:
        raise ValueError('Native clipping source drift')
    content_hash=sha(partition_path)
    destination=output/'surface-partitions'/f'{content_hash}.json'
    destination.parent.mkdir()
    partition_path.rename(destination)
    cells=defaultdict(list)
    for face in partition['faces']:
        for cell in face['cells']:
            cells[cell['label']].append(cell)
    old=json.loads((run/'baseline/shape-locks.json').read_text())
    locks=[]
    for item in old['locks']:
        label=item['label']
        selected=cells[label]+cells['iris-'+label]
        if not selected: continue
        locks.append(dict(subject_id=item['subject_id'],label=label,parent_label=item['parent_label'],
            status='PROTECTED_SHAPE_UNCERTAIN',view_support=2,
            reasons=['SOURCE_BOUNDARY_VISUAL_REVIEW_PENDING'],locked_cells=sorted(c['id'] for c in selected),
            nested_cells=sorted(c['id'] for c in cells['iris-'+label]),
            periocular_cells=sorted(c['id'] for c in cells['periocular-'+label])))
    sidecar=dict(request['identity'],schema='orca.beauty-shape-lock/v3',locks=locks,
                 partition_ref=dict(schema='orca.surface-partition-reference/v1',
                                    path='surface-partitions/'+content_hash+'.json',sha256=content_hash))
    publish(output/'shape-locks.json',sidecar)
    subprocess.run([str(tool),'--validate-v3',str(folder/'request.json'),str(output/'shape-locks.json'),str(output)],check=True)
    publish(output/'stage-report.json',dict(partition_sha256=partition['partition_sha256'],
        partition_file_sha256=content_hash,shape_lock_sha256=sha(output/'shape-locks.json'),
        added_triangles=partition['added_triangles'],triangle_budget=partition['triangle_budget'],
        face_status_counts=dict(Counter(f['status'] for f in partition['faces'])),
        cell_counts={k:len(v) for k,v in cells.items()},native_tool_sha256=sha(tool),
        production_enabled=False,workbench_v3_enabled=False,visual_status='PARTIAL_IMPROVEMENT'))
    print(json.dumps(dict(lock_count=len(locks),cell_counts={k:len(v) for k,v in cells.items()},
                          added_triangles=partition['added_triangles'])),flush=True)


def compact(folder, output):
    request=json.loads((folder/'request.json').read_text())
    report=json.loads((folder/'stage-report.json').read_text())
    audits=[]
    for key,points in request['curve_library'].items():
        polygon=np.asarray(points,dtype=np.float32)
        reduced=cv2.approxPolyDP(polygon,.035,True).reshape(-1,2)
        if len(reduced)<3:
            reduced=polygon
        request['curve_library'][key]=reduced.astype(float).tolist()
        audits.append(dict(curve=key,fitted_points=len(polygon),clip_points=len(reduced),
                           reduction_max_error_4096=.14))
    policy=dict(report['policy'],curve_reduction_epsilon_1024=.035,
                fitted_parent_policy_sha256=request['identity']['boundary_policy_sha256'],
                reducer_sha256=sha(Path(__file__)))
    request['identity']['boundary_policy_sha256']=digest(policy)
    report.update(identity=request['identity'],policy=policy,curve_reduction=audits)
    output.mkdir(parents=True,exist_ok=False)
    publish(output/'request.json',request)
    publish(output/'stage-report.json',report)
    shutil.copyfile(folder/'fitted-contours.json',output/'fitted-contours.json')
    print(json.dumps(dict(curves=len(audits),fitted_points=sum(r['fitted_points'] for r in audits),
                          clip_points=sum(r['clip_points'] for r in audits))),flush=True)


def seams(run,folder,output):
    manifest=verify(run)
    vertices,faces,geometry=read(run/'baseline/native.bin',manifest['source_sha256'])
    request=json.loads((folder/'request.json').read_text())
    report=json.loads((folder/'stage-report.json').read_text())
    _,welded=np.unique(vertices,axis=0,return_inverse=True)
    neighbors=surface_neighbors(vertices,faces)
    ownership=json.loads((run/'baseline/surface-ownership.json').read_text())
    base=defaultdict(list)
    for row in ownership['editing_domain']['split_leaves']:
        key=LeafKey(*row); base[key.source_face_id].append(key)
    included={r['source_face_id'] for r in request['faces']}
    # Include the full pre-existing split domain before its edge neighbors.
    # Terminal neighbors are unsplit roots, so conformity cannot propagate
    # unrecorded cuts into an implicit R6 root further away.
    initial=included | set(base)
    adjacent=initial | {int(n) for f in initial for n in neighbors[f] if int(n)>=0}
    adjacent-=included
    subject=json.loads((run/'baseline/evidence.json').read_text())['subjects'][0]
    for face in sorted(adjacent):
        keys=base.get(face,[LeafKey(face)])
        cells=[dict(polygon=k.corners().tolist(),holes=[],label='R6',parent_label='face',
                    subject_id=subject,kind='R6_PASSTHROUGH',source_leaf=k.encode()) for k in keys]
        request['faces'].append(dict(source_face_id=face,baseline_triangle_count=len(keys),base=cells,layers=[],
                                    reasons=['NON_TARGET_SEAM_CONFORMITY_ONLY']))
    request['faces'].sort(key=lambda r:r['source_face_id'])
    for row in request['faces']:
        row['source_vertices']=welded[faces[row['source_face_id']]].tolist()
    policy=dict(report['policy'],seam_conformity='welded_source_edge_intersections',
                seam_parent_policy_sha256=request['identity']['boundary_policy_sha256'],
                seam_builder_sha256=sha(Path(__file__)),native_modules={name:sha(Path(__file__).resolve().parents[2]/name) for name in
                    ('src/slic3r/GUI/AI/Model/SurfacePartition.hpp','src/slic3r/GUI/AI/Model/BeautySurfaceShapeLock.hpp',
                     'tools/ai/surface_partition_tool.cpp','src/libslic3r/ClipperUtils.cpp','src/libslic3r/Tesselate.cpp')})
    request['identity']['boundary_policy_sha256']=digest(policy)
    report.update(identity=request['identity'],policy=policy,seam_only_roots=len(adjacent))
    output.mkdir(parents=True,exist_ok=False)
    publish(output/'request.json',request)
    publish(output/'stage-report.json',report)
    shutil.copyfile(folder/'fitted-contours.json',output/'fitted-contours.json')
    print(json.dumps(dict(seam_only_roots=len(adjacent),total_roots=len(request['faces']))),flush=True)


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('stage',choices=('prepare','compact','seams','lock'))
    parser.add_argument('--run',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--proposals',type=Path)
    parser.add_argument('--tool',type=Path)
    args=parser.parse_args()
    if args.stage=='prepare': prepare(args.run.resolve(),args.output.resolve())
    elif args.stage=='compact': compact(args.proposals.resolve(),args.output.resolve())
    elif args.stage=='seams': seams(args.run.resolve(),args.proposals.resolve(),args.output.resolve())
    else: lock(args.run.resolve(),args.proposals.resolve(),args.output.resolve(),args.tool.resolve())
