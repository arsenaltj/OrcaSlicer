"""Offline parent material repair; ownership and reviewed contours are immutable."""
from collections import Counter, defaultdict
from copy import deepcopy
from math import gcd

import numpy as np

from beauty_leaf_domain import digest
from beauty_leaf_domain import LeafKey
from local_leaf_boundaries import area, clip_triangle
from portrait_r5_review_audit import oklab
from portrait_r9_colors import inheritance, slot_for_rgb
from portrait_surface_ownership_v2 import frozen_ids, root_id, validate


POLICY = dict(algorithm='frozen-detail-parent-cleanup/v1', minimum_independent_views=2,
              source_is_material_evidence_not_identity=True, color_count_exceptions=False,
              clothing_neutral_chroma=.018, clothing_white_lightness=.65,
              clothing_shadow_lightness=.42, source_chromaticity_distance=.02,
              shadow_lightness_step=.12, clothing_support_hops=3,
              small_component_units=64, minimum_fixed_neighbors=2, support_majority=2/3,
              cleanup_passes=1, repaired_units_become_donors=False,
              extra_material_minimum_view_pixels=32, extra_material_minimum_delta_e=.03,
              preserve_local_patterns=True, hair_is_protected=True,
              manual_priority=True, oral_auto_color=False, production_enabled=False)
PARENT_COLORS = ('PARENT_UNIFORM', 'PARENT_SUPPORTED_CLEANUP', 'MANUAL')


def lab(rgb):
    return oklab((np.asarray(rgb, dtype=float)*255).reshape(-1, 1, 3))[:, 0]


def chromaticity(value):
    return value[1:]/max(float(value[0]), .1)


def source_continuous(a, b, parent):
    if parent == 'skin':
        # Once identity is proved, warm/dark skin remains skin. Color only guards
        # local cleanup donors; it never removes a confirmed ownership claim.
        return np.linalg.norm(chromaticity(a)-chromaticity(b)) <= .07
    return np.linalg.norm(chromaticity(a)-chromaticity(b)) <= POLICY['source_chromaticity_distance'] and \
        abs(float(a[0]-b[0])) <= POLICY['shadow_lightness_step']


def adjacent_units(units, cells, source_vertices):
    """Positive-length shared boundaries; neither a shared vertex nor root is enough."""
    edges = defaultdict(list)
    neighbors = {key: set() for key in units}
    scale = 1_000_000_000
    for key, unit in units.items():
        face = unit['source_face_id']
        cell = cells.get(key, dict(polygon=np.eye(3).tolist(), holes=[]))
        for contour in [cell['polygon']]+cell.get('holes', []):
            points = np.rint(np.asarray(contour)*scale).astype(np.int64).tolist()
            for a, b in zip(points, points[1:]+points[:1]):
                zero = next((i for i in range(3) if a[i] == 0 and b[i] == 0), None)
                if zero is not None:
                    j, k = (zero+1) % 3, (zero+2) % 3
                    vj, vk = map(int, (source_vertices[face, j], source_vertices[face, k]))
                    line = ('seam', min(vj, vk), max(vj, vk))
                    start, end = (a[k], b[k]) if vj < vk else (a[j], b[j])
                else:
                    dx, dy = b[1]-a[1], b[2]-a[2]
                    divisor = gcd(abs(dx), abs(dy))
                    if not divisor:
                        continue
                    dx, dy = dx//divisor, dy//divisor
                    if dx < 0 or (dx == 0 and dy < 0):
                        dx, dy = -dx, -dy
                    line = ('internal', face, dx, dy, dx*a[2]-dy*a[1])
                    start, end = dx*a[1]+dy*a[2], dx*b[1]+dy*b[2]
                if start != end:
                    edges[line].append((min(start, end), max(start, end), key))
    for segments in edges.values():
        active = []
        for lo, hi, key in sorted(segments):
            active = [(end, other) for end, other in active if end > lo]
            for _, other in active:
                if other != key and units[key]['subject_id'] == units[other]['subject_id'] and \
                        units[key]['parent_label'] == units[other]['parent_label']:
                    neighbors[key].add(other)
                    neighbors[other].add(key)
            active.append((hi, key))
    return neighbors


