"""Source-texture segmentation inside a verified, side-specific brow proposal."""
from dataclasses import dataclass, replace
import importlib.metadata

import numpy as np

ITERATIONS = 5
MAX_HOPS = 2
MIN_SEED_PIXELS = 5
MIN_DELTA_E = 5.0


def runtime_identity():
    versions = {}
    for name in ('opencv-python', 'opencv-contrib-python', 'opencv-python-headless',
                 'opencv-contrib-python-headless'):
        try:
            versions[name] = importlib.metadata.version(name)
        except importlib.metadata.PackageNotFoundError:
            pass
    return {'packages': versions, 'iterations': ITERATIONS, 'max_hops': MAX_HOPS,
            'min_seed_pixels': MIN_SEED_PIXELS, 'min_delta_e': MIN_DELTA_E}


@dataclass
class Projection:
    rgb: np.ndarray
    ids: np.ndarray
    barycentric: np.ndarray
    uv: np.ndarray
    valid: np.ndarray
    origin: np.ndarray
    points: np.ndarray


def _reachable(seeds, allowed, neighbors, hops=None):
    reached = set(seeds) & set(allowed)
    frontier = set(reached)
    if neighbors is None:
        return reached
    remaining = len(allowed) if hops is None else hops
    for _ in range(remaining):
        following = {int(n) for face in frontier for n in neighbors[face] if int(n) >= 0}
        frontier = (following & allowed) - reached
        if not frontier:
            break
        reached.update(frontier)
    return reached


def refine(label, views, faces, semantic_seeds, legal, neighbors, contour, polygon_mask, fuse_masks):
    original = np.unique(np.asarray(faces, dtype=np.int64))
    seed = set(int(face) for face in original) & legal
    foreground = seed & semantic_seeds
    allowed = _reachable(seed, legal, neighbors, MAX_HOPS)
    audit = {'method': 'SOURCE_TEXTURE_GRABCUT', 'parameters': runtime_identity(),
             'search_face_count': len(allowed), 'foreground_seed_count': len(foreground), 'views': []}

    def fallback(reason):
        audit['status'] = reason
        return {'faces': original, 'views': views, 'refined': False,
                'reason': reason, 'audit': audit}

    if not foreground:
        return fallback('BROW_SOURCE_SEEDS_UNAVAILABLE')
    try:
        import cv2
    except ImportError:
        return fallback('BROW_SOURCE_REFINER_UNAVAILABLE')
    refined = []
    for view in views:
        projection = view.boundary
        row = {'family': view.family, 'status': 'BROW_SOURCE_UNAVAILABLE'}
        audit['views'].append(row)
        if projection is None:
            continue
        ids = projection.ids
        permitted = np.isin(ids, list(allowed)) & projection.valid
        band = polygon_mask(projection.points[contour], ids.shape)
        fg = permitted & band & np.isin(ids, list(foreground))
        # Only nearby, same-parent skin supplies the source background model.
        skin = projection.valid & np.isin(ids, list(legal - semantic_seeds)) & ~band
        location = np.argwhere(permitted | fg)
        if len(location) == 0:
            continue
        lo = np.maximum(location.min(0) - 8, 0)
        hi = np.minimum(location.max(0) + 9, ids.shape)
        crop = np.s_[lo[0]:hi[0], lo[1]:hi[1]]
        row.update(foreground_pixels=int(fg[crop].sum()), skin_pixels=int(skin[crop].sum()))
        if fg[crop].sum() < MIN_SEED_PIXELS or skin[crop].sum() < MIN_SEED_PIXELS:
            row['status'] = 'BROW_SOURCE_SEEDS_INSUFFICIENT'
            continue
        rgb = np.ascontiguousarray(projection.rgb[crop])
        lab = cv2.cvtColor(rgb.astype(np.float32) / 255.0, cv2.COLOR_RGB2Lab)
        contrast = float(np.linalg.norm(np.median(lab[fg[crop]], axis=0) - np.median(lab[skin[crop]], axis=0)))
        row['source_delta_e'] = contrast
        if not np.isfinite(contrast) or contrast < MIN_DELTA_E:
            row['status'] = 'BROW_SOURCE_COLOR_UNSEPARABLE'
            continue
        mask = np.full(rgb.shape[:2], cv2.GC_BGD, dtype=np.uint8)
        mask[permitted[crop]] = cv2.GC_PR_BGD
        mask[permitted[crop] & band[crop]] = cv2.GC_PR_FGD
        mask[skin[crop]] = cv2.GC_BGD
        mask[fg[crop]] = cv2.GC_FGD
        try:
            cv2.setRNGSeed(0)
            cv2.grabCut(rgb, mask, None, np.zeros((1, 65)), np.zeros((1, 65)),
                        ITERATIONS, cv2.GC_INIT_WITH_MASK)
        except cv2.error:
            row['status'] = 'BROW_SOURCE_SEGMENTATION_UNCERTAIN'
            continue
        result = np.zeros(ids.shape, dtype=bool)
        result[crop] = np.isin(mask, [cv2.GC_FGD, cv2.GC_PR_FGD]) & permitted[crop]
        selected, counts = np.unique(ids[result], return_counts=True)
        fraction = counts / view.counts[np.searchsorted(view.visible, selected)]
        parts = dict(view.parts)
        parts[label] = (selected, fraction)
        refined.append(replace(view, parts=parts))
        row.update(status='SOURCE_BOUNDARY_REFINED', selected_pixels=int(result.sum()),
                   selected_face_count=len(selected))
        # Each retained face has a deterministic, source-bound pixel witness.
        witnesses = []
        for face in selected:
            y, x = np.argwhere(result & (ids == face))[0]
            witnesses.append({'face_id': int(face), 'pixel': (projection.origin + [x, y]).tolist(),
                              'barycentric': projection.barycentric[y, x].tolist(),
                              'source_uv': projection.uv[y, x].tolist(),
                              'source_rgb': projection.rgb[y, x].tolist()})
        row['face_witnesses'] = witnesses
    if len({view.family for view in refined}) < 2:
        return fallback('BROW_SOURCE_BOUNDARY_UNCERTAIN')
    fused = set(int(face) for face in fuse_masks(refined, label))
    connected = _reachable(foreground & fused, fused, neighbors)
    if not connected:
        return fallback('BROW_SOURCE_BOUNDARY_UNCERTAIN')
    audit.update(status='SOURCE_BOUNDARY_REFINED', refined_face_count=len(connected),
                 removed_seed_faces=sorted(seed - connected), added_faces=sorted(connected - seed))
    return {'faces': np.asarray(sorted(connected), dtype=np.int64), 'views': refined,
            'refined': True, 'reason': '', 'audit': audit}
