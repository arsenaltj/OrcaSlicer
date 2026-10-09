"""Local, source-bound contour refinement without changing canonical geometry."""
from dataclasses import dataclass

import cv2
import numpy as np
from scipy.interpolate import PchipInterpolator

from beauty_leaf_domain import LeafKey

ROI_SIZE = 1024
MAX_DEPTH = 4
MAX_EDGE_PIXELS = 1.0
ALGORITHM_VERSION = 'r5-boundary/v2-analytic-visibility'
BROW_ITERATIONS = 5
BROW_MIN_SEED = 5
BROW_SOURCE_DELTA_L = 5.
BROW_SMOOTH_SIGMA = .75


def smooth_lid(points):
    """Shape-preserving arcs with the original corner endpoints held fixed."""
    points = np.asarray(points, dtype=float)
    if points.shape != (16, 2) or not np.isfinite(points).all():
        raise ValueError('Invalid eyelid landmarks')
    start, end = points[0], points[8]
    axis = end-start
    width = np.linalg.norm(axis)
    if width <= 1e-6:
        raise ValueError('Collapsed eye opening')
    axis /= width
    up = np.array([-axis[1], axis[0]])
    result = []
    for arc in (points[:9], np.vstack((points[8:], points[:1]))[::-1]):
        x, y = (arc-start) @ axis, (arc-start) @ up
        # Non-monotone landmarks do not justify inventing a new contour.
        if np.any(np.diff(x) <= 1e-6):
            raise ValueError('Non-monotone eyelid landmarks')
        samples = np.linspace(0, width, 65)
        curve = start + samples[:,None]*axis + PchipInterpolator(x, y)(samples)[:,None]*up
        curve[0], curve[-1] = start, end
        result.append(curve)
    return np.vstack((result[0], result[1][-2:0:-1]))


def clip_triangle(polygon, triangle):
    """Clip a simple contour against a convex triangle in continuous pixels."""
    result = np.asarray(polygon, dtype=float)
    triangle = np.asarray(triangle, dtype=float)
    orientation = np.sign(np.linalg.det(np.column_stack((triangle[1]-triangle[0], triangle[2]-triangle[0]))))
    if orientation == 0:
        return np.empty((0,2))
    for a, b in zip(triangle, np.roll(triangle, -1, axis=0)):
        if len(result) == 0:
            break
        edge = b-a
        distance = orientation*(edge[0]*(result[:,1]-a[1])-edge[1]*(result[:,0]-a[0]))
        output = []
        for i in range(len(result)):
            j = (i+1) % len(result)
            inside, following = distance[i] >= -1e-9, distance[j] >= -1e-9
            if inside:
                output.append(result[i])
            if inside != following:
                t = distance[i]/(distance[i]-distance[j])
                output.append(result[i]+t*(result[j]-result[i]))
        result = np.asarray(output, dtype=float).reshape(-1,2)
    return result


def area(polygon):
    return abs(float(cv2.contourArea(np.asarray(polygon, dtype=np.float32)))) if len(polygon) >= 3 else 0.


def positive_depth(polygon, coefficients, epsilon=1e-5):
    if len(polygon) == 0:
        return polygon
    distances = np.column_stack((polygon,np.ones(len(polygon)))) @ coefficients-epsilon
    output = []
    for i in range(len(polygon)):
        j = (i+1) % len(polygon)
        inside, following = distances[i] >= 0, distances[j] >= 0
        if inside:
            output.append(polygon[i])
        if inside != following:
            t = distances[i]/(distances[i]-distances[j])
            output.append(polygon[i]+t*(polygon[j]-polygon[i]))
    return np.asarray(output,dtype=float).reshape(-1,2)


