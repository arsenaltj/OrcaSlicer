"""Serial R5 boundary replay from the preserved R4 inputs; never recognizes anew."""
import argparse
from collections import Counter
import copy
import json
import importlib.metadata
from pathlib import Path

import cv2
import numpy as np

from beauty_leaf_domain import LeafKey, digest, domain
from local_brow_boundary import Projection
from local_eye_landmarks import EYE_BY_LABEL
from local_face_landmarks import EYEBROWS, FaceView, surface_neighbors
from local_leaf_boundaries import (ROI_SIZE, ALGORITHM_VERSION, AnalyticVisibility, BoundaryView, boundary_band, reconstruct_projection,
                                    refine_root, refinement_roots, smooth_lid, source_brow_contour,
                                    BROW_ITERATIONS, BROW_MIN_SEED, BROW_SOURCE_DELTA_L, BROW_SMOOTH_SIGMA)
from local_semantic_geometry import read
from portrait_r5_baseline import sha


def publish(path, value):
    with path.open('x', encoding='utf-8') as stream:
        json.dump(value, stream, indent=2, sort_keys=True, allow_nan=False)


def load_views(root):
    result = []
    for path in sorted(root.glob('*.npz')):
        with np.load(path, allow_pickle=False) as data:
            meta = json.loads(str(data['metadata']))
            parts = {name: (data['part_'+name],data['fraction_'+name]) for name in meta['parts']}
            irises = {name: (data['iris_'+name],data['iris_fraction_'+name]) for name in meta['irises']}
            boundary = Projection(*(data['boundary_'+name] for name in
                ('rgb','ids','barycentric','uv','valid','origin','points'))) if meta['boundary'] else None
            result.append(FaceView(meta['family'],data['points'],data['world'],data['valid'],
                meta['scale'],meta['pixel_size'],data['visible'],data['counts'],data['head'],
                parts,irises,meta['quality'],boundary))
    # A zoomed crop does not add a new independent view.
    chosen = {}
    for view in result:
        if view.family not in chosen or view.quality > chosen[view.family].quality:
            chosen[view.family] = view
    return list(chosen.values())


def eye_views(label, views, vertices, faces):
    result, audit, corners = [], [], set()
    contour = EYE_BY_LABEL[label][0]
    for view in views:
        if label not in view.parts or view.boundary is None:
            continue
        projection = view.boundary
        row = {'family': view.family}
        audit.append(row)
        try:
            transform, residual = reconstruct_projection(projection, vertices, faces)
            smooth = smooth_lid(projection.points[contour])
        except ValueError as error:
            row.update(status='R4_LOCAL_PROJECTION_FALLBACK', reason=str(error))
            continue
        scale = ROI_SIZE/max(projection.ids.shape)
        occlusion = AnalyticVisibility(vertices,faces,transform,projection.ids.shape)
        result.append(BoundaryView(view.family,transform,smooth*scale,set(map(int,view.visible)),scale,residual,occlusion))
        for index in (contour[0],contour[8]):
            x,y = np.floor(projection.points[index]).astype(int)
            if 0 <= y < projection.ids.shape[0] and 0 <= x < projection.ids.shape[1]:
                face = int(projection.ids[y,x])
                if face >= 0:
                    corners.add(face)
        row.update(status='PROJECTED', roi_size=ROI_SIZE, camera_residual_pixels=residual*scale)
    return result,audit,corners


def brow_views(label,views,vertices,faces,accepted,core,permitted,legal):
    result,audit = [],[]
    for view in views:
        if label not in view.parts or view.boundary is None:
            continue
        projection = view.boundary
        row = {'family':view.family}
        audit.append(row)
        try:
            transform,residual = reconstruct_projection(projection,vertices,faces)
            contour,source_audit = source_brow_contour(projection,accepted,core,permitted,legal,
                                                       projection.points[EYEBROWS[label]])
        except (ValueError,cv2.error) as error:
            row.update(status='R4_LOCAL_SOURCE_FALLBACK',reason=str(error))
            continue
        scale = ROI_SIZE/max(projection.ids.shape)
        occlusion = AnalyticVisibility(vertices,faces,transform,projection.ids.shape)
        result.append(BoundaryView(view.family,transform,contour,set(map(int,view.visible)),scale,residual,occlusion))
        row.update(status='SOURCE_PROJECTED',roi_size=ROI_SIZE,camera_residual_pixels=residual*scale,**source_audit)
    return result,audit,set()


