"""Complete local parent views and exact mesh visibility in independent new caches."""
import argparse
from collections import Counter
import hashlib
import importlib.metadata
import json
from pathlib import Path
import struct
import subprocess

import numpy as np

from beauty_leaf_domain import digest
from local_parent_boundary import PARENT_CLASSES,refine
from local_semantic_geometry import read
from portrait_r5_baseline import sha
from portrait_r5_visual_review import pixel_groups,publish
from portrait_r9_replay import PARENTS,baseline,bound_partition,native_children,observations
from portrait_surface_ownership import trusted_regions
from portrait_surface_ownership_v2 import (bound_subject,confirmed_r6_units,shape_conflicts,
                                          source_parent_seeds,validate)
from surface_partition import pixel_cells


def fixed_seed_unit(unit,confirmed_r6=()):
    return unit.get('evidence_source')=='R9_VERIFIED_SAME_SOURCE_PARENT_SEED' or \
        unit.get('evidence_source')=='FROZEN_R6_CONFIRMED_PARENT' and unit.get('id') in confirmed_r6


def source_reference(run,evidence,owner,conflicts,explicit,confirmed_r6):
    from portrait_r6_repair import load_render
    children=native_children(json.loads((run/'baseline/native-analysis.json').read_text()))
    seeds={face:(parent,proof) for face,(parent,proof) in source_parent_seeds(evidence).items()
           if face not in conflicts and face not in explicit and
           all(PARENTS.get(label)==parent for _,label in children[face])}
    renderer,_,faces,uv,colors,materials,material_ids=load_render(run)
    references={face:parent for face,(parent,_) in seeds.items()}
    detail_roots={face for region in evidence['regions'] if region['label'] in
                  ('le','re','lb','rb','ulip','llip','imouth','teeth','iris','lip-line-corner')
                  for face,*_ in region['samples']}
    for region in evidence['regions']:
        if region['label']!='cloth':continue
        for face,confidence,dominance,_,support in region['samples']:
            if confidence>=.9 and dominance>=.95 and support>=2 and face not in conflicts and \
                    face not in explicit and face not in detail_roots and \
                    all(PARENTS.get(label)=='cloth' for _,label in children[face]):
                references[face]='cloth'
    ids=np.array(sorted(references),np.int64)[None,:]
    rgb=renderer.shade(faces,uv,colors,materials,material_ids,ids,np.full((*ids.shape,3),1/3))[0]
    labels=np.array([PARENT_CLASSES[references[int(face)]] for face in ids[0]],np.uint8)
    extra=[unit for region in owner['regions'] for unit in region['units']
           if unit['id'] in confirmed_r6 and unit['source_face_id'] not in conflicts]
    for unit in extra:
        values=np.asarray(unit['source_rgb'])
        if values.shape!=(3,) or not np.isfinite(values).all() or np.any(values<0) or np.any(values>1):
            raise ValueError('Original parent reference is not verified source RGB')
    if extra:
        rgb=np.concatenate((rgb,np.rint(np.array([u['source_rgb'] for u in extra])*255).astype(np.uint8)))
        labels=np.r_[labels,[PARENT_CLASSES[u['parent_label']] for u in extra]].astype(np.uint8)
    return (rgb[:,None,:],labels[:,None]),seeds