class AnalyticVisibility:
    """Conservative visible area against all source triangles, including ones
    smaller than a raster pixel. Occlusion overlaps can only lower support."""
    CELL = 8

    def __init__(self, vertices, faces, transform, shape):
        self.shape = shape
        self.direction = -np.cross(transform[:3,0],transform[:3,1])
        self.direction /= np.linalg.norm(self.direction)
        projected = np.column_stack((vertices,np.ones(len(vertices)))) @ transform
        triangles = projected[faces]
        lower,upper = triangles.min(1),triangles.max(1)
        included = np.flatnonzero((upper[:,0] >= 0) & (upper[:,1] >= 0) &
                                 (lower[:,0] <= shape[1]) & (lower[:,1] <= shape[0]))
        self.triangles = triangles[included]
        # clip_triangle accepts half-plane cross products down to -1e-9.
        # Bound that relaxed triangle too, especially for thin slivers: an
        # ordinary exact AABB would incorrectly skip its tolerated contacts.
        edges = self.triangles[:,1:] - self.triangles[:,:1]
        determinant = np.abs(edges[:,0,0]*edges[:,1,1]-edges[:,0,1]*edges[:,1,0])
        slack = np.full(len(included), np.inf)
        np.divide(1e-9, determinant, out=slack, where=determinant>0)
        span = upper[included]-lower[included]
        padding = 3*slack[:,None]*np.maximum(span,np.finfo(float).tiny)
        padding += 32*np.finfo(float).eps*np.maximum(1,np.abs(self.triangles).max((1,2)))[:,None]
        self.lower, self.upper = lower[included]-padding, upper[included]+padding
        self.face_ids = included
        self.depths = (vertices @ self.direction)[faces[included]]
        self.buckets = {}
        lower = np.floor(np.maximum(lower[included],0)/self.CELL).astype(int)
        upper = np.floor(np.minimum(upper[included],shape[::-1])/self.CELL).astype(int)
        for index,(lo,hi) in enumerate(zip(lower,upper)):
            for y in range(lo[1],hi[1]+1):
                for x in range(lo[0],hi[0]+1):
                    self.buckets.setdefault((x,y),[]).append(index)

    def fraction(self, face, triangle, world):
        total = area(triangle)
        if total < 1e-12:
            return 0.
        h,w = self.shape
        rectangle = np.array([[0,0],[w,0],[w,h],[0,h]],dtype=float)
        clipped = clip_triangle(triangle,rectangle)
        visible = area(clipped)
        if visible < total*.9:
            return visible/total
        source = np.column_stack((triangle,np.ones(3)))
        try:
            plane = np.linalg.solve(source,world @ self.direction)
        except np.linalg.LinAlgError:
            return 0.
        lo = np.floor(np.maximum(triangle.min(0),0)/self.CELL).astype(int)
        hi = np.floor(np.minimum(triangle.max(0),self.shape[::-1])/self.CELL).astype(int)
        candidates = {i for y in range(lo[1],hi[1]+1) for x in range(lo[0],hi[0]+1)
                      for i in self.buckets.get((x,y),[])}
        # A spatial bucket contains many tiny triangles that do not intersect
        # this face at all. Reject only disjoint bounding boxes before the exact
        # clipping/depth proof, preserving its original face order and tolerance.
        indices = np.asarray(sorted(candidates), dtype=np.intp)
        lower, upper = clipped.min(0), clipped.max(0)
        overlaps = np.all(self.upper[indices] >= lower, axis=1) & np.all(self.lower[indices] <= upper, axis=1)
        for index in indices[overlaps]:
            if self.face_ids[index] == face:
                continue
            occluder = self.triangles[index]
            overlap = clip_triangle(clipped,occluder)
            if area(overlap) < 1e-12:
                continue
            try:
                front = np.linalg.solve(np.column_stack((occluder,np.ones(3))),self.depths[index])
            except np.linalg.LinAlgError:
                continue
            visible -= area(positive_depth(overlap,front-plane))
            if visible <= total*.9:
                break
        return float(np.clip(visible/total,0,1))


