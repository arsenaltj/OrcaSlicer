"""Generic source-bound contour requests; no fixture paths or color decisions."""
from collections import defaultdict

import cv2
import numpy as np

from local_face_landmarks import HEAD_LABELS, surface_neighbors
from local_leaf_boundaries import boundary_band
from local_surface_contours import POLICY, independent_views, proposals

POLICY_VERSION = 'source-contour-workbench-v2-proved-fallback'
ROOT = [[1., 0., 0.], [0., 1., 0.], [0., 0., 1.]]
DETAILS = ('le', 're', 'lb', 'rb', 'ulip', 'llip')


def associate_views(views, regions):
    heads = defaultdict(set)
    for region in regions:
        if region['label'] in HEAD_LABELS:
            heads[region['subject_id']].update(row[0] for row in region['samples'])
    groups = defaultdict(list)
    for view in views:
        scores = sorted(((len(set(view.head) & head), subject)
                         for subject, head in heads.items()), reverse=True)
        if not scores or scores[0][0] < max(16, .35 * len(view.head)):
            continue
        if len(scores) > 1 and scores[1][0] > .1 * scores[0][0]:
            continue
        groups[scores[0][1]].append(view)
    return groups


def topology_scope(seeds, neighbors, legal, blocked, rings):
    allowed = set(legal) - set(blocked)
    result = set(seeds) & allowed
    for _ in range(rings):
        result |= {int(n) for face in result for n in neighbors[face] if n >= 0} & allowed
    return result


def build(views, regions, shapes, vertices, faces, cancelled=None, progress=None):
    """Identity is added only after evidence bytes have been validated and hashed."""
    from local_shape_constraints import _hard_conflict
    checkpoint = cancelled or (lambda: False)
    eligible = {(s['subject_id'], s['label']): s for s in shapes
                if s['status'] in ('VALID_SHAPE', 'PROTECTED_SHAPE_UNCERTAIN')
                and s['accepted_faces'] and not _hard_conflict(s['reasons'], s.get('metrics', {}), s['label'])}
    if not eligible:
        return None
    neighbors = surface_neighbors(vertices, faces)
    groups = associate_views(views, regions)
    owners = {}
    conflicts = set()
    for region in regions:
        for row in region['samples']:
            face = row[0]
            owner = (region['subject_id'], region['label'])
            if face in owners and owners[face] != owner:
                conflicts.add(face)
            owners[face] = owner
    seed_owner, nested = {}, set()
    for key, shape in eligible.items():
        for face in shape['accepted_faces']:
            if face in seed_owner and seed_owner[face] != key:
                conflicts.add(face)
            seed_owner[face] = key
        nested.update(shape.get('nested_faces', []))
    layers, library, diagnostics = defaultdict(list), {}, []
    for subject, observations in groups.items():
        independent = independent_views(observations, vertices, faces)
        visibility_cache = {}
        detail_labels = DETAILS + ('periocular-le', 'periocular-re')
        for label_index, label in enumerate(detail_labels):
            if checkpoint():
                raise RuntimeError('contour_request_cancelled')
            primary = label.removeprefix('periocular-')
            shape = eligible.get((subject, primary))
            if shape is None:
                continue
            if progress:
                progress('ownership','校验连续五官边界 '+label,label_index,len(detail_labels))
            accepted = set(shape['accepted_faces']) - conflicts
            legal = {f for f, owner in owners.items() if owner == (subject, 'face')
                     or owner == (subject, 'nose') or owner == (subject, primary)} | accepted
            blocked = conflicts | set(shape['rejected_faces'])
            blocked |= {f for f, owner in seed_owner.items() if owner != (subject, primary)}
            rings = 2 if primary in ('lb', 'rb') else 1
            scope = topology_scope(accepted, neighbors, legal, blocked, rings)
            boundary, _ = boundary_band(accepted, neighbors)
            fitted, audit = proposals(label, independent, vertices, faces,
                                      accepted, accepted - boundary, scope, legal - blocked, visibility_cache=visibility_cache)
            diagnostics.append(dict(subject_id=subject, label=label, scope_faces=len(scope),
                                    independent_views=len(fitted), views=audit))
            for face in sorted(scope):
                world = vertices[faces[face]]
                witnesses, iris = [], []
                for view in fitted:
                    if not view.supported(face, world):
                        continue
                    try:
                        witnesses.append(view.references(world, library, subject + '/' + label))
                        if label in ('le', 're'):
                            iris.append(view.references(world, library, subject + '/iris-' + label, view.iris))
                    except ValueError:
                        continue
                if len(witnesses) < 2:
                    continue
                if label in ('le', 're'):
                    layers[face].append(dict(label='iris-' + label, parent_label=primary,
                        subject_id=subject, views=iris, envelope_views=witnesses, kind='CLIPPED_IRIS'))
                layers[face].append(dict(label=label, parent_label=primary,
                    subject_id=subject, views=witnesses,
                    kind='SOURCE_RESTORED_EYE_LINE' if label.startswith('periocular-') else 'CLIPPED_FEATURE'))
    included = set(layers) | (set(seed_owner) - conflicts)
    # Terminal neighbors make the source-edge conformity explicit, without
    # granting those neighbors any additional semantic or color authority.
    included |= {int(n) for face in tuple(included) for n in neighbors[face] if n >= 0}
    _, welded = np.unique(vertices, axis=0, return_inverse=True)
    requests = []
    for face in sorted(included):
        owner = seed_owner.get(face, owners.get(face))
        if owner is None:
            owner = next((seed_owner.get(int(n), owners.get(int(n))) for n in neighbors[face]
                          if int(n) in seed_owner or int(n) in owners), None)
            if owner is not None:
                owner = (owner[0], 'R6')
        if owner is None:
            continue
        subject, original = owner
        unproved_detail = original in DETAILS + ('imouth', 'lip-line-corner') and face not in seed_owner
        # Old parser rows can include detail faces outside the accepted shape.
        # Terminal neighbors serve seam conformity only; they keep their prior
        # appearance and must not claim accepted detail/nested authority.
        if unproved_detail:
            original = 'R6'
        if face in conflicts:
            original = 'R6'
        if face in nested and original in ('le', 're'):
            original = 'iris-' + original
        current = sorted(layers.get(face, []), key=lambda r:
            (0 if r['label'].startswith('iris-') else 1 if r['label'].startswith('periocular-') else 2, r['label']))
        redefining = bool(current) and (original in DETAILS or original.startswith('iris-'))
        requests.append(dict(source_face_id=face, baseline_triangle_count=1,
            source_vertices=welded[faces[face]].tolist(),
            base=[dict(polygon=ROOT, holes=[], label='face' if redefining else original,
                       fallback_label=original, parent_label='face', subject_id=subject, kind='SOURCE_PARENT')],
            layers=current, reasons=(['UNPROVED_DETAIL_NEIGHBOR'] if unproved_detail else []) +
                ([] if current else ['LOCAL_CONTOUR_VIEW_FALLBACK'])))
    # Keep chord error below 0.14 px at 4096 while avoiding thousands of
    # redundant vertices in each shared fitted curve.
    for key, polygon in library.items():
        reduced = cv2.approxPolyDP(np.asarray(polygon, np.float32), .035, True).reshape(-1, 2)
        if len(reduced) >= 3:
            library[key] = reduced.astype(float).tolist()
    return dict(faces=requests, curve_library=library, audit=diagnostics,
                policy=dict(POLICY, version=POLICY_VERSION, curve_reduction_epsilon_1024=.035))