def prepare(run,evidence,partition,ownership,output):
    manifest,old,_,plans,freeze=baseline(run)
    document,locks=bound_partition(run,partition)
    evidence_document=json.loads((run/'baseline/evidence.json').read_text())
    bound_subject(evidence_document,locks)
    vertices,faces,_=read(run/'baseline/native.bin',manifest['source_sha256'])
    centers=vertices[faces].mean(1)
    floor=vertices[:,2].min()+np.ptp(vertices[:,2])*.1
    trusted,conflicts=trusted_regions(evidence_document)
    conflicts |= shape_conflicts(evidence_document)
    root_labels=np.zeros(len(faces),np.uint8)
    owner=json.loads((ownership/'surface-ownership.json').read_text())
    validate(owner,document,locks,freeze['fingerprint'])
    explicit={face['source_face_id'] for face in document['faces']}
    confirmed_r6=confirmed_r6_units(json.loads((run/'baseline/surface-ownership.json').read_text()),document,owner)
    reference,seeds=source_reference(run,evidence_document,owner,conflicts,explicit,confirmed_r6)
    for face,(parent,_) in seeds.items():root_labels[face]=PARENT_CLASSES[parent]
    root_proposals=np.zeros(len(faces),np.uint8);cell_proposals={}
    cell_labels={}
    for region in owner['regions']:
        for unit in region['units']:
            if unit['source_face_id'] in conflicts:
                continue
            fixed=fixed_seed_unit(unit,confirmed_r6) and (unit['evidence_source']=='FROZEN_R6_CONFIRMED_PARENT' or
                    unit['source_face_id'] in seeds and seeds[unit['source_face_id']][0]==region['parent_label'])
            if not fixed:
                if len(unit['view_ids'])>=2:
                    if unit['implicit_root']:root_proposals[unit['source_face_id']]=PARENT_CLASSES[region['parent_label']]
                    else:cell_proposals[unit['id']]=PARENT_CLASSES[region['parent_label']]
                continue
            if unit['implicit_root']:root_labels[unit['source_face_id']]=PARENT_CLASSES[region['parent_label']]
            else:cell_labels[unit['id']]=PARENT_CLASSES[region['parent_label']]
    output.mkdir(parents=True,exist_ok=False)
    records=[]
    for index,(row,obs) in enumerate(observations(evidence,manifest)):
        valid=obs['ids']>=0
        foreground=valid.copy();foreground[valid]=centers[obs['ids'][valid],2]>floor
        known=np.zeros(obs['ids'].shape,np.uint8);known[valid]=root_labels[obs['ids'][valid]]
        proposals=np.zeros(obs['ids'].shape,np.uint8);proposals[valid]=root_proposals[obs['ids'][valid]]
        mapping,lookup=pixel_cells(document,obs['ids'],obs['bary'],pixel_groups(obs['ids']),set(plans[3]['analytic_source_faces']))
        classes=np.array([cell_labels.get(c['id'],0) for c in lookup],np.uint8)
        explicit=mapping>=0;confirmed=explicit.copy();confirmed[explicit]=classes[mapping[explicit]]>0
        known[confirmed]=classes[mapping[confirmed]]
        proposed=np.array([cell_proposals.get(c['id'],0) for c in lookup],np.uint8)
        proposals[explicit]=proposed[mapping[explicit]]
        labels,quality,audit=refine(obs['rgb'],foreground,obs['labels'],obs['confidence'],known,proposals,reference)
        path=output/f'{index:02d}.npz'
        with path.open('xb') as stream:
            np.savez_compressed(stream,rgb=obs['rgb'],ids=obs['ids'],bary=obs['bary'],labels=labels,confidence=quality,
                model_labels=obs['labels'],model_confidence=obs['confidence'],foreground=foreground,seed_labels=known,
                proposal_labels=proposals)
        records.append(dict(path=path.name,sha256=sha(path),camera=row['camera'],family=row['family'],audit=audit,
            source_observation_sha256=row['sha256'],class_pixels={str(k):int((labels==k).sum()) for k in (1,3,4)}))
        print(json.dumps(dict(view=row['camera']['view'],scope=row['camera']['scope'],class_pixels=records[-1]['class_pixels'],
                             overlapping_pixels=audit['overlap_pixels'])),flush=True)
    policy=dict(algorithm='r9-source-parent-boundary/v7',source_evidence_sha256=sha(evidence/'evidence.json'),
        confirmed_ownership_sha256=sha(ownership/'surface-ownership.json'),iterations=5,
        fixed_source_seeds='original-verified-semantic-or-original-CONFIRMED_PARENT-only',
        original_confirmed_r6_seed_units=len(confirmed_r6),
        supported_parent_proposals_are_fixed_seeds=False,
        earlier_model_or_refinement_claims_are_fixed_seeds=False,
        global_reference='original-pure-source-UV-and-confirmed-R6-source-samples',
        original_cloth_reference=dict(confidence=.9,dominance=.95,view_support=2,
                                      role='source-color-reference-only-not-fixed-authority'),
        reference_parent_counts={str(c):int((reference[1]==c).sum()) for c in (1,3,4)},
        reference_sha256=digest(dict(rgb=reference[0].tolist(),labels=reference[1].tolist())),
        supplemental_claims='two-view-proposal-with-all-competing-source-GMM-margin-2',
        supplemental_claims_use_gc_probable_foreground_only=True,
        competing_source_mask_log_likelihood_margin=2.,
        model_seed_source_validation='verified-parent-GMM-margin-2-demotes-only',
        overlap_resolution='verified-parent-source-GMM-before-proposal-GMM',
        threshold_lowered=False,frozen_features_consumed=False,source_colors_only=True,
        confidence_field='binary_source_mask_coverage',packages={n:importlib.metadata.version(n) for n in ('numpy','opencv-python')},
        modules={n:sha(Path(__file__).with_name(n)) for n in ('local_parent_boundary.py','portrait_r9_parent_evidence.py')})
    publish(output/'evidence.json',dict(schema='orca.portrait-parent-observations/v1',
        identity={k:manifest[k] for k in ('source_sha256','geometry_id','face_count','evidence_sha256')},
        detail_freeze_sha256=freeze['fingerprint'],policy=policy,policy_sha256=digest(policy),observations=records,
        source_refinement_performed=True,local_inference_performed=False,paid_generation_performed=False,production_enabled=False))