def components(keys, neighbors):
    remaining = set(keys)
    result = []
    while remaining:
        seed = min(remaining)
        remaining.remove(seed)
        found, stack = [seed], [seed]
        while stack:
            key = stack.pop()
            additions = neighbors.get(key, set()) & remaining
            remaining.difference_update(additions)
            stack.extend(sorted(additions))
            found.extend(additions)
        result.append(sorted(found))
    return result


def material_targets(units, neighbors, palette, visibility):
    """Normalize skin and neutral cloth; keep pigment patterns local and explicit."""
    roles = {s['uid']: s for s in palette}
    values = lab([u['source_rgb'] for u in units.values()]) if units else np.empty((0, 3))
    sources = dict(zip(units, values))
    targets, retained = {}, {}
    for key, unit in units.items():
        parent, color = unit['parent_label'], sources[key]
        if not unit.get('confirmed_parent', True):
            retained[key] = 'PARENT_OWNERSHIP_NOT_CONFIRMED'
        elif parent == 'skin':
            if 'portrait-skin' in roles:
                targets[key] = ('portrait-skin', 'UNIFIED_CONFIRMED_BODY_SKIN')
            else:
                retained[key] = 'MISSING_PORTRAIT_SKIN_ROLE'
        elif parent == 'hair':
            retained[key] = 'HAIR_BOUNDARY_PROTECTED'
        elif np.linalg.norm(color[1:]) > POLICY['clothing_neutral_chroma']:
            retained[key] = 'LOCAL_SOURCE_PIGMENT_PATTERN_PRESERVED'
        elif color[0] >= POLICY['clothing_white_lightness']:
            if 'portrait-light' in roles:
                targets[key] = ('portrait-light', 'COL009_NEUTRAL_CLOTH_CORE')
            else:
                retained[key] = 'MISSING_PORTRAIT_LIGHT_ROLE'
        else:
            retained[key] = 'NEUTRAL_SHADOW_REQUIRES_CONNECTED_CORE'

    # Bounded paths follow source-neutral cloth only. Pigment patterns are
    # barriers, rather than exempting every other unit in the same garment.
    frontier = {key for key, (uid, _) in targets.items()
                if uid == 'portrait-light' and units[key]['parent_label'] == 'cloth'}
    visited = set(frontier)
    for _ in range(POLICY['clothing_support_hops']):
        following = set()
        for key in sorted(frontier):
            for other in sorted(neighbors.get(key, ())):
                if other in visited or units[other]['parent_label'] != 'cloth' or \
                        units[other]['subject_id'] != units[key]['subject_id'] or \
                        not units[other].get('confirmed_parent', True):
                    continue
                value = sources[other]
                if value[0] < POLICY['clothing_shadow_lightness'] or \
                        np.linalg.norm(value[1:]) > POLICY['clothing_neutral_chroma'] or \
                        not source_continuous(sources[key], value, 'cloth'):
                    continue
                targets[other] = ('portrait-light', 'COL009_CONNECTED_NEUTRAL_CLOTH_SHADOW')
                retained.pop(other, None)
                following.add(other)
        visited.update(following)
        frontier = following
        if not frontier:
            break

    # A separate neutral gray material needs a visible, continuous source region.
    # It is optional; limited palettes need not use every available slot.
    unresolved = {key for key, unit in units.items() if unit['parent_label'] == 'cloth' and
                  key not in targets and retained.get(key) == 'NEUTRAL_SHADOW_REQUIRES_CONNECTED_CORE'}
    for group in components(unresolved, neighbors):
        visible = {family: sum(visibility.get(key, {}).get(family, 0) for key in group)
                   for family in {v for key in group for v in visibility.get(key, {})}}
        rgb = np.median([units[key]['source_rgb'] for key in group], axis=0)
        if 'portrait-mid' not in roles or 'portrait-light' not in roles or \
                sum(n >= POLICY['extra_material_minimum_view_pixels'] for n in visible.values()) < 2:
            continue
        mid, light = lab([roles['portrait-mid']['rgb'], roles['portrait-light']['rgb']])
        center = lab([rgb])[0]
        benefit = np.linalg.norm(center-light)-np.linalg.norm(center-mid)
        if benefit >= POLICY['extra_material_minimum_delta_e']:
            for key in group:
                targets[key] = ('portrait-mid', 'VISIBLE_SEPARATE_NEUTRAL_CLOTH_MATERIAL')
                retained.pop(key, None)
    return targets, retained, sources


