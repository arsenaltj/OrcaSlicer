"""Deterministic orthographic views and observed-face crops, without face priors."""
from dataclasses import dataclass
import math

import numpy as np


@dataclass(frozen=True)
class Camera:
    name: str
    basis: np.ndarray
    center: np.ndarray
    half_height: float
    size: int

    def describe(self):
        return {'name': self.name, 'basis': self.basis.tolist(), 'center': self.center.tolist(),
                'half_height': self.half_height, 'size': self.size}


def coarse_cameras(vertices):
    """Eight azimuths fit the whole mesh, independent of a presumed face/front.

    Z-up is the native coordinate system, not a guarantee of an upright person.
    A face missed by these bounded views remains unknown; nothing is hallucinated
    on the back/top/bottom or outside detections.
    """
    v = np.asarray(vertices)
    if v.ndim != 2 or v.shape[1] != 3 or not 0 < len(v) <= 6_000_000 or not np.isfinite(v).all():
        raise ValueError('Invalid camera geometry')
    low, high = v.min(0).astype(np.float64), v.max(0).astype(np.float64)
    center = low + (high-low)/2
    radius = float(np.sqrt(np.max(np.sum((v.astype(np.float64)-center)**2, axis=1))))
    if not math.isfinite(radius) or radius <= 0:
        raise ValueError('Geometry has no finite camera extent')
    result = []
    for i in range(8):
        yaw, elevation = i*math.pi/4, math.radians(15 if i % 2 == 0 else -15)
        direction = np.array([math.cos(yaw)*math.cos(elevation), math.sin(yaw)*math.cos(elevation), math.sin(elevation)])
        right = np.cross([0., 0., 1.], direction); right /= np.linalg.norm(right)
        up = np.cross(direction, right)
        result.append(Camera(f'coarse-{i}', np.stack((right, up, direction)), center.copy(), radius*1.05, 512))
    return result


def focus_camera(parent, rectangle, ids, depths, selected, name):
    """Focus an actually detected/parsed visible surface using its measured depth.

    Crops retain the parent viewing direction. No fixed head height, front axis,
    mesh recentering, unseen surface inference or bounding-box-only depth guess.
    """
    rect = np.asarray(rectangle, dtype=np.float64)
    ids, depths, selected = np.asarray(ids), np.asarray(depths), np.asarray(selected)
    shape = (parent.size, parent.size)
    if (rect.shape != (4,) or not np.isfinite(rect).all() or rect[2] <= rect[0] or rect[3] <= rect[1] or
            ids.shape != shape or ids.dtype.kind not in 'iu' or depths.shape != shape or selected.shape != shape or
            selected.dtype != np.bool_ or not isinstance(name, str) or not name):
        raise ValueError('Invalid observed face crop')
    clipped = np.clip(rect, 0, parent.size)
    x0, y0 = np.floor(clipped[:2]).astype(int)
    x1, y1 = np.ceil(clipped[2:]).astype(int)
    if x1-x0 < 4 or y1-y0 < 4:
        return None
    active = selected[y0:y1, x0:x1] & (ids[y0:y1, x0:x1] >= 0) & np.isfinite(depths[y0:y1, x0:x1])
    yy, xx = np.nonzero(active)
    if len(xx) < 4:
        return None
    # The rectangle comes from detection, depth only from its observed parser
    # support. Background depths and geometry behind visible pixels cannot vote.
    local = np.array([((x0+x1)/parent.size-1)*parent.half_height,
                      (1-(y0+y1)/parent.size)*parent.half_height,
                      float(np.median(depths[y0+yy, x0+xx]))])
    center = parent.center + local @ parent.basis
    half_height = max(x1-x0, y1-y0)*parent.half_height/parent.size*1.6
    if not np.isfinite(center).all() or not 0 < half_height < parent.half_height:
        return None
    return Camera(name, parent.basis.copy(), center, half_height, 768)
