"""Cell-safe R9 parent repair; reviewed features and undecided siblings retain R8."""
import argparse
from collections import Counter, defaultdict
from copy import deepcopy
import json
from pathlib import Path
import shutil
import subprocess

import numpy as np

from beauty_leaf_domain import LeafKey, digest
from local_face_landmarks import surface_neighbors
from local_semantic_geometry import read
from portrait_r5_baseline import sha
from portrait_r5_visual_review import assignments, publish
from portrait_r6_repair import verify
from portrait_r6_review import masks
from portrait_r9_evidence import camera_family
from portrait_surface_ownership import partition as leaf_partition, trusted_regions
from portrait_surface_ownership_v2 import (POLICY, SCHEMA, analytic_coverage, combine_votes,
                                         bound_subject, connected_units, frozen_ids, root_id, shape_conflicts,
                                         parent_semantic_blockers,source_parent_seeds,validate)
from surface_detail_freeze import catalog, validate as validate_freeze
from surface_partition import inside_analytic, pixel_cells


PARENTS = {2: 'hair', 3: 'skin', 4: 'skin', 5: 'cloth'}


def baseline(run):
    manifest = verify(run)
    document = json.loads((run/'baseline/r8-partition.json').read_text())
    locks = json.loads((run/'baseline/r8-shape-locks.json').read_text())
    plans = {n: json.loads((run/f'baseline/r8-colors-{n}/portrait-color-plan.json').read_text()) for n in (3,4,5,6)}
    freeze = json.loads((run/'baseline/detail-freeze.json').read_text())
    validate_freeze(document, locks, plans, freeze)
    return manifest, document, locks, plans, freeze


def observations(folder, expected):
    report = json.loads((folder/'evidence.json').read_text())
    keys=('source_sha256','geometry_id','face_count','evidence_sha256')
    if report['identity'] != {k: expected[k] for k in keys} or \
            report['policy_sha256'] != digest(report['policy']):
        raise ValueError('Supplemental parent evidence identity drift')
    for row in report['observations']:
        path = folder/row['path']
        camera=row['camera']
        fields=('geometry_id','source_sha256','scope','view','basis','center','half_height','width','height')
        basis=np.asarray(camera['basis'],float);center=np.asarray(camera['center'],float)
        if path.parent != folder or Path(row['path']).is_absolute() or sha(path) != row['sha256'] or \
                camera_family(camera) != row['family'] or camera['geometry_id']!=expected['geometry_id'] or \
                camera['source_sha256']!=expected['source_sha256'] or digest({k:camera[k] for k in fields})!=camera['render_id'] or \
                basis.shape!=(3,3) or center.shape!=(3,) or not np.isfinite(basis).all() or not np.isfinite(center).all() or \
                not np.allclose(basis@basis.T,np.eye(3),atol=1e-6) or not np.isfinite(camera['half_height']) or \
                camera['half_height']<=0 or any(type(camera[k]) is not int or not 1<=camera[k]<=4096 for k in ('width','height')):
            raise ValueError('Parent view cache drift or correlated camera identity')
        with np.load(path, allow_pickle=False) as npz:
            observation={k:npz[k].copy() for k in npz.files}
        shape=(camera['height'],camera['width'])
        rgb,ids,bary,labels,quality=(observation[k] for k in ('rgb','ids','bary','labels','confidence'))
        if rgb.shape!=(*shape,3) or rgb.dtype!=np.uint8 or ids.shape!=shape or not np.issubdtype(ids.dtype,np.signedinteger) or \
                bary.shape!=(*shape,3) or labels.shape!=shape or quality.shape!=shape or not np.isfinite(bary).all() or \
                not np.isfinite(quality).all() or np.any(ids<-1) or np.any(ids>=expected['face_count']) or \
                np.any(labels<0) or np.any(labels>5) or np.any(quality<0) or np.any(quality>1):
            raise ValueError('Parent observation face mapping or array shape drift')
        points=bary[ids>=0]
        if np.any(points<-2e-8) or np.any(points>1+2e-8) or np.any(np.abs(points.sum(1)-1)>1e-5):
            raise ValueError('Parent observation barycentric mapping drift')
        for prefix in ('model_',):
            if prefix+'labels' in observation:
                extra,extra_quality=observation[prefix+'labels'],observation[prefix+'confidence']
                if extra.shape!=shape or extra_quality.shape!=shape or not np.isfinite(extra_quality).all() or \
                        np.any(extra<0) or np.any(extra>5) or np.any(extra_quality<0) or np.any(extra_quality>1):
                    raise ValueError('Parent model audit mapping drift')
        for name in ('seed_labels','proposal_labels'):
            if name in observation and (observation[name].shape!=shape or not np.isin(observation[name],(0,1,3,4)).all()):
                raise ValueError('Parent source seed or proposal mapping drift')
        yield row,observation