def supported_cleanup(units, neighbors, targets, histories, sources):
    """One pass from an immutable set of already-correct reliable material donors."""
    donors = {key for key, (uid, _) in targets.items()
              if histories[key]['original_slot'] == uid and len(set(units[key]['view_ids'])) >= 2}
    candidates = {key for key in targets if key not in donors}
    accepted, retained = {}, {}
    # Group by old assignment as well as parent, so a single island is audited.
    bins = defaultdict(set)
    for key in candidates:
        bins[(units[key]['subject_id'], units[key]['parent_label'],
              histories[key]['original_slot'], targets[key][0])].add(key)
    for keys in bins.values():
        for group in components(keys, neighbors):
            if len(group) > POLICY['small_component_units']:
                for key in group:
                    retained[key] = 'NOT_A_SMALL_SUPPORTED_ISLAND'
                continue
            for key in group:
                supported = [other for other in neighbors.get(key, ()) if other in donors and
                             targets[other][0] == targets[key][0] and
                             units[other]['subject_id'] == units[key]['subject_id'] and
                             units[other]['parent_label'] == units[key]['parent_label'] and
                             source_continuous(sources[key], sources[other], units[key]['parent_label'])]
                total = len(neighbors.get(key, ()))
                if len(supported) >= POLICY['minimum_fixed_neighbors'] and \
                        len(supported)/max(total, 1) >= POLICY['support_majority']:
                    accepted[key] = (targets[key][0], 'FIXED_SUPPORT_SINGLE_PASS_ISLAND')
                else:
                    retained[key] = 'INSUFFICIENT_FIXED_SAME_PARENT_SOURCE_SUPPORT'
    return accepted, retained, donors


def build_plans(old_plan, partition, locks, ownership, freeze_sha, histories, neighbors,
                visibility, manual=None):
    validate(ownership, partition, locks, freeze_sha)
    frozen = frozen_ids(locks)
    units = {u['id']: dict(u, parent_label=r['parent_label'], subject_id=r['subject_id'],
                         component_id=r['id'], confirmed_parent=r['status']=='CONFIRMED_PARENT')
             for r in ownership['regions'] if r['parent_label'] in ('skin','cloth') for u in r['units']}
    if set(histories) != set(units) or any(key in frozen for key in units):
        raise ValueError('Parent repair history or frozen unit mapping drift')
    if any(np.asarray(u['source_rgb']).shape != (3,) or not np.isfinite(u['source_rgb']).all() or
           np.any(np.asarray(u['source_rgb'])<0) or np.any(np.asarray(u['source_rgb'])>1) for u in units.values()):
        raise ValueError('Parent material samples are not verified source RGB')
    targets, reasons, sources = material_targets(units, neighbors, old_plan['palette'], visibility)
    cleanup, cleanup_reasons, donors = supported_cleanup(units, neighbors, targets, histories, sources)
    palette = {s['uid']: i for i, s in enumerate(old_plan['palette'])}
    inherited_manual = {key:row['original_slot'] for key,row in histories.items()
                        if row['source']=='MANUAL_COLOR' and row['original_slot'] is not None}
    manual = {**inherited_manual, **(manual or {})}
    if any(key not in units and key not in {r['id'] for r in old_plan['cells']} for key in manual) or \
            any(uid not in palette for uid in manual.values()):
        raise ValueError('Manual edit leaves the verified cell or palette set')
    plans = {}
    for mode in ('uniform', 'cleanup', 'combined'):
        decisions = {row['id']: deepcopy(row) for row in old_plan['cells']}
        changes = targets if mode in ('uniform', 'combined') else cleanup
        for key, unit in units.items():
            inherited = decisions.get(key)
            uid, reason = changes.get(key, (None, None))
            row = dict(id=key, source_face_id=unit['source_face_id'], label=unit['parent_label'],
                       implicit_root=unit['implicit_root'], parent_label=unit['parent_label'],
                       subject_id=unit['subject_id'], anatomical_scope=unit['anatomical_scope'],
                       component_id=unit['component_id'], actual_inheritance=histories[key],
                       original_slot=histories[key]['original_slot'], slot_uid=uid,
                       slot=palette[uid] if uid else None,
                       repair_reason=reason, color_source='PARENT_SUPPORTED_CLEANUP' if key in cleanup and
                       mode in ('cleanup', 'combined') else 'PARENT_UNIFORM' if uid else 'BASELINE_PRESERVED',
                       retain_reason=None if uid else reasons.get(key, cleanup_reasons.get(key, 'BASELINE_PRESERVED')))
            if uid is None and inherited is not None:
                row.update(slot_uid=inherited['slot_uid'], slot=inherited['slot'],
                           color_source=inherited['color_source'])
            decisions[key] = row
        for key, uid in manual.items():
            decisions[key].update(slot_uid=uid, slot=palette[uid], color_source='MANUAL',
                                  repair_reason='MANUAL_PRIORITY', retain_reason=None)
        plan = deepcopy(old_plan)
        plan.update(iteration='FROZEN_PARENT_CLEANUP', mode=mode,
                    boundary_sha256=partition['partition_sha256'], partition_ref=locks['partition_ref'],
                    detail_freeze_sha256=freeze_sha, policy=deepcopy(POLICY), policy_sha256=digest(POLICY),
                    cells=sorted(decisions.values(), key=lambda row: row['id']),
                    manual_priority=True, oral_auto_color=False, production_enabled=False)
        plans[mode] = plan
    audit = dict(units=len(units), target_units=len(targets), fixed_donor_units=len(donors),
                 protected_hair_units=sum(len(r['units']) for r in ownership['regions'] if r['parent_label']=='hair'),
                 cleanup_units=len(cleanup), retained_reasons=dict(Counter(reasons.values())),
                 cleanup_retained_reasons=dict(Counter(cleanup_reasons.values())),
                 repaired_units_become_donors=False)
    return plans, audit


