"""Palette-independent parent attribution around immutable reviewed feature locks."""
from collections import Counter, defaultdict
import json
import re

import numpy as np

from beauty_leaf_domain import LeafKey, digest, domain, validate_keys
from local_face_landmarks import OVAL, surface_neighbors
from local_leaf_boundaries import BoundaryView, reconstruct_projection
from local_face_view_io import load_views

POLICY = {'algorithm': 'r6-parent-ownership/v1', 'minimum_confidence': .9,
          'minimum_dominance': .85, 'minimum_view_families': 2,
          'face_envelope_fraction': .9, 'cloth_seam_rings': 1,
          'maximum_depth': 4, 'source_color_is_ownership': False}


def partition(face, cuts, locked=()):
    """Complete adaptive partition; a frozen lock leaf is never subdivided."""
    cuts = set(cuts)
    protected = set(locked)
    result, pending = [], [LeafKey(face)]
    while pending:
        key = pending.pop()
        deeper = any(key != cut and key.contains(cut) for cut in cuts)
        if deeper and key in protected:
            raise ValueError('Parent partition would subdivide a reviewed feature')
        if deeper:
            pending.extend(key.children())
        else:
            result.append(key)
    result.sort()
    validate_keys(result, face + 1)
    if sum(4 ** -key.depth for key in result) != 1:
        raise ValueError('Incomplete parent partition')
    return result


def validate(document, locks):
    if 'leaf_domain' not in locks:
        raise ValueError('Parent repairs require a reviewed leaf boundary')
    count = locks['face_count']
    for field in ('geometry_id', 'source_sha256', 'evidence_sha256'):
        if document[field] != locks[field] or not re.fullmatch('[0-9a-f]{64}', document[field]):
            raise ValueError('Parent ownership identity drift')
    if document['schema'] != 'orca.portrait-surface-ownership/v1' or document['face_count'] != count or \
            document['boundary_sha256'] != digest(locks) or document['policy_sha256'] != digest(document['policy']):
        raise ValueError('Parent ownership boundary or policy drift')
    split = [LeafKey(*row) for row in document['editing_domain']['split_leaves']]
    if domain(locks['geometry_id'], count, split) != document['editing_domain'] or \
            document['editing_mapping_sha256'] != digest(document['editing_domain']):
        raise ValueError('Parent editing mapping drift')
    by_root = defaultdict(set)
    for key in split:
        by_root[key.source_face_id].add(key)
    frozen = {LeafKey(*row) for lock in locks['locks'] for row in lock['locked_leaves']}
    for key in frozen:
        if by_root.get(key.source_face_id, {LeafKey(key.source_face_id)}) and \
                key not in by_root.get(key.source_face_id, {LeafKey(key.source_face_id)}):
            raise ValueError('Reviewed feature leaf mapping changed')
    claimed, seen = [], set()
    subjects = {row['subject_id'] for row in locks['locks']}
    for region in document['regions']:
        if region['id'] in seen or region['subject_id'] not in subjects or region['parent_label'] not in ('face', 'cloth') or \
                region['status'] not in ('CONFIRMED_PARENT', 'SUPPORTED_PARENT_PROPOSAL') or len(set(region['view_ids'])) < 2:
            raise ValueError('Invalid parent region or cross-person ownership')
        seen.add(region['id'])
        keys = [LeafKey(*row) for row in region['leaves']]
        if not keys:
            raise ValueError('Empty parent region')
        validate_keys(keys, count)
        for key in keys:
            if key in frozen or key not in by_root.get(key.source_face_id, {LeafKey(key.source_face_id)}):
                raise ValueError('Parent ownership leaves its partition or crosses a feature')
        claimed.extend(keys)
    validate_keys(sorted(claimed), count)
    return document


def trusted_regions(evidence):
    records, conflicts = defaultdict(dict), set()
    for region in evidence['regions']:
        for face, confidence, dominance, samples, support in region['samples']:
            if confidence < POLICY['minimum_confidence'] or dominance < POLICY['minimum_dominance'] or support < 2:
                continue
            previous = records[face].get(region['label'])
            if previous and previous != region['subject_id']:
                conflicts.add(face)
            records[face][region['label']] = region['subject_id']
    for face, labels in records.items():
        if len(set(labels.values())) > 1:
            conflicts.add(face)
        if ('face' in labels or 'nose' in labels) and set(labels).intersection(('hair', 'cloth', 'imouth', 'teeth')):
            conflicts.add(face)
    return records, conflicts