def cell_samples(document):
    from portrait_parent_projection import SAMPLES
    records=[]
    for face in document['faces']:
        for cell in face['cells']:
            triangles=np.asarray(cell['triangles'])
            areas=np.linalg.norm(np.cross(triangles[:,1]-triangles[:,0],triangles[:,2]-triangles[:,0]),axis=1)
            triangle=triangles[int(np.argmax(areas))]
            samples=SAMPLES@triangle
            from surface_partition import inside_analytic
            if not np.isfinite(samples).all() or np.any(samples<-2e-8) or np.any(samples>1+2e-8) or \
                    np.any(np.abs(samples.sum(1)-1)>1e-8) or not inside_analytic(cell['polygon'],samples).all() or \
                    any(inside_analytic(hole,samples).any() for hole in cell['holes']):
                raise ValueError('Cell visibility samples leave their verified partition')
            records.append(dict(id=cell['id'],source_face_id=face['source_face_id'],samples=samples.tolist()))
    return sorted(records,key=lambda c:c['id'])


def reuse_root_visibility(folder,manifest,cameras,native_sha256):
    request=json.loads((folder/'request.json').read_text())
    receipt=json.loads((folder/'receipt.json').read_text())
    if request.get('schema')!='orca.parent-visibility-request/v1' or \
            receipt.get('request_sha256')!=sha(folder/'request.json') or \
            receipt.get('visibility_sha256')!=sha(folder/'visibility.bin') or \
            receipt.get('native_sha256')!=native_sha256:
        raise ValueError('Previous root visibility request, binary or receipt drift')
    previous=load_visibility(folder,manifest,request['cameras'])
    identities={digest(camera):index for index,camera in enumerate(request['cameras'])}
    if len(identities)!=len(request['cameras']):
        raise ValueError('Previous visibility has duplicate camera records')
    rows=np.zeros((len(cameras),manifest['face_count']),np.uint8)
    missing=[];reused=[]
    for index,camera in enumerate(cameras):
        old=identities.get(digest(camera))
        if old is None:missing.append(index)
        else:
            rows[index]=previous[old]
            reused.append(dict(view_index=index,previous_view_index=old,render_id=camera['render_id']))
    return rows,missing,reused


