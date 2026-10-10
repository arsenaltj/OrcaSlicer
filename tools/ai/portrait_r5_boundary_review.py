"""Check an immutable R5 boundary artifact and render its actual leaf selection."""
import argparse
import json
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

from beauty_leaf_domain import LeafKey, digest, domain, validate_keys
from local_face_landmarks import surface_neighbors
from local_leaf_boundaries import boundary_band
from local_semantic_geometry import read
from portrait_r5_baseline import sha
from portrait_r5_boundaries import load_views, publish


def leaf_mask(ids, bary, leaves):
    result = np.zeros(ids.shape,dtype=bool)
    flat = ids.ravel()
    order = np.argsort(flat,kind='stable')
    unique,starts,counts = np.unique(flat[order],return_index=True,return_counts=True)
    groups = {int(face):order[start:start+count] for face,start,count in zip(unique,starts,counts) if face >= 0}
    for key in leaves:
        indices = groups.get(key.source_face_id)
        if indices is None:
            continue
        local = bary.reshape(-1,3)[indices] @ np.linalg.inv(key.corners())
        result.ravel()[indices[np.min(local,axis=1) >= -1e-6]] = True
    return result


def validate_lock(lock, original, legal, blocked):
    for field in ('subject_id','label','status','view_support','reasons'):
        if lock[field] != original[field]:
            raise ValueError('R4 lock metadata changed: '+field)
    if lock['status'] not in ('VALID_SHAPE','PROTECTED_SHAPE_UNCERTAIN') or lock['view_support'] < 2:
        raise ValueError('Unusable boundary lock')
    if lock['parent_label'] != original['label']:
        raise ValueError('Boundary semantic parent changed')
    keys = [LeafKey(*key) for key in lock['locked_leaves']]
    nested = [LeafKey(*key) for key in lock['nested_leaves']]
    if not keys or sorted({key.source_face_id for key in nested}) != lock['nested_faces']:
        raise ValueError('Empty lock or nested root summary drift')
    roots = {key.source_face_id for key in keys}
    if not roots <= (set(original['accepted_faces']) | set(legal)) or roots & set(blocked):
        raise ValueError('Boundary crosses a protected or unbound semantic parent')