def build(vertices, faces, evidence, locks, views_path, native, color_children):
    if len(faces) != locks['face_count'] or evidence['source_sha256'] != locks['source_sha256'] or \
            evidence['geometry_id'] != locks['geometry_id']:
        raise ValueError('Parent source mapping changed')
    neighbors = surface_neighbors(vertices, faces)
    views = load_views(views_path)
    families = sorted({view.family for view in views})
    subjects = set(evidence['subjects'])
    if len(subjects) != 1:
        raise ValueError('Parent completion requires unambiguous single-person evidence')
    subject = next(iter(subjects))
    records, conflicts = trusted_regions(evidence)
    locked = defaultdict(set)
    existing = defaultdict(set)
    for key in map(lambda row: LeafKey(*row), locks['leaf_domain']['split_leaves']):
        existing[key.source_face_id].add(key)
    for lock in locks['locks']:
        for key in map(lambda row: LeafKey(*row), lock['locked_leaves']):
            locked[key.source_face_id].add(key)
    for shape in evidence.get('shape_details', []):
        if shape['status'] == 'INVALID_SHAPE_CONFLICT':
            conflicts.update(shape['rejected_faces'])
    excluded = {face for face, labels in records.items() if set(labels).intersection(('hair', 'imouth', 'teeth'))}
    seeds = {face for face, labels in records.items() if labels.get('face', labels.get('nose')) == subject}
    clothes = {face for face, labels in records.items() if labels.get('cloth') == subject}
    head_votes = Counter(int(face) for view in views for face in view.head)
    eligible = {face for face, votes in head_votes.items() if votes >= 2} | seeds
    eligible -= excluded | conflicts | clothes
    skin_roots = set(seeds & eligible)
    pending = sorted(skin_roots)
    for face in pending:
        for neighbor in neighbors[face]:
            neighbor = int(neighbor)
            if neighbor in eligible and neighbor not in skin_roots:
                skin_roots.add(neighbor)
                pending.append(neighbor)
    labels = np.array([int(v, 16) for v in native['labels']], dtype=np.uint8)
    confidence = np.frombuffer(bytes.fromhex(native['confidence_f32']), dtype='<f4')
    if len(labels) != len(faces) or len(confidence) != len(faces):
        raise ValueError('Native analysis face identity changed')
    native_children = defaultdict(list)
    for face, depth, path, label, confidence_hex, samples in native['subfaces']:
        conf = float(np.frombuffer(bytes.fromhex(confidence_hex), dtype='<f4')[0])
        if conf >= .7 and samples:
            native_children[face].append((LeafKey(face, depth, path), label))
    seam = {face for face in clothes if any(labels[n] in (3, 4) and confidence[n] >= .7 for n in neighbors[face])}
    seam |= {face for face in clothes if any(set(records.get(int(n), {})).intersection(('face', 'nose', 'neck', 'lr', 'rr'))
                                            for n in neighbors[face])}
    seam |= {int(n) for face in sorted(seam) for n in neighbors[face]}
    seam -= excluded | conflicts | skin_roots
    # Direct child evidence can expose a cloth fragment in a skin-dominated root.
    seam |= {face for face, children in native_children.items() if any(label == 5 for _, label in children)
             and any(n in clothes for n in neighbors[face]) and face not in excluded | conflicts | skin_roots}
    cuts = defaultdict(set)
    for children in color_children:
        for child in children:
            key = LeafKey(child['face_id'], child['path']['depth'], child['path']['value'])
            if key.source_face_id in skin_roots | seam and not any(lock.contains(key) for lock in locked[key.source_face_id]):
                cuts[key.source_face_id].add(key)
    for face in skin_roots | seam:
        for key, _ in native_children[face]:
            if not any(lock.contains(key) for lock in locked[face]):
                cuts[face].add(key)
    projections, view_errors = [], []
    for view in views:
        if view.boundary is None:
            continue
        try:
            transform, residual = reconstruct_projection(view.boundary, vertices, faces)
            projections.append(BoundaryView(view.family, transform, view.boundary.points[OVAL],
                                            set(map(int, view.visible)), 1., residual))
        except ValueError as error:
            view_errors.append({'family': view.family, 'reason': str(error)})
    root_views = defaultdict(set)
    for view in views:
        for face in view.head:
            root_views[int(face)].add(view.family)
    remaining = min(20000, len(faces) * 2 // 100) - (sum(len(v) for v in existing.values()) - len(existing))
    selected, preserved, added = {}, [], 0
    for face in sorted(map(int, skin_roots | seam)):
        before = existing.get(face, {LeafKey(face)})
        try:
            leaves = partition(face, before | cuts[face], locked[face])
        except ValueError:
            preserved.append({'face': face, 'reason': 'FROZEN_FEATURE_PARTITION_CONFLICT'})
            continue
        increase = len(leaves) - len(before)
        if increase > remaining:
            preserved.append({'face': face, 'reason': 'PARENT_PARTITION_BUDGET_EXHAUSTED'})
            continue
        if leaves != [LeafKey(face)]:
            existing[face] = set(leaves)
        remaining -= increase
        added += increase
        for key in leaves:
            if key in locked[face]:
                continue
            specific = [(child.depth, label) for child, label in native_children[face] if child.contains(key)]
            child_label = max(specific)[1] if specific else None
            if child_label not in (None, 3, 4, 5):
                preserved.append({'leaf': key.encode(), 'reason': 'EXPLICIT_NON_PARENT_CHILD'})
                continue
            if face in seam:
                if child_label == 5 or (child_label is None and face in clothes and labels[face] == 5):
                    selected[key] = ('cloth', 'CONFIRMED_PARENT', 'VERIFIED_CLOTH_SEAM', families)
                else:
                    preserved.append({'leaf': key.encode(), 'reason': 'UNCONFIRMED_CLOTHING_SIBLING'})
                continue
            if child_label == 5:
                preserved.append({'leaf': key.encode(), 'reason': 'CLOTH_CHILD_WITHIN_FACE_ROOT'})
                continue
            support = sorted(root_views[face])
            direct = face in seeds
            if not direct:
                world = key.corners() @ vertices[faces[face]]
                support = [view.family for view in projections if face in view.visible and
                           view.coverage(world, face)[0] >= POLICY['face_envelope_fraction']]
            if len(set(support)) < 2:
                # Parsed seed support is already validated across independent cameras.
                if direct:
                    support = families
                else:
                    preserved.append({'leaf': key.encode(), 'reason': 'PARENT_ENVELOPE_SUPPORT_INSUFFICIENT'})
                    continue
            selected[key] = ('face', 'CONFIRMED_PARENT' if direct else 'SUPPORTED_PARENT_PROPOSAL',
                             'VERIFIED_FACE_OR_NOSE' if direct else 'CONNECTED_MULTIVIEW_FACE_ENVELOPE', sorted(set(support)))
    groups = defaultdict(list)
    for key, (label, status, origin, support) in selected.items():
        groups[label, status, origin, tuple(support)].append(key)
    regions = [{'id': 'parent-' + str(index), 'subject_id': subject, 'parent_label': label,
                'status': status, 'evidence_source': origin, 'view_ids': list(support),
                'leaves': [key.encode() for key in sorted(keys)],
                'risks': ['TOPOLOGY_PARENT_COMPLETION'] if status == 'SUPPORTED_PARENT_PROPOSAL' else []}
               for index, ((label, status, origin, support), keys) in enumerate(sorted(groups.items()))]
    mapping = domain(locks['geometry_id'], len(faces), sorted(key for values in existing.values() for key in values if key.depth))
    document = {field: locks[field] for field in ('source_sha256', 'geometry_id', 'evidence_sha256', 'face_count')}
    document.update(schema='orca.portrait-surface-ownership/v1', boundary_sha256=digest(locks),
                    policy=POLICY, policy_sha256=digest(POLICY), regions=regions,
                    editing_domain=mapping, editing_mapping_sha256=digest(mapping), preserved=preserved)
    validate(document, locks)
    audit = {'face_scope_roots': len(eligible), 'face_connected_roots': len(skin_roots), 'cloth_seam_roots': len(seam),
             'scope_face_ids': sorted(map(int, eligible)),
             'clothing_scope_status': 'SUPPORTED_SEAM' if seam else 'NO_VERIFIED_ADJACENT_SKIN_SEAM',
             'accepted_leaves': len(selected), 'accepted_parent_counts': dict(Counter(value[0] for value in selected.values())),
             'preserved_counts': dict(Counter(row['reason'] for row in preserved)), 'explicit_conflict_roots': sorted(conflicts),
             'added_parent_triangles': added, 'total_added_triangles': len(mapping['split_leaves']) - len(existing),
             'triangle_budget': min(20000, len(faces) * 2 // 100), 'view_errors': view_errors,
             'shape_lock_unchanged': True, 'palette_independent': True}
    return document, audit