def native_children(native):
    children = defaultdict(list)
    for face, depth, path, label, confidence_hex, samples in native['subfaces']:
        confidence = float(np.frombuffer(bytes.fromhex(confidence_hex), dtype='<f4')[0])
        if confidence>=.7 and samples:
            children[face].append((LeafKey(face, depth, path), label))
    return children


def split(run, evidence_folder, output, tool):
    manifest, old, locks, _, freeze = baseline(run)
    vertices, faces, _ = read(run/'baseline/native.bin', manifest['source_sha256'])
    native = json.loads((run/'baseline/native-analysis.json').read_text())
    children = native_children(native)
    existing = {r['source_face_id']: r for r in old['faces']}
    neighbors = surface_neighbors(vertices, faces)
    protected = {face for key, (face, _) in catalog(old).items() if key in frozen_ids(locks)}
    protected |= {int(n) for face in list(protected) for n in neighbors[face] if int(n)>=0}
    visible = set()
    for _, obs in observations(evidence_folder, manifest):
        visible.update(map(int, np.unique(obs['ids'][obs['ids']>=0])))
    remaining = old['triangle_budget']-old['added_triangles']
    selected, retained = {}, []
    subject = locks['locks'][0]['subject_id']
    for face, rows in sorted(children.items()):
        parents = {PARENTS.get(label) for _, label in rows}
        if face in existing or face in protected or face not in visible or len(parents-{None})<2:
            continue
        keys = leaf_partition(face, {key for key, _ in rows})
        # Reserve conformity space; native validation applies the exact final budget.
        if (len(keys)-1)*2 > remaining:
            retained.append(dict(source_face_id=face, reason='MIXED_PARENT_BUDGET_RETAIN_R8'))
            continue
        remaining -= (len(keys)-1)*2
        selected[face] = keys
    requested = set(existing) | set(selected)
    requested |= {int(n) for face in selected for n in neighbors[face] if int(n)>=0}
    _, welded = np.unique(vertices, axis=0, return_inverse=True)
    records = []
    for face in sorted(requested):
        old_face = existing.get(face)
        keys = selected.get(face, [LeafKey(face)])
        cells = old_face['cells'] if old_face else [dict(polygon=k.corners().tolist(), holes=[], label='R6',
            parent_label='R6', subject_id=subject, kind='R9_NATIVE_MIXED_PARENT', source_leaf=k.encode()) for k in keys]
        records.append(dict(source_face_id=face, base=cells, layers=[],
            baseline_triangle_count=old_face['triangle_count'] if old_face else len(cells),
            source_vertices=welded[faces[face]].tolist(), preserve_color_provenance=True))
    request = dict(schema='orca.surface-partition-request/v1', identity={k: old[k] for k in
        ('geometry_id','source_sha256','face_count','evidence_sha256','runtime_sha256','policy_sha256',
         'baseline_sha256','boundary_policy_sha256')}, incremental_baseline=old,
        baseline_partition_sha256=old['partition_sha256'], existing_added_triangles=old['added_triangles'],
        triangle_budget=old['triangle_budget'], frozen_cell_ids=sorted(frozen_ids(locks)), faces=records)
    output.mkdir(parents=True, exist_ok=False)
    publish(output/'request.json', request)
    path = output/'partition.json'
    subprocess.run([str(tool), str(output/'request.json'), str(path)], check=True)
    document = json.loads(path.read_text())
    content_hash = sha(path)
    destination = output/'surface-partitions'/f'{content_hash}.json'
    destination.parent.mkdir()
    path.rename(destination)
    sidecar = deepcopy(locks)
    sidecar['partition_ref'] = dict(schema='orca.surface-partition-reference/v1',
                                  path=f'surface-partitions/{content_hash}.json', sha256=content_hash)
    publish(output/'shape-locks.json', sidecar)
    subprocess.run([str(tool), '--validate-v3', str(output/'request.json'), str(output/'shape-locks.json'), str(output)], check=True)
    publish(output/'stage-report.json', dict(mixed_roots=len(selected), retained=retained,
        added_triangles=document['added_triangles'], triangle_budget=document['triangle_budget'],
        original_geometry_unchanged=True, frozen_cell_geometry_unchanged=True))
    print(json.dumps(dict(mixed_roots=len(selected), added_triangles=document['added_triangles'])), flush=True)


