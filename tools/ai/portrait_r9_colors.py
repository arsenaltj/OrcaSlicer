"""Draft parent color plans with R8 underlayers, fixed details and manual priority."""
import argparse
from collections import Counter, defaultdict
from copy import deepcopy
import json
from pathlib import Path
import shutil

import numpy as np

from beauty_leaf_domain import LeafKey
from portrait_r5_baseline import sha
from portrait_r5_review_audit import oklab
from portrait_r5_visual_review import assignments, pixel_groups, publish
from portrait_r6_repair import pixel_provenance
from portrait_r9_replay import baseline, load_partition, manual_first, observations, repaired_slot
from portrait_surface_ownership_v2 import safe_reference, validate
from surface_detail_freeze import validate as validate_freeze
from surface_partition import pixel_cells


def slot_for_rgb(rgb, palette):
    if rgb is None: return None
    return next((s['uid'] for s in palette if np.allclose(rgb,s['rgb'],atol=1e-6)), None)


def inheritance(unit, old_decisions, roots, children, palette, cells):
    cell = cells.get(unit['id'])
    origin = unit['id'] if unit['id'] in old_decisions else (cell or {}).get('origin_cell_id')
    if origin in old_decisions and old_decisions[origin]['slot_uid']:
        row = old_decisions[origin]
        uid = row['slot_uid']
        return dict(source='R8_CELL_COLOR', original_slot=uid, inherited_cell_id=row['id'],
                    original_rgb=next(s['rgb'] for s in palette if s['uid']==uid))
    face = unit['source_face_id']
    leaf = LeafKey(*cell['source_leaf']) if cell and cell.get('source_leaf') else LeafKey(face)
    rgb, origin = roots.get(face), 'OLD_ROOT_SLOT' if face in roots else 'SOURCE_TEXTURE'
    matches = [r for r in children.get(face,[]) if LeafKey(r['face_id'],r['path']['depth'],r['path']['value']).contains(leaf)]
    if matches:
        child = max(matches, key=lambda r:r['path']['depth'])
        rgb, origin = child['color'], 'OLD_SUBFACE_COLOR'
    return dict(source=origin, original_slot=slot_for_rgb(rgb,palette), original_rgb=rgb,
                original_leaf=leaf.encode(), original_root_slot=slot_for_rgb(roots.get(face),palette))


def unit_visibility(document, observations_iter, analytic_roots, face_count):
    root_counts, cell_counts = {}, {}
    for row, obs in observations_iter:
        visible = obs['ids']>=0
        amounts = np.bincount(obs['ids'][visible],minlength=face_count).astype(np.uint32)
        mapping, lookup = pixel_cells(document,obs['ids'],obs['bary'],pixel_groups(obs['ids']),analytic_roots)
        local = np.bincount(mapping[mapping>=0],minlength=len(lookup))
        family = row['family']
        root_counts[family] = np.maximum(root_counts.get(family,np.zeros(face_count,dtype=np.uint32)),amounts)
        previous = cell_counts.setdefault(family,{})
        for index,cell in enumerate(lookup):
            previous[cell['id']] = max(previous.get(cell['id'],0),int(local[index]))
    return root_counts, cell_counts


def component_visibility(units, root_counts, cell_counts):
    return {family:sum(int(amounts[u['source_face_id']]) if u['implicit_root'] else
                       cell_counts[family].get(u['id'],0) for u in units)
            for family,amounts in root_counts.items()}


