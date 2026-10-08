"""Palette-independent parent ownership on explicit cells and implicit source roots."""
from collections import defaultdict
import json
from math import gcd
import re

import numpy as np

from beauty_leaf_domain import digest
from local_leaf_boundaries import area, clip_triangle
from surface_detail_freeze import catalog
from surface_partition import inside_analytic


SCHEMA = 'orca.portrait-surface-ownership/v2'
POLICY = dict(algorithm='r9-cell-parent-ownership/v2', minimum_views=2, minimum_coverage=.9,
              confidence=.9, correlated_crops_are_one_vote=True, color_is_ownership=False,
              components='shared-positive-length-cell-edges', source_color='cell-pixels-when-visible',
              body_skin_scope='whole-person-except-pedestal', source_faces_immutable=True)


def root_id(document, face):
    return digest(dict(schema='orca.surface-root-cell/v1', geometry_id=document['geometry_id'],
                       source_sha256=document['source_sha256'], source_face_id=face))


def frozen_ids(locks):
    return {key for lock in locks['locks'] for key in lock['locked_cells']+lock.get('periocular_cells', [])}


def bound_subject(evidence,locks):
    subjects={lock['subject_id'] for lock in locks['locks']}
    if len(subjects)!=1 or subjects!=set(evidence['subjects']) or any(
            region['subject_id'] not in subjects for region in evidence['regions']):
        raise ValueError('Parent completion requires one verified same-source subject')
    return next(iter(subjects))


def shape_conflicts(evidence):
    return {face for shape in evidence.get('shape_details',[]) if shape['status']=='INVALID_SHAPE_CONFLICT'
            for face in shape['rejected_faces']}


def parent_semantic_blockers(labels):
    """Coarse parent predictions are priors; oral and dental identity is protected."""
    return set(labels) & {'imouth', 'teeth'}


def confirmed_r6_units(prior, partition, ownership):
    from beauty_leaf_domain import LeafKey
    known=defaultdict(list)
    for region in prior['regions']:
        if region['status']!='CONFIRMED_PARENT':continue
        for row in region['leaves']:
            known[row[0]].append((LeafKey(*row),'skin' if region['parent_label']=='face' else 'cloth'))
    cells={c['id']:c for face in partition['faces'] for c in face['cells']}
    result=set()
    for region in ownership['regions']:
        for unit in region['units']:
            if unit.get('evidence_source')!='FROZEN_R6_CONFIRMED_PARENT':continue
            cell=cells.get(unit['id'])
            for leaf,parent in known[unit['source_face_id']]:
                if parent!=region['parent_label']:continue
                if (cell is None and unit['implicit_root'] and leaf.depth==0) or \
                        (cell is not None and all(inside_analytic(leaf.corners(),np.asarray(t)).all()
                                                  for t in cell['triangles'])):
                    result.add(unit['id']);break
    return result


def source_parent_seeds(evidence):
    names={'face':'skin','nose':'skin','neck':'skin','lr':'skin','rr':'skin','hair':'hair','cloth':'cloth'}
    details={'le','re','lb','rb','ulip','llip','imouth','teeth','iris','lip-line-corner'}
    regions=defaultdict(set);proofs=defaultdict(list);protected=shape_conflicts(evidence)
    for region in evidence['regions']:
        for face,confidence,dominance,samples,support in region['samples']:
            if region['label'] in details:
                protected.add(face)
            parent=names.get(region['label'])
            if parent and confidence>=.95 and dominance>=.95 and support>=2:
                regions[face].add(parent)
                proofs[face].append(dict(label=region['label'],confidence=confidence,dominance=dominance,
                                         view_support=support,source_samples=samples))
    return {face:(next(iter(parents)),proofs[face]) for face,parents in regions.items()
            if len(parents)==1 and face not in protected}


def validate(document, partition, locks, freeze_sha):
    for key in ('source_sha256', 'geometry_id', 'evidence_sha256', 'face_count'):
        if document[key] != partition[key]:
            raise ValueError('Cell ownership source identity drift')
    if document['schema'] != SCHEMA or document['partition_sha256'] != partition['partition_sha256'] or \
            document['partition_ref'] != locks['partition_ref'] or document['detail_freeze_sha256'] != freeze_sha or \
            document['policy_sha256'] != digest(document['policy']):
        raise ValueError('Cell ownership partition, freeze or policy drift')
    cells = catalog(partition)
    explicit_roots = {face['source_face_id'] for face in partition['faces']}
    frozen = frozen_ids(locks)
    subjects = {lock['subject_id'] for lock in locks['locks']}
    claimed, region_ids = set(), set()
    for region in document['regions']:
        if region['id'] in region_ids or region['subject_id'] not in subjects or \
                region['parent_label'] not in ('skin', 'hair', 'cloth') or \
                region['status'] not in ('CONFIRMED_PARENT', 'SUPPORTED_PARENT_PROPOSAL') or \
                len(set(region['view_ids'])) < 2 or region['view_ids'] != sorted(set(region['view_ids'])):
            raise ValueError('Conflicting, unbound or single-view cell ownership')
        region_ids.add(region['id'])
        if not region['units']:
            raise ValueError('Empty parent component')
        for unit in region['units']:
            key, face = unit['id'], unit['source_face_id']
            if key in claimed or key in frozen or not re.fullmatch('[0-9a-f]{64}', key) or \
                    type(face) is not int or not 0<=face<partition['face_count']:
                raise ValueError('Duplicate, frozen or invalid parent unit')
            claimed.add(key)
            support = unit.get('view_ids', region['view_ids'])
            if len(support)<2 or support!=sorted(set(support)) or '' in support:
                raise ValueError('Parent unit has insufficient independent support')
            if face in explicit_roots:
                if key not in cells or cells[key][0] != face or cells[key][1]['subject_id'] != region['subject_id'] or \
                        cells[key][1]['label'] not in ('face', 'R6', 'skin', 'hair', 'cloth'):
                    raise ValueError('Parent ownership crosses a detail or mixed sibling')
            elif key != root_id(partition, face) or not unit.get('implicit_root'):
                raise ValueError('Implicit parent source mapping drift')
    return document