def replay(run, output, labels, previous=None):
    manifest = json.loads((run/'stage-manifest.json').read_text())
    for item in manifest['files']:
        if sha(run/item['path']) != item['sha256']:
            raise ValueError('Preserved R4 input hash drift')
    source = run/'baseline/source.glb'
    if sha(source) != manifest['source_sha256']:
        raise ValueError('R4 source changed')
    vertices,faces,geometry = read(run/'baseline/replay/native.bin',manifest['source_sha256'])
    evidence = json.loads((run/'baseline/replay/evidence.json').read_text())
    if geometry != manifest['geometry_id'] or len(faces) != manifest['face_count']:
        raise ValueError('Canonical geometry drift')
    if previous:
        from portrait_r5_boundary_review import verify
        locks,_ = verify(run,previous)
    else:
        locks = {key:evidence[key] for key in ('geometry_id','source_sha256','runtime_sha256','policy_sha256','face_count')}
        locks.update(schema='orca.beauty-shape-lock/v2',evidence_sha256=manifest['evidence_sha256'],locks=[])
        for item in evidence['shape_details']:
            if item['accepted_faces']:
                locks['locks'].append({'subject_id':item['subject_id'],'label':item['label'],'parent_label':item['label'],
                    'status':item['status'],'view_support':item['view_support'],'reasons':item['reasons'],
                    'locked_faces':item['accepted_faces'],'nested_faces':item.get('nested_faces',[]),
                    'locked_leaves':[[f,0,0] for f in item['accepted_faces']],
                    'nested_leaves':[[f,0,0] for f in item.get('nested_faces',[])]})
    original = {(item['subject_id'],item['label']):item for item in evidence['shape_details'] if item['accepted_faces']}
    neighbors = surface_neighbors(vertices,faces)
    views = load_views(run/'baseline/replay/face-views')
    split = [LeafKey(*key) for key in locks.get('leaf_domain',{}).get('split_leaves',[])]
    budget = min(20000,len(faces)*2//100) - len(split) + len({key.source_face_id for key in split})
    audits = []
    for lock in locks['locks']:
        label = lock['label']
        if label not in labels:
            continue
        original_detail = original[lock['subject_id'],label]
        accepted = set(original_detail['accepted_faces'])
        nested = set(original_detail.get('nested_faces',[]))
        boundary,ring = boundary_band(accepted,neighbors)
        others = {f for item in locks['locks'] if item['label'] != label for f in item['locked_faces']}
        legal = set()
        for region in evidence['regions']:
            if region['subject_id'] == lock['subject_id'] and region['label'] in ('face',label):
                legal.update(int(s[0]) for s in region['samples'])
        blocked = set(original_detail['rejected_faces']) | others
        permitted = refinement_roots(accepted,neighbors,legal,blocked | nested)
        if label in ('le','re'):
            projections,view_audit,pinned = eye_views(label,views,vertices,faces)
        elif label in ('lb','rb'):
            projections,view_audit,pinned = brow_views(label,views,vertices,faces,accepted,accepted-boundary,permitted,legal)
        else:
            raise ValueError('Only approved eye and brow boundaries may be refined')
        permitted -= pinned
        kept = [LeafKey(f) for f in accepted-permitted]
        rows = []
        for face in sorted(permitted):
            leaves,status,error = refine_root(face,vertices[faces[face]],projections,face in accepted,budget)
            added = len(leaves)-1
            budget -= added
            if added:
                split.extend(key for key,_ in leaves)
            kept.extend(key for key,keep in leaves if keep)
            rows.append({'face_id':face,'status':status,'leaf_count':len(leaves),'accepted_leaf_count':sum(keep for _,keep in leaves),
                         'contour_error_pixels_1024':error,'was_r4':face in accepted})
        # Local disagreement must never erase an approved complete detail.
        if not kept:
            raise ValueError('Boundary refinement erased the R4 core')
        kept.sort()
        lock['locked_leaves'] = [key.encode() for key in kept]
        lock['locked_faces'] = sorted({key.source_face_id for key in kept})
        lock['nested_leaves'] = [[f,0,0] for f in sorted(nested)]
        lock['nested_faces'] = sorted(nested)
        audits.append({'label':label,'boundary_roots':len(boundary),'one_ring_roots':len(ring),'core_roots_unchanged':len(accepted-boundary),
            'pinned_corner_roots':sorted(pinned & accepted),'r4_roots':len(accepted),'accepted_roots':len(lock['locked_faces']),
            'accepted_leaves':len(kept),'added_roots':sorted(set(lock['locked_faces'])-accepted),
            'removed_roots':sorted(accepted-set(lock['locked_faces'])),'view_audit':view_audit,'faces':rows,
            'status_counts':dict(Counter(row['status'] for row in rows))})
    split.sort()
    locks['leaf_domain'] = domain(geometry,len(faces),split)
    locks['leaf_mapping_sha256'] = digest(locks['leaf_domain'])
    locks['baseline_sha256'] = manifest['evidence_sha256']
    modules = ('beauty_leaf_domain.py','local_leaf_boundaries.py','portrait_r5_boundaries.py')
    policy = {'policy':ALGORITHM_VERSION,'roi_size':1024,'max_depth':4,'max_edge_pixels':1.,
        'topology_rings':1,'corner_pinned':True,'source_refiner':'R4_EYE_PCHIP',
        'modules':{name:sha(Path(__file__).with_name(name)) for name in modules},
        'dependencies':{name:importlib.metadata.version(name) for name in ('numpy','scipy','opencv-python')}}
    policy['brow'] = {'iterations':BROW_ITERATIONS,'min_seed_pixels':BROW_MIN_SEED,
                     'delta_l':BROW_SOURCE_DELTA_L,'smooth_sigma_1024':BROW_SMOOTH_SIGMA,'topology_rings':1}
    locks['boundary_policy_sha256'] = digest(policy)
    if sha(source) != manifest['source_sha256']:
        raise ValueError('Source changed during replay')
    output.mkdir(parents=True,exist_ok=False)
    publish(output/'shape-locks.json',locks)
    publish(output/'stage-report.json',{'schema':'orca.r5-boundary-report/v1','code_status':'PASS','visual_status':'PENDING_USER',
        'labels':labels,'refined_labels':sorted(set(labels) | set(json.loads((previous/'stage-report.json').read_text()).get('refined_labels',[])) if previous else set(labels)),
        'boundary_policy':policy,'details':audits,'added_triangles':min(20000,len(faces)*2//100)-budget,
        'triangle_budget':min(20000,len(faces)*2//100),'leaf_mapping_sha256':locks['leaf_mapping_sha256'],
        'shape_lock_sha256':sha(output/'shape-locks.json'),'source_sha256':manifest['source_sha256'],
        'geometry_id':geometry,'source_changed':False,'geometry_changed':False,'new_recognition_performed':False,
        'material_tree_changed':False,'production_enabled':False})
    print(json.dumps([{key:detail[key] for key in ('label','r4_roots','accepted_roots','accepted_leaves','status_counts')}
                      for detail in audits]),flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--labels',nargs='+',default=['le','re'])
    parser.add_argument('--previous',type=Path)
    args = parser.parse_args()
    replay(args.run.resolve(),args.output.resolve(),args.labels,args.previous)