def colors(run, folder, ownership_folder, evidence_folder, output, manual=None):
    manifest, old, old_locks, old_plans, freeze = baseline(run)
    document, locks = load_partition(folder)
    ownership_path = ownership_folder/'surface-ownership.json'
    ownership = validate(json.loads(ownership_path.read_text()), document, locks, freeze['fingerprint'])
    cells = {c['id']: c for f in document['faces'] for c in f['cells']}
    cell_faces = {c['id']:f['source_face_id'] for f in document['faces'] for c in f['cells']}
    analytic_roots = set(old_plans[3]['analytic_source_faces']) | \
        ({f['source_face_id'] for f in document['faces']}-{f['source_face_id'] for f in old['faces']})
    counts, cell_counts = unit_visibility(document,observations(evidence_folder,manifest),
                                         analytic_roots,manifest['face_count'])
    output.mkdir(parents=True, exist_ok=False)
    ownership_hash = sha(ownership_path)
    destination = output/'portrait-ownership'/f'{ownership_hash}.json'
    destination.parent.mkdir()
    shutil.copyfile(ownership_path, destination)
    ref = dict(schema='orca.portrait-surface-ownership-reference/v2',path=f'portrait-ownership/{ownership_hash}.json',sha256=ownership_hash)
    if not safe_reference(ref): raise ValueError('Unsafe parent ownership reference')
    all_plans, reports = {}, []
    for n in (5,3,4,6):
        old_plan = old_plans[n]
        palette = old_plan['palette']
        old_decisions = {r['id']:r for r in old_plan['cells']}
        roots = assignments(run/f'baseline/colors-{n}/face-colors.bin',manifest['face_count'])
        children = defaultdict(list)
        for child in json.loads((run/f'baseline/colors-{n}/subface-colors.json').read_text()):
            children[child['face_id']].append(child)
        decisions = {key:deepcopy(row) for key,row in old_decisions.items() if key in cells}
        components = []
        for region in ownership['regions']:
            parent = region['parent_label']
            source = np.array([u['source_rgb'] for u in region['units']])
            median = np.median(source,axis=0)
            labs = oklab((source*255).reshape(-1,1,3))[:,0]
            chromatic_spread = float(np.max(np.linalg.norm(labs[:,1:]-np.median(labs[:,1:],axis=0),axis=1)))
            support = component_visibility(region['units'],counts,cell_counts)
            old_rgb = []
            histories = {}
            for unit in region['units']:
                history = inheritance(unit,old_decisions,roots,children,palette,cells)
                histories[unit['id']] = history
                if history['original_rgb'] is not None: old_rgb.append(history['original_rgb'])
            original = np.median(old_rgb,axis=0) if old_rgb else median
            role, reason = repaired_slot(parent,palette,median,support,original)
            # A whole-body class does not prove a garment's pigment pattern.
            preserve_pattern = parent=='cloth' and chromatic_spread>.06
            for unit in region['units']:
                row = dict(id=unit['id'],source_face_id=unit['source_face_id'],
                    label=cells[unit['id']]['label'] if unit['id'] in cells else 'R6',
                    slot_uid=None if preserve_pattern else role,
                    slot=None if preserve_pattern else next(i for i,s in enumerate(palette) if s['uid']==role),
                    color_source='R8_PATTERN_RETAINED' if preserve_pattern else 'R9_CONFIRMED_PARENT_REPAIR',
                    original_slot=histories[unit['id']]['original_slot'], actual_inheritance=histories[unit['id']],
                    parent_label=parent, anatomical_scope=unit['anatomical_scope'], component_id=region['id'],
                    repair_reason=None if preserve_pattern else reason,
                    retain_reason='SOURCE_CLOTH_PATTERN_REQUIRES_LOCAL_PIGMENT_EVIDENCE' if preserve_pattern else None)
                row = manual_first(row,manual or {})
                if row['slot_uid']:
                    row['slot'] = next(i for i,s in enumerate(palette) if s['uid']==row['slot_uid'])
                decisions[row['id']] = row
            components.append(dict(id=region['id'],parent=parent,units=len(region['units']),source_rgb=median.tolist(),
                chromatic_spread=chromatic_spread,selected_slot=None if preserve_pattern else role,
                view_pixels=support,retained_pattern=preserve_pattern))
        for key, cell in cells.items():
            if key not in decisions:
                unit = dict(id=key,source_face_id=cell_faces[key])
                history = inheritance(unit,old_decisions,roots,children,palette,cells)
                decisions[key] = dict(id=key,source_face_id=cell_faces[key],
                    label=cell['label'],slot_uid=None,slot=None,color_source='R8_UNRESOLVED_INHERITED',
                    original_slot=history['original_slot'],actual_inheritance=history,
                    retain_reason='UNCONFIRMED_SIBLING_KEEP_R8')
        plan = deepcopy(old_plan)
        plan.update(iteration='R9',boundary_sha256=document['partition_sha256'],partition_ref=locks['partition_ref'],
            shape_lock_sha256=sha(folder/'shape-locks.json'),detail_freeze_sha256=freeze['fingerprint'],
            ownership_ref=ref,ownership_scope='confirmed whole-person parents; frozen details excluded',
            policy='r9-cell-parent-color/v1',cells=sorted(decisions.values(),key=lambda r:r['id']),components=components,
            analytic_source_faces=sorted(set(old_plan['analytic_source_faces']) |
                ({f['source_face_id'] for f in document['faces']}-{f['source_face_id'] for f in old['faces']})),
            r8_plan_sha256=sha(run/f'baseline/r8-colors-{n}/portrait-color-plan.json'),
            oral_auto_color=False,manual_priority=True,production_enabled=False)
        all_plans[n] = plan
        path = output/f'colors-{n}'; path.mkdir()
        publish(path/'portrait-color-plan.json',plan)
        publish(path/'material-cells.json',[{k:r[k] for k in ('id','source_face_id','slot_uid','color_source')} for r in plan['cells']])
        reports.append(dict(color_count=n,plan_sha256=sha(path/'portrait-color-plan.json'),
            repaired_units=sum(r['color_source']=='R9_CONFIRMED_PARENT_REPAIR' for r in decisions.values()),
            inherited_sources=dict(Counter(r.get('actual_inheritance',{}).get('source','R8_FIXED_DETAIL') for r in decisions.values())),
            retained_patterns=sum(r['color_source']=='R8_PATTERN_RETAINED' for r in decisions.values())))
    validate_freeze(document,locks,all_plans,freeze)
    publish(output/'stage-report.json',dict(schema='orca.r9-color-report/v1',variants=reports,
        ownership_sha256=ownership_hash,frozen_details_unchanged=True,production_enabled=False,material_tree_changed=False))
    print(json.dumps(dict(variants=reports,frozen_details_unchanged=True)),flush=True)


