"""Read-only barycentric cell lookup and palette application for R7 review."""
from collections import defaultdict
import json
from pathlib import Path
import re

import numpy as np

from portrait_r5_baseline import sha
from portrait_r5_visual_review import publish


def load(folder, expected):
    sidecar=json.loads((folder/'shape-locks.json').read_text())
    if sidecar['schema']!='orca.beauty-shape-lock/v3':
        raise ValueError('Unsupported contour lock')
    for key,value in expected.items():
        if sidecar.get(key)!=value:
            raise ValueError('Contour lock identity drift')
    ref=sidecar['partition_ref']
    if not re.fullmatch('[0-9a-f]{64}',ref['sha256']) or \
            ref['schema']!='orca.surface-partition-reference/v1' or \
            ref['path']!='surface-partitions/'+ref['sha256']+'.json':
        raise ValueError('Unsafe partition reference')
    path=folder/ref['path']
    if sha(path)!=ref['sha256']:
        raise ValueError('Partition content drift')
    document=json.loads(path.read_text())
    if document['schema']!='orca.surface-partition/v1':
        raise ValueError('Unsupported partition')
    for key,value in expected.items():
        if document.get(key)!=value:
            raise ValueError('Partition identity drift')
    if not 0<=document['added_triangles']<=document['triangle_budget']<=19536:
        raise ValueError('Partition budget exceeded')
    known={}
    roots=set()
    for face in document['faces']:
        root=face['source_face_id']
        if type(root) is not int or not 0<=root<document['face_count'] or root in roots:
            raise ValueError('Invalid source face')
        roots.add(root)
        for cell in face['cells']:
            if cell['id'] in known or not re.fullmatch('[0-9a-f]{64}',cell['id']):
                raise ValueError('Duplicate stable cell')
            for polygon in [cell['polygon']]+cell['holes']:
                p=np.asarray(polygon)
                if p.ndim!=2 or p.shape[1]!=3 or len(p)<3 or not np.isfinite(p).all() or \
                        np.min(p)<-2e-8 or np.max(np.abs(p.sum(1)-1))>1e-7:
                    raise ValueError('Cell leaves source face')
            known[cell['id']]=cell
    claimed=set()
    for lock in sidecar['locks']:
        ids=lock['locked_cells']
        if not ids or ids!=sorted(set(ids)) or claimed.intersection(ids) or \
                lock['status'] not in ('VALID_SHAPE','PROTECTED_SHAPE_UNCERTAIN') or lock['view_support']<2:
            raise ValueError('Invalid clipped lock')
        for cell_id in ids:
            cell=known[cell_id]
            if cell['subject_id']!=lock['subject_id'] or cell['label'] not in (lock['label'],'iris-'+lock['label']):
                raise ValueError('Clipped lock crosses parent or side')
        for cell_id in lock['nested_cells']:
            if cell_id not in ids or known[cell_id]['label']!='iris-'+lock['label']:
                raise ValueError('Nested iris crosses parent')
        claimed.update(ids)
    return document,sidecar


def inside(polygon, points):
    """Continuous barycentric membership, including deterministic edge hits."""
    polygon=np.asarray(polygon,dtype=float)[:,1:]
    points=np.asarray(points,dtype=float)[:,1:]
    result=np.zeros(len(points),dtype=bool)
    boundary=np.zeros(len(points),dtype=bool)
    x,y=points.T
    for a,b in zip(polygon,np.roll(polygon,-1,axis=0)):
        delta=b-a
        if abs(delta[1])>1e-12:
            result ^= ((a[1]>y)!=(b[1]>y)) & (x<delta[0]*(y-a[1])/delta[1]+a[0])
        cross=delta[0]*(y-a[1])-delta[1]*(x-a[0])
        dot=(x-a[0])*delta[0]+(y-a[1])*delta[1]
        boundary |= (np.abs(cross)<1e-10) & (dot>=-1e-10) & (dot<=float(delta@delta)+1e-10)
    return result|boundary


def inside_analytic(polygon, points):
    """Distance-normalized closed-edge tests remain valid for tiny clip segments."""
    polygon=np.asarray(polygon,dtype=float)[:,1:]
    points=np.asarray(points,dtype=float)[:,1:]
    result=np.zeros(len(points),dtype=bool)
    boundary=np.zeros(len(points),dtype=bool)
    x,y=points.T
    tolerance=2e-9
    for a,b in zip(polygon,np.roll(polygon,-1,axis=0)):
        delta=b-a
        length=float(np.linalg.norm(delta))
        if length<1e-15:
            continue
        if abs(delta[1])>1e-15:
            result ^= ((a[1]>y)!=(b[1]>y)) & (x<delta[0]*(y-a[1])/delta[1]+a[0])
        cross=delta[0]*(y-a[1])-delta[1]*(x-a[0])
        dot=(x-a[0])*delta[0]+(y-a[1])*delta[1]
        boundary |= (np.abs(cross)<=tolerance*length) & (dot>=-tolerance*length) & \
                    (dot<=length*length+tolerance*length)
    return result|boundary