def load_partition(folder):
    from surface_partition import load
    locks = json.loads((folder/'shape-locks.json').read_text())
    expected = {k: locks[k] for k in ('geometry_id','source_sha256','face_count','evidence_sha256',
        'runtime_sha256','policy_sha256','baseline_sha256','boundary_policy_sha256')}
    return load(folder, expected)


def bound_partition(run,folder,state=None):
    manifest,old,_,plans,freeze=state or baseline(run)
    document,locks=load_partition(folder)
    for field in ('geometry_id','source_sha256','face_count','evidence_sha256','runtime_sha256',
                  'policy_sha256','baseline_sha256'):
        if document[field]!=old[field] or locks[field]!=old[field]:
            raise ValueError('Parent partition left the verified R8 source identity')
    validate_freeze(document,locks,plans,freeze)
    tool=Path(__file__).resolve().parents[2]/'build-shape-integration/tests/slic3rutils/Release/surface_partition_tool.exe'
    subprocess.run([str(tool),'--validate-v3',str(folder/'request.json'),str(folder/'shape-locks.json'),str(folder)],
                   check=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
    return document,locks


def analytic_unit_vote(cell, face, obs, camera, vertices, faces, parent):
    basis, center = np.asarray(camera['basis']), np.asarray(camera['center'])
    triangle = vertices[faces[face]]
    covered, total = 0., 0.
    for local in cell['triangles']:
        world = np.asarray(local) @ triangle
        projected = (world-center) @ basis[:2].T
        xy = np.column_stack(((projected[:,0]/camera['half_height']+1)*camera['width']/2,
                              (1-projected[:,1]/camera['half_height'])*camera['height']/2))
        from local_leaf_boundaries import area
        amount = area(xy)
        covered += amount*analytic_coverage(xy, obs['labels'], obs['confidence'], parent)
        total += amount
    return covered/max(total, 1e-12)


def build_ownership(run, folder, evidence_folder, output, visibility_folder=None,cell_visibility_folder=None,
                    local_recognition_performed=True):
    manifest, old, old_locks, old_plans, freeze = baseline(run)
    document, locks = bound_partition(run,folder,(manifest,old,old_locks,old_plans,freeze))
    vertices, faces, _ = read(run/'baseline/native.bin', manifest['source_sha256'])
    centers = vertices[faces].mean(1)
    count = len(faces)
    native = json.loads((run/'baseline/native-analysis.json').read_text())
    children = native_children(native)
    evidence = json.loads((run/'baseline/evidence.json').read_text())
    subject = bound_subject(evidence,locks)
    trusted, root_conflicts = trusted_regions(evidence)
    root_conflicts |= shape_conflicts(evidence)
    old_ownership = json.loads((run/'baseline/surface-ownership.json').read_text())
    known = defaultdict(list)
    for region in old_ownership['regions']:
        for row in region['leaves']:
            known[row[0]].append((LeafKey(*row), region))
    explicit = {r['source_face_id']: r for r in document['faces']}
    retained_mixed={face for face,entry in explicit.items() if
                    'R9_UNRESOLVED_MIXED_PARENT_RETAIN_R8' in entry.get('reasons',[]) or
                    entry['status']=='R9_LOCAL_BUDGET_FALLBACK'}
    frozen = frozen_ids(locks)
    floor = float(vertices[:,2].min()+np.ptp(vertices[:,2])*.1)
    head_ids = [face for face, labels in trusted.items() if set(labels)&{'face','nose'}]
    low, high = np.quantile(centers[head_ids], [.02,.98], axis=0)
    units, votes, rejected = {}, [], []
    color_total = np.zeros((count,3), dtype=float)
    color_pixels = np.zeros(count, dtype=np.int64)
    cell_color_total, cell_color_pixels = {}, defaultdict(int)
    scope_roots = set()
    exact_rows = []
    exact_cell_rows = []
    source_seeds={}
    visibility = None
    cell_visibility = None
    if visibility_folder is not None:
        from portrait_r9_parent_evidence import cell_samples,load_visibility
        cameras = [row['camera'] for row,_ in observations(evidence_folder,manifest)]
        visibility = load_visibility(visibility_folder,manifest,cameras)
        if cell_visibility_folder is None:
            raise ValueError('Exact parent repair requires cell-local visibility')
        cell_records=cell_samples(document)
        cell_visibility=load_visibility(cell_visibility_folder,manifest,cameras,cell_records,document['partition_sha256'])
        cell_indices_exact={row['id']:index for index,row in enumerate(cell_records)}
    observed_index = 0
    for row, obs in observations(evidence_folder, manifest):
        ids, labels, quality = obs['ids'], obs['labels'], obs['confidence']
        valid = ids>=0
        visible = np.bincount(ids[valid], minlength=count)
        scope_roots.update(map(int, np.flatnonzero(visible)))
        if visibility is not None:
            from portrait_parent_projection import root_votes
            exact,coverage = root_votes(vertices,faces,row['camera'],labels,quality,
                                        visibility[observed_index],centers[:,2]>floor,ids)
            exact_rows.append((row['family'],row['camera']['width']/row['camera']['half_height'],exact))
            scope_roots.update(map(int,np.flatnonzero(visibility[observed_index])))
        for channel in range(3):
            color_total[:,channel] += np.bincount(ids[valid], weights=obs['rgb'][valid,channel]/255., minlength=count)
        color_pixels += visible
        root_votes = {}
        for parent, classes in (('skin',(2,3)),('hair',(1,)),('cloth',(4,))):
            mask = valid & np.isin(labels, classes) & (quality>=.9)
            amount = np.bincount(ids[mask], minlength=count)
            fraction = amount/np.maximum(visible, 1)
            for face in map(int, np.flatnonzero((fraction>=.9)&(visible>0)&(centers[:,2]>floor))):
                root_votes[face] = parent
                if face not in explicit and face not in root_conflicts:
                    key = root_id(document, face)
                    units[key] = dict(id=key, source_face_id=face, implicit_root=True)
                    votes.append((row['family'], key, parent, 1.))
        from portrait_r5_visual_review import pixel_groups
        analytic_roots = set(old_plans[3]['analytic_source_faces']) | (set(explicit)-{r['source_face_id'] for r in old['faces']})
        mapping, lookup = pixel_cells(document, ids, obs['bary'], pixel_groups(ids), analytic_roots)
        present = mapping>=0
        sample_count = np.bincount(mapping[present], minlength=len(lookup))
        local_rgb = np.column_stack([np.bincount(mapping[present],weights=obs['rgb'][present,channel]/255.,
                                                minlength=len(lookup)) for channel in range(3)])
        for index in np.flatnonzero(sample_count):
            key=lookup[index]['id']
            cell_color_total[key]=cell_color_total.get(key,np.zeros(3))+local_rgb[index]
            cell_color_pixels[key]+=int(sample_count[index])
        fractions = {}
        for parent, classes in (('skin',(2,3)),('hair',(1,)),('cloth',(4,))):
            mask = present & np.isin(labels, classes) & (quality>=.9)
            fractions[parent] = np.bincount(mapping[mask], minlength=len(lookup))/np.maximum(sample_count,1)
        cell_indices = {c['id']: index for index,c in enumerate(lookup)}
        cell_row=np.full(len(cell_records),255,np.uint8) if visibility is not None else None
        # A subpixel cell uses continuous coverage inside a visible source root.
        for face, entry in explicit.items():
            cell_visible=visibility is not None and any(cell_visibility[observed_index,cell_indices_exact[c['id']]]
                                                       for c in entry['cells'])
            if not (visible[face] or cell_visible) or centers[face,2]<=floor:
                continue
            for cell in entry['cells']:
                if cell['id'] in frozen or cell['label'] not in ('face','R6','skin','hair','cloth'):
                    continue
                key = cell['id']
                units[key] = dict(id=key, source_face_id=face, implicit_root=False)
                if visibility is not None and not cell_visibility[observed_index,cell_indices_exact[key]]:
                    continue
                if visibility is not None:
                    from portrait_parent_projection import project
                    xy=project(np.asarray(cell['polygon'])@vertices[faces[face]],row['camera'])
                    if np.any(xy<0) or np.any(xy>np.array([row['camera']['width'],row['camera']['height']])):
                        continue
                    cell_row[cell_indices_exact[key]]=0
                for parent in ('skin','hair','cloth'):
                    index = cell_indices[key]
                    partial=visibility is not None and cell_visibility[observed_index,cell_indices_exact[key]]!=127
                    if (visibility is None or partial) and sample_count[index]:
                        coverage = fractions[parent][index]
                    elif partial:
                        coverage = 0.
                    else:
                        coverage = analytic_unit_vote(cell, face, obs, row['camera'], vertices, faces, parent)
                    if coverage>=.9:
                        if visibility is None:
                            votes.append((row['family'], key, parent, coverage))
                        else:
                            from portrait_parent_projection import cell_sample_support
                            record_index=cell_indices_exact[key]
                            if partial or cell_sample_support(cell_records[record_index]['samples'],vertices[faces[face]],row['camera'],
                                    labels,quality,int(cell_visibility[observed_index,record_index]),parent,
                                    ids,face,obs['bary'],cell):
                                cell_row[record_index]={'hair':1,'skin':3,'cloth':4}[parent]
        observed_index += 1
        if visibility is not None:
            exact_cell_rows.append((row['family'],row['camera']['width']/row['camera']['half_height'],cell_row))
            print(json.dumps(dict(stage='exact_parent_projection',scope=row['camera']['scope'],
                view=row['camera']['view'],supported_roots=int(np.isin(exact,(1,3,4)).sum()))),flush=True)
    if visibility is not None:
        from portrait_parent_projection import full_sample_support,independent_votes
        exact_accepted,exact_conflicts,columns = independent_votes(exact_rows)
        cell_accepted,cell_conflicts,cell_columns=independent_votes(exact_cell_rows)
        exact_accepted[~full_sample_support(exact_rows,visibility,exact_accepted)]=0
        cell_accepted[~full_sample_support(exact_cell_rows,cell_visibility,cell_accepted)]=0
        for face,(parent,proof) in source_parent_seeds(evidence).items():
            if face in explicit or face in root_conflicts or centers[face,2]<=floor or any(
                    PARENTS.get(label) not in (None,parent) for _,label in children[face]):
                continue
            supports=sorted({family for index,(family,_,values) in enumerate(exact_rows)
                             if values[face]!=255 and visibility[index,face]==127})
            if len(supports)<2:
                continue
            exact_accepted[face]={'hair':1,'skin':3,'cloth':4}[parent]
            exact_conflicts[face]=False
            source_seeds[root_id(document,face)]=(parent,supports,proof)
        votes=[]
        for record_index in np.flatnonzero(cell_accepted):
            key=cell_records[record_index]['id']
            if key not in units:
                continue
            parent={1:'hair',3:'skin',4:'cloth'}[int(cell_accepted[record_index])]
            for family,column in cell_columns:
                if column[record_index]==cell_accepted[record_index]:votes.append((family,key,parent,1.))
        for face in map(int,np.flatnonzero(exact_accepted)):
            if face in explicit or face in root_conflicts:
                continue
            key=root_id(document,face)
            parent={1:'hair',3:'skin',4:'cloth'}[int(exact_accepted[face])]
            units[key]=dict(id=key,source_face_id=face,implicit_root=True)
            for family,column in columns:
                if column[face]==exact_accepted[face]: votes.append((family,key,parent,1.))
            if key in source_seeds:
                for family in source_seeds[key][1]:votes.append((family,key,parent,1.))
        # Do not preserve contradictory coarse raster votes after exact integration.
        votes=[v for v in votes if v[1] in units and (not units[v[1]]['implicit_root'] or
               exact_accepted[units[v[1]]['source_face_id']]>0 and
               v[2]=={1:'hair',3:'skin',4:'cloth'}[int(exact_accepted[units[v[1]]['source_face_id']])])]
    accepted, conflicts = combine_votes(votes)
    source_rgb = color_total/np.maximum(color_pixels,1)[:,None]
    if visibility is not None:
        from portrait_r6_repair import load_render
        renderer,_,_,uv,vertex_colors,materials,material_ids=load_render(run)
        missing=np.flatnonzero((color_pixels==0)&np.isin(np.arange(count),list(scope_roots)))
        for start in range(0,len(missing),32768):
            local=missing[start:start+32768]
            ids=local[None,:]
            bary=np.full((1,len(local),3),1/3)
            source_rgb[local]=renderer.shade(faces,uv,vertex_colors,materials,material_ids,ids,bary)[0]/255.
    final = {}
    for key, unit in units.items():
        face = unit['source_face_id']
        cell = next((c for c in explicit.get(face,{}).get('cells',[]) if c['id']==key), None)
        verified_old = []
        if cell is not None:
            for leaf, region in known[face]:
                if all(inside_analytic(leaf.corners(), np.asarray(t)).all() for t in cell['triangles']):
                    verified_old.append(region)
        elif known[face]:
            verified_old = [r for k,r in known[face] if k.depth==0]
        if face in root_conflicts:
            rejected.append(dict(**unit, reason='CROSS_SUBJECT_OR_EXPLICIT_SEMANTIC_CONFLICT'))
            continue
        if face in retained_mixed:
            rejected.append(dict(**unit,reason='MIXED_PARENT_BOUNDARY_RETAIN_R8'))
            continue
        confirmed_old=[r for r in verified_old if r['status']=='CONFIRMED_PARENT']
        if confirmed_old and len({r['parent_label'] for r in confirmed_old})==1:
            region = confirmed_old[0]
            parent = 'skin' if region['parent_label']=='face' else 'cloth'
            support, origin = sorted(set(region['view_ids'])), 'FROZEN_R6_CONFIRMED_PARENT'
        elif key in source_seeds:
            parent,support,_=source_seeds[key]
            origin='R9_VERIFIED_SAME_SOURCE_PARENT_SEED'
        elif key in accepted:
            parent, support = accepted[key]
            origin = 'R9_LOCAL_WHOLE_BODY_MULTIVIEW'
        else:
            local_conflict=visibility is not None and (exact_conflicts[face] if unit['implicit_root'] else
                                                       cell_conflicts[cell_indices_exact[key]])
            rejected.append(dict(**unit, reason='PARENT_VIEW_CONFLICT' if key in conflicts or local_conflict else
                'PARENT_SUPPORT_INSUFFICIENT'))
            continue
        semantic = set(trusted.get(face, {}))
        blockers=parent_semantic_blockers(semantic)
        if blockers:
            rejected.append(dict(**unit, reason='EXPLICIT_PARENT_CONFLICT',conflicting_labels=sorted(blockers)))
            continue
        scope = 'hair' if parent=='hair' else 'clothing' if parent=='cloth' else \
            'ear-side' if semantic & {'lr','rr'} else 'face' if centers[face,2]>=low[2] else \
            'neck' if centers[face,2]>=low[2]-(high[2]-low[2])*.6 and \
                np.all(centers[face,:2]>=low[:2]-.15*(high[2]-low[2])) and \
                np.all(centers[face,:2]<=high[:2]+.15*(high[2]-low[2])) else 'body-skin'
        scoped_rgb=cell_color_total[key]/cell_color_pixels[key] if cell_color_pixels[key] else source_rgb[face]
        source_scope='CELL_SOURCE_PIXELS' if cell_color_pixels[key] else \
            'SOURCE_ROOT_PIXELS' if color_pixels[face] else 'VERIFIED_SOURCE_UV_CENTROID'
        if visibility is not None and cell is not None and not cell_color_pixels[key]:
            record=cell_records[cell_indices_exact[key]]
            ids=np.full((1,7),face,np.int64)
            bary=np.asarray(record['samples'])[None,:,:]
            scoped_rgb=renderer.shade(faces,uv,vertex_colors,materials,material_ids,ids,bary)[0].mean(0)/255.
            source_scope='VERIFIED_CELL_LOCAL_SOURCE_UV'
        final[key] = dict(unit, parent_label=parent, view_ids=support, evidence_source=origin,
            anatomical_scope=scope,source_rgb=scoped_rgb.tolist(),
            source_color_scope=source_scope,prior_semantic_labels=sorted(semantic),
            prior_parent_states=sorted({r['status'] for r in verified_old}))
        if key in source_seeds:final[key]['source_semantic_proof']=source_seeds[key][2]
    _,welded=np.unique(vertices,axis=0,return_inverse=True)
    cell_catalog={c['id']:c for f in document['faces'] for c in f['cells']}
    groups=connected_units(final,cell_catalog,welded[faces])
    regions = []
    for parent, keys in groups:
        supports = sorted({view for key in keys for view in final[key]['view_ids']})
        regions.append(dict(id=digest(dict(parent=parent, units=keys)), subject_id=subject, parent_label=parent,
            status='CONFIRMED_PARENT', view_ids=supports, evidence_source='CELL_SPECIFIC_VERIFIED_PARENT', risks=[],
            units=[final[key] for key in keys]))
    doc = dict(schema=SCHEMA, **{k: document[k] for k in ('source_sha256','geometry_id','face_count','evidence_sha256')},
        partition_sha256=document['partition_sha256'], partition_ref=locks['partition_ref'],
        detail_freeze_sha256=freeze['fingerprint'], evidence_ref=dict(sha256=sha(evidence_folder/'evidence.json')),
        policy=POLICY, policy_sha256=digest(POLICY), regions=regions, preserved=rejected,
        scope_source_faces=sorted(scope_roots),
        unclaimed_visible_source_faces=sorted(scope_roots-{u['source_face_id'] for u in final.values()}),
        body_skin_is_production_semantic_id=False)
    validate(doc, document, locks, freeze['fingerprint'])
    if visibility is not None:
        doc['policy']={**POLICY,'algorithm':'r9-exact-visible-parent-ownership/v6',
            'old_coarse_parent_predictions_are_hard_conflicts':False,
            'original_r6_proposals_require_current_multiview_confirmation':True,
            'ownership_module_sha256':sha(Path(__file__)),
            'visibility_sha256':sha(visibility_folder/'visibility.bin'),
            'cell_visibility_sha256':sha(cell_visibility_folder/'visibility.bin'),
            'projection_module_sha256':sha(Path(__file__).with_name('portrait_parent_projection.py')),
            'source_subpixel_sampling':'verified-source-UV-centroid',
            'partial_visibility_class_witness':'target-raster-face-and-cell-only',
            'partial_witness_coverage':.9,
            'partial_witness_denominator':'all-target-raster-pixels-including-unknown-and-low-quality',
            'minimum_full_sample_views':1,
            'full_sample_proof':'final-parent-class-after-same-family-crop-resolution',
            'cell_visibility_limit':'seven-samples-on-largest-cell-triangle-not-all-cell-triangles',
            'full_visibility_subpixels':'continuous-parent-mask-coverage',
            'same_direction_crop':'higher-resolution-refines-single-vote'}
        doc['policy']['source_parent_seed']=dict(confidence=.95,dominance=.95,original_view_support=2,
            current_full_visible_independent_views=2,mixed_or_detail_roots_excluded=True,
            evidence_sha256=manifest['evidence_sha256'])
        doc['policy_sha256']=digest(doc['policy'])
        validate(doc,document,locks,freeze['fingerprint'])
    output.mkdir(parents=True, exist_ok=False)
    ownership_path = output/'surface-ownership.json'
    publish(ownership_path, doc)
    if visibility is not None:
        path=output/'projection-votes.npz'
        with path.open('xb') as stream:
            np.savez_compressed(stream,roots=np.stack([r[2] for r in exact_rows]),
                cells=np.stack([r[2] for r in exact_cell_rows]),
                families=np.array([r[0] for r in exact_rows]),
                resolutions=np.array([r[1] for r in exact_rows]),
                cell_ids=np.array([r['id'] for r in cell_records]))
        publish(output/'projection-audit.json',dict(identity={k:manifest[k] for k in
            ('source_sha256','geometry_id','face_count','evidence_sha256')},
            partition_sha256=document['partition_sha256'],evidence_sha256=sha(evidence_folder/'evidence.json'),
            projection_sha256=sha(path),ownership_sha256=sha(ownership_path),
            original_source_seed_units=len(source_seeds),
            source_seed_support='original-high-confidence-semantic-proof-plus-current-geometric-visibility',
            correlated_crops_are_one_vote=True,unobserved=255,observed_uncertain=0))
    report = dict(accepted_units=len(final), parent_counts=dict(Counter(r['parent_label'] for r in final.values())),
        anatomical_scope_counts=dict(Counter(r['anatomical_scope'] for r in final.values())), components=len(regions),
        preserved_counts=dict(Counter(r['reason'] for r in rejected)), source_scope_roots=len(scope_roots),
        unclaimed_visible_source_roots=len(doc['unclaimed_visible_source_faces']),
        source_color_scopes=dict(Counter(r['source_color_scope'] for r in final.values())),
        original_face_count=count, unseen_roots=count-len(scope_roots), frozen_feature_count=len(freeze['details']),
        policy_sha256=doc['policy_sha256'], ownership_sha256=sha(ownership_path),
        local_recognition_performed=local_recognition_performed,
        reused_source_observations=not local_recognition_performed,
        production_enabled=False, material_tree_changed=False)
    publish(output/'stage-report.json', report)
    print(json.dumps(report), flush=True)


def repaired_slot(parent, palette, rgb, support_pixels, original_rgb):
    from portrait_r5_review_audit import oklab
    slots = {s['uid']: np.asarray(s['rgb']) for s in palette}
    if parent=='skin': return 'portrait-skin', 'UNIFIED_CONFIRMED_SKIN'
    if parent=='hair': return 'portrait-dark', 'CONTINUOUS_SOURCE_CONFIRMED_HAIR'
    compatible = {k:v for k,v in slots.items() if k not in ('portrait-skin','portrait-lips')}
    lab = oklab(np.asarray(rgb).reshape(1,1,3)*255)[0,0]
    distances = {k: float(np.linalg.norm(oklab((v*255).reshape(1,1,3))[0,0]-lab)) for k,v in compatible.items()}
    best = min(distances, key=distances.get)
    if best in ('portrait-cool','portrait-mid'):
        visible = sum(n>=32 for n in support_pixels.values())>=2
        difference = float(np.linalg.norm(oklab((compatible[best]*255).reshape(1,1,3))[0,0]-
                                         oklab((np.asarray(original_rgb)*255).reshape(1,1,3))[0,0]))
        if not visible or difference<.03:
            best = min((k for k in distances if k not in ('portrait-cool','portrait-mid')), key=distances.get)
    return best, 'CONTINUOUS_CONFIRMED_CLOTH_COMPATIBLE_SLOT'


def manual_first(row, manual):
    if row['id'] in manual:
        row = dict(row, slot_uid=manual[row['id']], color_source='MANUAL', repair_reason='MANUAL_PRIORITY')
    return row


if __name__=='__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('stage', choices=('split','ownership'))
    for name in ('run','evidence','output'):
        parser.add_argument('--'+name, type=Path, required=True)
    parser.add_argument('--partition', type=Path)
    parser.add_argument('--tool', type=Path)
    parser.add_argument('--visibility', type=Path)
    parser.add_argument('--cell-visibility',type=Path)
    args = parser.parse_args()
    if args.stage=='split': split(args.run.resolve(), args.evidence.resolve(), args.output.resolve(), args.tool.resolve())
    else: build_ownership(args.run.resolve(), args.partition.resolve(), args.evidence.resolve(), args.output.resolve(),
                         args.visibility.resolve() if args.visibility else None,
                         args.cell_visibility.resolve() if args.cell_visibility else None)