@dataclass
class BoundaryView:
    family: str
    transform: np.ndarray
    contour: np.ndarray
    visible: set
    scale: float
    residual: float = 0.
    visibility: object = None

    def projected(self, corners):
        return np.column_stack((corners, np.ones(len(corners)))) @ self.transform * self.scale

    def coverage(self, corners, face=None):
        triangle = self.projected(corners)
        total = area(triangle)
        fraction = min(1., area(clip_triangle(self.contour, triangle))/total) if total > 1e-9 else 0.
        extent = max(np.linalg.norm(triangle[i]-triangle[(i+1)%3]) for i in range(3))
        visible = self.visibility.fraction(face,triangle/self.scale,corners) if self.visibility is not None else 1.
        return fraction, float(extent), visible


def reconstruct_projection(projection, vertices, faces):
    y, x = np.where(projection.valid & (projection.ids >= 0))
    if len(y) < 32:
        raise ValueError('Projection witnesses unavailable')
    take = np.linspace(0, len(y)-1, min(4096,len(y)), dtype=int)
    y, x = y[take], x[take]
    world = np.einsum('ij,ijk->ik', projection.barycentric[y,x], vertices[faces[projection.ids[y,x]]])
    source = np.column_stack((world, np.ones(len(world))))
    # Exact face-ID/barycentric pixel witnesses recover the saved orthographic
    # camera, independently of landmark depth predictions.
    target = np.column_stack((x+.5, y+.5))
    transform, _, rank, _ = np.linalg.lstsq(source, target, rcond=None)
    residual = float(np.max(np.linalg.norm(source @ transform-target, axis=1)))
    if rank != 4 or residual > .05 or not np.isfinite(transform).all():
        raise ValueError('Saved camera projection cannot be recovered')
    return transform, residual


def source_brow_contour(projection, accepted, core, permitted, legal, landmark_band, all_components=False):
    from local_face_landmarks import polygon_mask
    ids,valid = projection.ids,projection.valid
    candidate = valid & np.isin(ids,list(set(accepted) | set(permitted)))
    band = polygon_mask(landmark_band,ids.shape)
    nearby = cv2.dilate(candidate.astype(np.uint8),np.ones((7,7),np.uint8)).astype(bool)
    skin = valid & np.isin(ids,list(set(legal)-set(accepted))) & ~band & nearby
    if skin.sum() < BROW_MIN_SEED:
        raise ValueError('BROW_SOURCE_BACKGROUND_INSUFFICIENT')
    lab = cv2.cvtColor(projection.rgb.astype(np.float32)/255.,cv2.COLOR_RGB2Lab)
    skin_lightness = float(np.median(lab[skin,0]))
    # Original pigment and R4 core jointly seed the source segmentation. A
    # dark hair/eyelid pixel outside the legal one-ring set is never a seed.
    fg = valid & np.isin(ids,list(core)) & band & (lab[:,:,0] < skin_lightness-BROW_SOURCE_DELTA_L)
    if fg.sum() < BROW_MIN_SEED:
        raise ValueError('BROW_SOURCE_FOREGROUND_INSUFFICIENT')
    location = np.argwhere(candidate)
    lo,hi = np.maximum(location.min(0)-8,0),np.minimum(location.max(0)+9,ids.shape)
    crop = np.s_[lo[0]:hi[0],lo[1]:hi[1]]
    mask = np.full(projection.rgb[crop].shape[:2],cv2.GC_BGD,dtype=np.uint8)
    mask[candidate[crop]] = cv2.GC_PR_BGD
    mask[candidate[crop] & band[crop]] = cv2.GC_PR_FGD
    mask[skin[crop]] = cv2.GC_BGD
    mask[fg[crop]] = cv2.GC_FGD
    cv2.setRNGSeed(0)
    cv2.grabCut(np.ascontiguousarray(projection.rgb[crop]),mask,None,
                np.zeros((1,65)),np.zeros((1,65)),BROW_ITERATIONS,cv2.GC_INIT_WITH_MASK)
    foreground = np.isin(mask,[cv2.GC_FGD,cv2.GC_PR_FGD]) & candidate[crop]
    _,components = cv2.connectedComponents(foreground.astype(np.uint8))
    seed_components = np.unique(components[fg[crop]])
    seed_components = seed_components[seed_components != 0]
    foreground &= np.isin(components,seed_components)
    full = np.zeros(ids.shape,dtype=np.uint8)
    full[crop] = foreground.astype(np.uint8)
    scale = ROI_SIZE/max(ids.shape)
    size = (int(round(ids.shape[1]*scale)),int(round(ids.shape[0]*scale)))
    normalized = cv2.resize(full.astype(np.float32),size,interpolation=cv2.INTER_LINEAR)
    smoothed = cv2.GaussianBlur(normalized,(0,0),BROW_SMOOTH_SIGMA) >= .5
    contours,_ = cv2.findContours(smoothed.astype(np.uint8),cv2.RETR_EXTERNAL,cv2.CHAIN_APPROX_NONE)
    if not contours:
        raise ValueError('BROW_SOURCE_BOUNDARY_UNAVAILABLE')
    supported = [c.reshape(-1,2).astype(float)+.5 for c in contours if cv2.contourArea(c) >= 1]
    if not supported:
        raise ValueError('BROW_SOURCE_BOUNDARY_UNAVAILABLE')
    contour = supported if all_components else max(supported,key=area)
    return contour,{'foreground_seed_pixels':int(fg.sum()),'source_pixels':int(foreground.sum()),
                    'component_count':len(seed_components),'retained_contours':len(supported) if all_components else 1,
                    'source_delta_l':skin_lightness-float(np.median(lab[fg,0]))}