def visibility(run,evidence,output,tool,partition=None,previous=None):
    manifest,*_=baseline(run)
    cameras=[r['camera'] for r,_ in observations(evidence,manifest)]
    output.mkdir(parents=True,exist_ok=False)
    request=dict(schema='orca.parent-visibility-request/v1',geometry_path=str(run/'baseline/native.bin'),
        geometry_packet_sha256=sha(run/'baseline/native.bin'),
        **{k:manifest[k] for k in ('source_sha256','geometry_id','face_count')},cameras=cameras,
        barycentric_samples=[[1/3]*3,[.98,.01,.01],[.01,.98,.01],[.01,.01,.98],[.49,.49,.02],[.02,.49,.49],[.49,.02,.49]])
    cells=None
    if partition is not None:
        document,_=bound_partition(run,partition)
        cells=cell_samples(document)
        request.update(schema='orca.parent-cell-visibility-request/v1',cells=cells,partition_sha256=document['partition_sha256'])
    publish(output/'request.json',request)
    reused=[]
    if previous is not None:
        if cells is not None:raise ValueError('Changed cell partitions cannot reuse root visibility')
        rows,missing,reused=reuse_root_visibility(previous,manifest,cameras,sha(tool))
        if missing:
            fresh=output/'additional-cameras';fresh.mkdir()
            additional=dict(request,cameras=[cameras[index] for index in missing])
            publish(fresh/'request.json',additional)
            subprocess.run([str(tool),'--parent-visibility',str(fresh/'request.json'),str(fresh/'visibility.bin')],check=True)
            values=load_visibility(fresh,manifest,additional['cameras'])
            for index,value in zip(missing,values):rows[index]=value
        header=struct.pack('<8sQQ64s64s64s',b'ORCAPV01',manifest['face_count'],len(cameras),
            manifest['source_sha256'].encode(),manifest['geometry_id'].encode(),sha(output/'request.json').encode())
        content=header+rows.tobytes()
        with (output/'visibility.bin').open('xb') as stream:stream.write(content+hashlib.sha256(content).digest())
    else:
        subprocess.run([str(tool),'--parent-cell-visibility' if cells else '--parent-visibility',str(output/'request.json'),str(output/'visibility.bin')],check=True)
    load_visibility(output,manifest,cameras,cells,document['partition_sha256'] if cells else None)
    publish(output/'receipt.json',dict(request_sha256=sha(output/'request.json'),visibility_sha256=sha(output/'visibility.bin'),
        source_sha256=manifest['source_sha256'],geometry_id=manifest['geometry_id'],native_sha256=sha(tool),
        occlusion_check='closest_source_mesh_AABB_intersection',samples=7,views=len(cameras),
        reused_camera_records=reused,previous_visibility_sha256=sha(previous/'visibility.bin') if previous else None))


def load_visibility(folder,manifest,cameras,cells=None,partition_sha256=None):
    data=(folder/'visibility.bin').read_bytes()
    header=struct.Struct('<8sQQ64s64s64s')
    magic,faces,views,source,geometry,request=header.unpack_from(data)
    expected_count=len(cells) if cells is not None else manifest['face_count']
    if magic!=(b'ORCAPC01' if cells is not None else b'ORCAPV01') or faces!=expected_count or views!=len(cameras) or \
        source.decode()!=manifest['source_sha256'] or geometry.decode()!=manifest['geometry_id'] or \
        request.decode()!=sha(folder/'request.json') or len(data)!=header.size+faces*views+32 or \
        hashlib.sha256(data[:-32]).digest()!=data[-32:]:raise ValueError('Parent visibility identity, sample count or checksum drift')
    expected=json.loads((folder/'request.json').read_text())
    if expected['cameras']!=cameras:raise ValueError('Visibility cameras changed')
    for field in ('source_sha256','geometry_id','face_count'):
        if expected[field]!=manifest[field]:raise ValueError('Visibility request source identity changed')
    packet=next(row['sha256'] for row in manifest['files'] if row['path']=='baseline/native.bin')
    if expected['geometry_packet_sha256']!=packet:
        raise ValueError('Visibility geometry packet changed')
    from portrait_parent_projection import SAMPLES
    if expected['barycentric_samples']!=SAMPLES.tolist():raise ValueError('Visibility sample sequence changed')
    if cells is not None and (expected['cells']!=cells or not partition_sha256 or
                              expected['partition_sha256']!=partition_sha256):
        raise ValueError('Cell visibility geometry, partition or mapping changed')
    if np.any(np.frombuffer(data,dtype=np.uint8,count=faces*views,offset=header.size)>127):
        raise ValueError('Invalid source visibility sample bits')
    return np.frombuffer(data,dtype=np.uint8,count=faces*views,offset=header.size).reshape(views,faces)


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('stage',choices=('refine','visibility'))
    for name in ('run','evidence','output'):parser.add_argument('--'+name,type=Path,required=True)
    for name in ('partition','ownership','tool'):parser.add_argument('--'+name,type=Path)
    parser.add_argument('--previous',type=Path)
    args=parser.parse_args()
    if args.stage=='refine':prepare(args.run.resolve(),args.evidence.resolve(),args.partition.resolve(),args.ownership.resolve(),args.output.resolve())
    else:visibility(args.run.resolve(),args.evidence.resolve(),args.output.resolve(),args.tool.resolve(),
                    args.partition.resolve() if args.partition else None,args.previous.resolve() if args.previous else None)
