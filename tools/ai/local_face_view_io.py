"""Read stored face-view observations without importing an experiment runner."""
import json
import numpy as np


def load_views(root, *, view_factory=None, boundary_factory=None):
    if view_factory is None:
        from local_face_landmarks import FaceView
        view_factory = FaceView
    if boundary_factory is None:
        from local_brow_boundary import Projection
        boundary_factory = Projection
    result = []
    for path in sorted(root.glob('*.npz')):
        with np.load(path, allow_pickle=False) as data:
            meta = json.loads(str(data['metadata']))
            parts = {name: (data['part_'+name], data['fraction_'+name]) for name in meta['parts']}
            irises = {name: (data['iris_'+name], data['iris_fraction_'+name]) for name in meta['irises']}
            boundary = boundary_factory(*(data['boundary_'+name] for name in
                ('rgb', 'ids', 'barycentric', 'uv', 'valid', 'origin', 'points'))) if meta['boundary'] else None
            result.append(view_factory(meta['family'], data['points'], data['world'], data['valid'],
                meta['scale'], meta['pixel_size'], data['visible'], data['counts'], data['head'],
                parts, irises, meta['quality'], boundary))
    # A zoomed crop does not add a new independent view.
    chosen = {}
    for view in result:
        if view.family not in chosen or view.quality > chosen[view.family].quality:
            chosen[view.family] = view
    return list(chosen.values())