def boundary_band(accepted, neighbors):
    accepted = set(accepted)
    boundary = {f for f in accepted if any(int(n) not in accepted for n in neighbors[f])}
    ring = boundary | {int(n) for f in boundary for n in neighbors[f] if int(n) >= 0}
    return boundary, ring


def refinement_roots(accepted, neighbors, legal, blocked=()):
    boundary, ring = boundary_band(accepted, neighbors)
    return (boundary | (ring-set(accepted))) & set(legal) - set(blocked)


def refine_root(face, corners, views, original, remaining):
    """Return a complete root partition, or a local R4 fallback with a reason."""
    by_family = {}
    for view in views:
        if face in view.visible:
            by_family[view.family] = view
    active = list(by_family.values())
    if len(active) < 2:
        return [(LeafKey(face), original)], 'R4_LOCAL_VIEW_FALLBACK', 0.
    pending, output, max_error = [LeafKey(face)], [], 0.
    while pending:
        leaf = pending.pop()
        positions = leaf.corners() @ corners
        samples = [view.coverage(positions,face) for view in active]
        fractions, extents, visibility = np.asarray(samples).T
        crossing = (fractions > 1e-5) & (fractions < 1-1e-5)
        # A changed leaf needs two independent positive view witnesses. View
        # disagreement leaves this root unchanged rather than enlarging it.
        if crossing.any() and max(extents[crossing]) > MAX_EDGE_PIXELS:
            if leaf.depth == MAX_DEPTH:
                return [(LeafKey(face), original)], 'R4_LOCAL_PRECISION_FALLBACK', float(max(extents[crossing]))
            if len(output)+len(pending)+4-1 > remaining:
                return [(LeafKey(face), original)], 'R4_LOCAL_BUDGET_FALLBACK', 0.
            pending.extend(reversed(leaf.children()))
            continue
        confirmed = visibility >= .9
        if confirmed.sum() < 2:
            return [(LeafKey(face), original)], 'R4_LOCAL_OCCLUSION_FALLBACK', 0.
        fractions = fractions[confirmed]
        votes = int((fractions >= .5).sum())
        keep = votes >= 2 and votes > len(fractions)/2
        reject = len(fractions)-votes >= 2 and len(fractions)-votes > len(fractions)/2
        if not keep and not reject:
            return [(LeafKey(face), original)], 'R4_LOCAL_DISAGREEMENT_FALLBACK', 0.
        if crossing.any():
            max_error = max(max_error, float(max(extents[crossing])))
        output.append((leaf, keep))
    return sorted(output), 'BOUNDARY_REFINED', max_error
