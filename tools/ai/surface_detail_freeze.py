"""Per-detail geometry/color invariants independent of the complete partition hash."""
from copy import deepcopy
import re

from beauty_leaf_domain import digest

FIELDS = ('geometry_id', 'source_sha256', 'face_count')
LOCK_FIELDS = ('label', 'subject_id', 'parent_label', 'status', 'view_support',
               'reasons', 'locked_cells', 'nested_cells', 'periocular_cells')


def cell_geometry(cell, face):
    def fixed(points):
        return [[int(round(float(v) * 1_000_000_000)) for v in point] for point in points]
    return dict(id=cell['id'], source_face_id=face, label=cell['label'],
                parent_label=cell['parent_label'], subject_id=cell['subject_id'],
                polygon=fixed(cell['polygon']), holes=[fixed(h) for h in cell['holes']])


def catalog(document):
    result = {}
    for face in document['faces']:
        for cell in face['cells']:
            if cell['id'] in result:
                raise ValueError('Duplicate freeze cell')
            result[cell['id']] = (face['source_face_id'], cell)
    return result


def colors_for(ids, plan):
    decisions = {c['id']: c for c in plan['cells']}
    palette = {s['uid']: s['rgb'] for s in plan['palette']}
    result = []
    for key in ids:
        row = decisions[key]
        uid = row['slot_uid']
        rgb = [int(round(v * 1_000_000_000)) for v in palette[uid]] if uid else None
        result.append(dict(id=key, slot_uid=uid, rgb=rgb, color_source=row['color_source']))
    return result


def capture(document, locks, plans, labels=('le', 're', 'lb', 'ulip', 'llip')):
    cells = catalog(document)
    details = []
    for lock in locks['locks']:
        if lock['label'] not in labels:
            continue
        record = {field: deepcopy(lock.get(field, [])) for field in LOCK_FIELDS}
        ids = sorted(set(record['locked_cells'] + record['periocular_cells']))
        record['cells'] = [cell_geometry(cells[key][1], cells[key][0]) for key in ids]
        record['boundary_sha256'] = digest(record)
        record['palette_fingerprints'] = {
            str(count): digest(colors_for(ids, plan)) for count, plan in sorted(plans.items())}
        details.append(record)
    if {row['label'] for row in details} != set(labels):
        raise ValueError('Incomplete frozen feature set')
    details.sort(key=lambda row: (row['subject_id'], row['label']))
    result = dict(schema='orca.beauty-detail-freeze/v1',
                  identity={k: document[k] for k in FIELDS}, details=details)
    result['fingerprint'] = digest(result)
    return result


def validate(document, locks, plans, freeze):
    if freeze.get('schema') != 'orca.beauty-detail-freeze/v1' or \
            not re.fullmatch('[0-9a-f]{64}', freeze.get('fingerprint', '')) or \
            digest({k: v for k, v in freeze.items() if k != 'fingerprint'}) != freeze['fingerprint']:
        raise ValueError('Frozen feature fingerprint drift')
    if freeze['identity'] != {k: document[k] for k in FIELDS}:
        raise ValueError('Frozen feature source/geometry drift')
    actual = capture(document, locks, plans, tuple(r['label'] for r in freeze['details']))
    if actual != freeze:
        raise ValueError('Frozen feature boundary, nested, risk or color changed')
    return True