def verify(run,stage):
    manifest = json.loads((run/'stage-manifest.json').read_text())
    document = json.loads((stage/'shape-locks.json').read_text())
    report = json.loads((stage/'stage-report.json').read_text())
    if sha(stage/'shape-locks.json') != report['shape_lock_sha256']:
        raise ValueError('Boundary sidecar changed')
    evidence = json.loads((run/'baseline/replay/evidence.json').read_text())
    vertices,faces,geometry = read(run/'baseline/replay/native.bin',manifest['source_sha256'])
    if document['geometry_id'] != geometry or document['source_sha256'] != sha(run/'baseline/source.glb'):
        raise ValueError('Boundary source identity drift')
    if document['baseline_sha256'] != sha(run/'baseline/replay/evidence.json'):
        raise ValueError('Boundary baseline drift')
    for key in ('evidence_sha256','runtime_sha256','policy_sha256','face_count'):
        expected = manifest['evidence_sha256'] if key == 'evidence_sha256' else evidence[key]
        if document[key] != expected:
            raise ValueError('Boundary evidence identity drift: '+key)
    if digest(report['boundary_policy']) != document['boundary_policy_sha256']:
        raise ValueError('Boundary policy drift')
    split = [LeafKey(*key) for key in document['leaf_domain']['split_leaves']]
    mapping = domain(geometry,len(faces),split)
    if mapping != document['leaf_domain'] or digest(mapping) != document['leaf_mapping_sha256']:
        raise ValueError('Boundary mapping drift')
    neighbors = surface_neighbors(vertices,faces)
    old = {(item['subject_id'],item['label']):item for item in evidence['shape_details'] if item['accepted_faces']}
    if {(item['subject_id'],item['label']) for item in document['locks']} != set(old):
        raise ValueError('Boundary detail identity drift')
    partition = set(split)
    split_roots = {key.source_face_id for key in split}
    claimed = set()
    summaries = []
    for lock in document['locks']:
        keys = [LeafKey(*key) for key in lock['locked_leaves']]
        nested = [LeafKey(*key) for key in lock['nested_leaves']]
        validate_keys(keys,len(faces))
        validate_keys(nested,len(faces))
        if not set(nested) <= set(keys) or set(keys) & claimed:
            raise ValueError('Nested ownership or cross-detail overlap')
        for key in keys:
            if key.source_face_id in split_roots and key not in partition:
                raise ValueError('Lock key is not a final leaf')
            if key.source_face_id not in split_roots and key.depth:
                raise ValueError('Lock key has no partition')
        if sorted({key.source_face_id for key in keys}) != lock['locked_faces']:
            raise ValueError('Root summary drift')
        original = old[lock['subject_id'],lock['label']]
        legal = {int(s[0]) for region in evidence['regions']
                 if region['subject_id'] == lock['subject_id'] and region['label'] in ('face',lock['label'])
                 for s in region['samples']}
        blocked = set(original['rejected_faces']) | {f for key,item in old.items()
                  if key != (lock['subject_id'],lock['label']) for f in item['accepted_faces']}
        validate_lock(lock,original,legal,blocked)
        nested_r4 = [LeafKey(f) for f in original.get('nested_faces',[])]
        if nested != nested_r4:
            raise ValueError('R4 iris changed')
        accepted = set(original['accepted_faces'])
        if lock['label'] not in report['refined_labels'] and keys != [LeafKey(f) for f in sorted(accepted)]:
            raise ValueError('Untouched detail changed')
        boundary,ring = boundary_band(accepted,neighbors)
        changed_roots = {f for f in accepted | set(lock['locked_faces'])
                         if (LeafKey(f) in set(keys)) != (f in accepted)}
        if not changed_roots <= ring:
            raise ValueError('Boundary changed outside one ring')
        if not {LeafKey(f) for f in accepted-boundary} <= set(keys):
            raise ValueError('R4 interior core changed')
        claimed.update(keys)
        summaries.append({'label':lock['label'],'root_count':len(lock['locked_faces']),
                          'leaf_count':len(keys),'nested_count':len(nested),'changed_roots':len(changed_roots)})
    return document,summaries


def review(run,stage,output):
    document,summaries = verify(run,stage)
    evidence = json.loads((run/'baseline/replay/evidence.json').read_text())
    original = {(item['subject_id'],item['label']):item for item in evidence['shape_details'] if item['accepted_faces']}
    output.mkdir(parents=True,exist_ok=False)
    for index,view in enumerate(load_views(run/'baseline/replay/face-views')):
        projection = view.boundary
        if projection is None:
            continue
        panels = []
        for mode in ('source','R4','R5'):
            rgb = projection.rgb.copy()
            if mode != 'source':
                for lock in document['locks']:
                    label = lock['label']
                    if label not in ('le','re','lb','rb'):
                        continue
                    keys = [LeafKey(f) for f in original[lock['subject_id'],label]['accepted_faces']] if mode == 'R4' else \
                           [LeafKey(*key) for key in lock['locked_leaves']]
                    mask = leaf_mask(projection.ids,projection.barycentric,keys)
                    nested = leaf_mask(projection.ids,projection.barycentric,[LeafKey(f) for f in lock['nested_faces']])
                    color = [55,220,120] if label in ('le','re') else [235,180,35]
                    rgb[mask & ~nested] = (rgb[mask & ~nested]*.45 + np.array(color)*.55).astype(np.uint8)
            panels.append(Image.fromarray(rgb).resize((512,512)))
        canvas = Image.new('RGB',(1536,540),'white')
        draw = ImageDraw.Draw(canvas)
        for i,panel in enumerate(panels):
            canvas.paste(panel,(i*512,28))
            draw.text((i*512+8,8),('Source texture','R4 locks','R5 leaf locks')[i],fill='black')
        canvas.save(output/f'{index:02d}.png')
    publish(output/'checks.json',{'status':'PASS','visual_status':'PENDING_USER','details':summaries,
                                'shape_lock_sha256':sha(stage/'shape-locks.json')})
    print(json.dumps(summaries),flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run',type=Path,required=True)
    parser.add_argument('--stage',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    args = parser.parse_args()
    review(args.run.resolve(),args.stage.resolve(),args.output.resolve())