def histories_for(units, old_plan, roots, children, cells):
    old = {r['id']: r for r in old_plan['cells']}
    result = {}
    for key, unit in units.items():
        history = inheritance(unit, old, roots, children, old_plan['palette'], cells)
        if history['source'] == 'R8_CELL_COLOR':
            row = old[history['inherited_cell_id']]
            history['source'] = 'MANUAL_COLOR' if row['color_source']=='MANUAL' else 'BASELINE_CELL_COLOR'
        elif children.get(unit['source_face_id']) and not (cells.get(key) or {}).get('source_leaf'):
            # A clipped polygon can span several old color leaves. Its history
            # is mixed until proven otherwise; do not invent a root assignment.
            cell = cells.get(key, dict(triangles=[np.eye(3).tolist()]))
            triangles = [np.asarray(t)[:,1:] for t in cell['triangles']]
            total = sum(area(t) for t in triangles)
            parts = []
            for child in children[unit['source_face_id']]:
                leaf = LeafKey(child['face_id'],child['path']['depth'],child['path']['value'])
                amount = sum(area(clip_triangle(t,leaf.corners()[:,1:])) for t in triangles)
                if amount > max(1e-15,total*1e-8):
                    parts.append(dict(leaf=leaf.encode(),coverage=amount/max(total,1e-15),rgb=child['color'],
                                      slot_uid=slot_for_rgb(child['color'],old_plan['palette'])))
            full = [p for p in parts if p['coverage']>=1-1e-7]
            if full:
                selected = max(full,key=lambda p:p['leaf'][1])
                history.update(source='OLD_SUBFACE_COLOR',original_rgb=selected['rgb'],
                               original_slot=selected['slot_uid'],original_leaf=selected['leaf'])
            elif parts:
                history.update(source='MIXED_OLD_SUBFACE_COLOR',original_rgb=None,original_slot=None,
                               legacy_subface_candidates=parts)
        result[key] = history
    return result