def pixel_cells(document, ids, bary, groups, analytic_roots=()):
    mapping=np.full(ids.shape,-1,dtype=np.int32)
    lookup=[]
    weights=bary.reshape(-1,3)
    for face in document['faces']:
        contains=inside_analytic if face['source_face_id'] in analytic_roots else inside
        pixels=groups.get(face['source_face_id'])
        for cell in face['cells']:
            index=len(lookup); lookup.append(dict(cell,source_face_id=face['source_face_id'],face_status=face['status']))
            if pixels is None: continue
            mask=contains(cell['polygon'],weights[pixels])
            for hole in cell['holes']: mask &= ~contains(hole,weights[pixels])
            # Exact boundary points can belong to two closed cells. Stable ID
            # order supplies a reproducible display tie, without mixing RGB.
            selected=pixels[mask & (mapping.ravel()[pixels]<0)]
            mapping.ravel()[selected]=index
        if pixels is not None and np.any(mapping.ravel()[pixels]<0):
            raise ValueError('Raster hit not covered by clipped source partition')
    return mapping,lookup


def color_plan(document, palette, mode='combined'):
    uids={s['uid']:i for i,s in enumerate(palette)}
    cells=[]
    for face in document['faces']:
        for cell in face['cells']:
            label=cell['label']
            role='portrait-dark' if label in ('lb','rb') or label.startswith(('iris-','periocular-')) else \
                 'portrait-light' if label in ('le','re') else \
                 ('portrait-lips' if len(palette)>3 else 'portrait-dark') if label in ('ulip','llip') else \
                 'portrait-skin' if label=='face' and cell.get('fallback_label','face')!='face' else None
            if face['status'].endswith('FALLBACK'):
                role=None
            if role and role not in uids:
                raise ValueError('Missing fixed palette role')
            cells.append(dict(id=cell['id'],source_face_id=face['source_face_id'],label=label,
                              slot_uid=role,slot=uids[role] if role else None,
                              color_source='FIXED_R6_ROLE' if role else 'R6_INHERITED',
                              original_slot=None,retain_reason=None if role else 'UNRESOLVED_OR_NON_TARGET_KEEP_R6'))
    return dict(schema='orca.portrait-color-plan/v1',iteration='R7',mode=mode,
                geometry_id=document['geometry_id'],source_sha256=document['source_sha256'],
                boundary_sha256=document['partition_sha256'],palette=palette,
                weights={'source_error':1,'continuity':2,'ordinary_speckle':3,'recognizability':4,'adjacent_contrast':3},
                cells=cells,oral_auto_color=False,manual_priority=True,production_enabled=False,
                unused_slots=[s['uid'] for s in palette if s['uid'] not in {c['slot_uid'] for c in cells}])


def apply(r6, mapping, lookup, plan, old_labels, mode='combined', manual=None):
    result=r6.copy()
    palette={s['uid']:np.rint(np.asarray(s['rgb'])*255).astype(np.uint8) for s in plan['palette']}
    decisions={c['id']:c for c in plan['cells']}
    colors=np.zeros((len(lookup),3),dtype=np.uint8)
    enabled=np.zeros(len(lookup),dtype=bool)
    restored=np.zeros(len(lookup),dtype=bool)
    brows=np.zeros(len(lookup),dtype=bool)
    for index,cell in enumerate(lookup):
        row=decisions[cell['id']]
        if not row['slot_uid']: continue
        if mode=='clipping-only' and cell['label'].startswith('periocular-'):
            continue
        colors[index]=palette[row['slot_uid']]
        enabled[index]=True
        restored[index]=cell['label'].startswith('periocular-')
        brows[index]=cell['label'] in ('lb','rb')
    valid=mapping>=0
    selected=valid.copy()
    selected[valid]=enabled[mapping[valid]]
    if mode=='details-only':
        selected[valid] &= restored[mapping[valid]] | (brows[mapping[valid]] & (old_labels[valid]!=3))
    result[selected]=colors[mapping[selected]]
    if manual is not None:
        mask,rgb=manual; result[mask]=rgb[mask]
    return result