def combine_votes(votes, minimum=2):
    """One class per independent camera, even when close-up crops overlap."""
    combined = defaultdict(dict)
    conflicts = set()
    for family, key, parent, quality in votes:
        if quality < .9:
            continue
        old = combined[key].get(family)
        if old is not None and old != parent:
            conflicts.add(key)
        else:
            combined[key][family] = parent
    accepted = {}
    for key, families in combined.items():
        labels = set(families.values())
        if key not in conflicts and len(labels)==1 and len(families)>=minimum:
            accepted[key] = (next(iter(labels)), sorted(families))
        elif len(labels)>1:
            conflicts.add(key)
    return accepted, conflicts


def analytic_coverage(polygon, labels, confidence, parent):
    """Integrate continuous polygon/pixel intersections; a leaf need not fill a pixel."""
    polygon = np.asarray(polygon, dtype=float)
    total = area(polygon)
    if total < 1e-12:
        return 0.
    classes = {'skin': (2, 3), 'hair': (1,), 'cloth': (4,)}[parent]
    lo = np.maximum(np.floor(polygon.min(0)).astype(int), 0)
    hi = np.minimum(np.ceil(polygon.max(0)).astype(int), labels.shape[::-1])
    covered = 0.
    for y in range(lo[1], hi[1]):
        for x in range(lo[0], hi[0]):
            if labels[y, x] in classes and confidence[y, x]>=.9:
                square = np.array([[x,y], [x+1,y], [x+1,y+1], [x,y+1]], dtype=float)
                covered += area(clip_triangle(polygon, square))
    return min(1., covered/total)


def unit_pixels(partition, ids, bary, groups, analytic_roots=()):
    from surface_partition import pixel_cells
    mapping, lookup = pixel_cells(partition, ids, bary, groups, set(analytic_roots))
    return mapping, lookup


def connected_units(units, cells, source_vertices):
    """Join same-parent cells across shared edges, not merely a shared root face."""
    parents={key:key for key in units}
    def find(key):
        while parents[key]!=key:
            parents[key]=parents[parents[key]];key=parents[key]
        return key
    edges=defaultdict(list)
    scale=1000000000
    for key,unit in units.items():
        face=unit['source_face_id'];parent=unit['parent_label']
        cell=cells.get(key,dict(polygon=np.eye(3).tolist(),holes=[]))
        for contour in [cell['polygon']]+cell.get('holes',[]):
            points=np.rint(np.asarray(contour)*scale).astype(np.int64).tolist()
            for a,b in zip(points,points[1:]+points[:1]):
                zero=next((i for i in range(3) if a[i]==0 and b[i]==0),None)
                if zero is not None:
                    j,k=(zero+1)%3,(zero+2)%3
                    vj,vk=map(int,(source_vertices[face,j],source_vertices[face,k]))
                    line=(parent,'seam',min(vj,vk),max(vj,vk))
                    start,end=(a[k],b[k]) if vj<vk else (a[j],b[j])
                else:
                    dx,dy=b[1]-a[1],b[2]-a[2]
                    divisor=gcd(abs(dx),abs(dy))
                    if not divisor:continue
                    dx,dy=dx//divisor,dy//divisor
                    if dx<0 or (dx==0 and dy<0):dx,dy=-dx,-dy
                    line=(parent,'internal',face,dx,dy,dx*a[2]-dy*a[1])
                    start,end=dx*a[1]+dy*a[2],dx*b[1]+dy*b[2]
                if start!=end:edges[line].append((min(start,end),max(start,end),key))
    for segments in edges.values():
        active=[]
        for lo,hi,key in sorted(segments):
            active=[(end,other) for end,other in active if end>lo]
            for _,other in active:
                a,b=find(key),find(other)
                if a!=b:parents[max(a,b)]=min(a,b)
            active.append((hi,key))
    groups=defaultdict(list)
    for key in units:groups[(units[key]['parent_label'],find(key))].append(key)
    return [(parent,sorted(keys)) for (parent,_),keys in sorted(groups.items())]


def safe_reference(ref):
    key = ref.get('sha256', '')
    return bool(re.fullmatch('[0-9a-f]{64}', key)) and ref.get('schema')=='orca.portrait-surface-ownership-reference/v2' and \
        ref.get('path')=='portrait-ownership/'+key+'.json'