def protect_ownership(ownership, partition, native):
    """Native oral/accessory witnesses protect their actual units, including mixed roots."""
    labels = np.asarray([int(value,16) for value in native['labels']],dtype=np.uint8)
    confidence = np.frombuffer(bytes.fromhex(native['confidence_f32']),dtype='<f4')
    if len(labels)!=partition['face_count'] or len(confidence)!=len(labels) or not np.isfinite(confidence).all():
        raise ValueError('Native protected source mapping drift')
    root_barriers = set(map(int,np.flatnonzero(np.isin(labels,(6,8))&(confidence>=.7))))
    children = defaultdict(list)
    for face,depth,path,label,confidence_hex,samples in native['subfaces']:
        score = float(np.frombuffer(bytes.fromhex(confidence_hex),dtype='<f4')[0])
        if label in (6,8) and score>=.7 and samples:
            children[face].append(LeafKey(face,depth,path))
    cells = {c['id']:c for f in partition['faces'] for c in f['cells']}
    result = deepcopy(ownership)
    regions, rejected = [], []
    for region in result['regions']:
        retained = []
        for unit in region['units']:
            key,face = unit['id'],unit['source_face_id']
            protected = face in root_barriers
            if not protected and children.get(face):
                triangles = (cells.get(key) or dict(triangles=[np.eye(3).tolist()]))['triangles']
                protected = any(area(clip_triangle(np.asarray(t)[:,1:],leaf.corners()[:,1:]))>1e-12
                                for leaf in children[face] for t in triangles)
            if protected:
                rejected.append(dict(id=key,source_face_id=face,implicit_root=unit['implicit_root'],
                                     reason='SOURCE_ORAL_OR_ACCESSORY_PROTECTION'))
            else:
                retained.append(unit)
        if retained:
            region['units'] = retained
            region['id'] = digest(dict(parent=region['parent_label'],units=sorted(u['id'] for u in retained)))
            regions.append(region)
    result['regions'] = regions
    result['preserved'] = result.get('preserved',[])+rejected
    claimed = {u['source_face_id'] for r in regions for u in r['units']}
    result['unclaimed_visible_source_faces'] = sorted(set(result.get('scope_source_faces',[]))-claimed)
    result['policy'].update(native_oral_accessory_protection=dict(labels=[6,8],confidence=.7,
                                  mixed_unconfirmed_units_retained=True),
                            protected_units=len(rejected))
    result['policy_sha256'] = digest(result['policy'])
    return result


def apply_plan(base, ids, mapping, lookup, plan, partition):
    """Implicit roots never paint an explicit mixed root; explicit units paint themselves."""
    result = base.copy()
    parents = np.zeros(ids.shape, np.uint8)
    palette = {s['uid']: np.rint(np.asarray(s['rgb'])*255).astype(np.uint8) for s in plan['palette']}
    decisions = {row['id']: row for row in plan['cells'] if row['color_source'] in PARENT_COLORS and row['slot_uid']}
    explicit_faces = {face['source_face_id'] for face in partition['faces']}
    explicit_keys = {cell['id'] for face in partition['faces'] for cell in face['cells']}
    roots = {}
    for row in decisions.values():
        face = row['source_face_id']
        if row['id'] in explicit_keys:
            continue
        if face in explicit_faces or row['id'] != root_id(partition, face) or not row.get('implicit_root'):
            raise ValueError('Parent color attempts to paint an unverified or mixed root')
        roots[face] = row
    rgb = np.zeros((partition['face_count'], 3), np.uint8)
    classes = np.zeros(partition['face_count'], np.uint8)
    for face, row in roots.items():
        rgb[face] = palette[row['slot_uid']]
        classes[face] = {'skin': 1, 'hair': 2, 'cloth': 3}.get(row.get('parent_label'), 4)
    mask = ids >= 0
    mask[mask] = classes[ids[mask]] > 0
    result[mask], parents[mask] = rgb[ids[mask]], classes[ids[mask]]
    local_rgb = np.zeros((len(lookup), 3), np.uint8)
    local_class = np.zeros(len(lookup), np.uint8)
    for index, cell in enumerate(lookup):
        row = decisions.get(cell['id'])
        if row:
            if row['source_face_id'] != cell['source_face_id']:
                raise ValueError('Explicit parent color source mapping drift')
            local_rgb[index] = palette[row['slot_uid']]
            local_class[index] = {'skin': 1, 'hair': 2, 'cloth': 3}.get(row.get('parent_label'), 4)
    valid = mapping >= 0
    selected = valid.copy()
    selected[valid] = local_class[mapping[valid]] > 0
    result[selected], parents[selected] = local_rgb[mapping[selected]], local_class[mapping[selected]]
    return result, parents