def verify_ownership_pair(plan,ownership_path,color_folder,partition,freeze_sha256):
    ref=plan['ownership_ref']
    if not safe_reference(ref) or sha(ownership_path)!=ref['sha256'] or \
            sha(color_folder/ref['path'])!=ref['sha256'] or \
            plan['boundary_sha256']!=partition['partition_sha256'] or \
            plan['detail_freeze_sha256']!=freeze_sha256:
        raise ValueError('Color plan belongs to a different ownership, partition or freeze')


def apply_parents(base, ids, mapping, lookup, plan):
    result = base.copy()
    palette = {s['uid']:np.rint(np.asarray(s['rgb'])*255).astype(np.uint8) for s in plan['palette']}
    decisions = {r['id']:r for r in plan['cells'] if r['color_source'] in ('R9_CONFIRMED_PARENT_REPAIR','MANUAL') and r['slot_uid']}
    parent_map = np.zeros(ids.shape,dtype=np.uint8)
    roots, explicit = {}, {}
    known = {r['id'] for r in lookup}
    for row in decisions.values():
        if row['id'] in known: explicit[row['id']]=row
        else: roots[row['source_face_id']]=row
    if roots:
        count=max(int(ids.max()),max(roots))+1
        root_rgb=np.zeros((count,3),dtype=np.uint8)
        root_parent=np.zeros(count,dtype=np.uint8)
        for face,row in roots.items():
            root_rgb[face]=palette[row['slot_uid']]
            root_parent[face]={'skin':1,'hair':2,'cloth':3}.get(row['parent_label'],0)
        mask=ids>=0
        mask[mask]=root_parent[ids[mask]]>0
        result[mask]=root_rgb[ids[mask]]
        parent_map[mask]=root_parent[ids[mask]]
    rgb = np.zeros((len(lookup),3),dtype=np.uint8)
    classes = np.zeros(len(lookup),dtype=np.uint8)
    for index,cell in enumerate(lookup):
        row=explicit.get(cell['id'])
        if row:
            rgb[index]=palette[row['slot_uid']]
            classes[index]={'skin':1,'hair':2,'cloth':3}.get(row['parent_label'],0)
    valid=mapping>=0
    target=valid.copy();target[valid]=classes[mapping[valid]]>0
    result[target]=rgb[mapping[target]]
    parent_map[target]=classes[mapping[target]]
    return result,parent_map


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('run','partition','ownership','evidence','output'):
        parser.add_argument('--'+name,type=Path,required=True)
    args=parser.parse_args()
    colors(args.run.resolve(),args.partition.resolve(),args.ownership.resolve(),args.evidence.resolve(),args.output.resolve())
