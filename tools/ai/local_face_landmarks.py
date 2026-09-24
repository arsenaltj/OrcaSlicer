"""Multi-view facial shape hints on an already known, raster-visible surface.

Landmark geometry is independent of FaRL class confidence. The parser identifies
the person; corresponding landmark contours must agree across distinct cameras.
No inferred z coordinate, silhouette union, or recolored input is used.
"""
from dataclasses import dataclass
import numpy as np
from local_eye_landmarks import EYES, eye_masks

# Ordered eye/lip contours in the official MediaPipe 478-point topology.
# A nose has no comparable sharp pigment boundary; keep the parser's nose mask.
UPPER_OUTER = [61,185,40,39,37,0,267,269,270,409,291]
LOWER_OUTER = [61,146,91,181,84,17,314,405,321,375,291]
UPPER_INNER = [78,191,80,81,82,13,312,311,310,415,308]
LOWER_INNER = [78,95,88,178,87,14,317,402,318,324,308]
CONTOURS = {
    're': EYES[0][0], 'le': EYES[1][0],
    'ulip': UPPER_OUTER + UPPER_INNER[::-1],
    'llip': LOWER_OUTER + LOWER_INNER[::-1],
    'imouth': UPPER_INNER + LOWER_INNER[::-1],
}
OVAL = [10,338,297,332,284,251,389,356,454,323,361,288,397,365,379,
        378,400,377,152,148,176,149,150,136,172,58,132,93,234,127,162,21,54,103,67,109]
HEAD_LABELS = {'face','nose','re','le','ulip','llip','imouth','rb','lb'}


def polygon_mask(points, shape):
    p = np.asarray(points, dtype=float)
    if p.ndim != 2 or p.shape[1] != 2 or len(p) < 3 or not np.isfinite(p).all():
        raise ValueError('Invalid landmark contour')
    result = np.zeros(shape, dtype=bool)
    lo = np.maximum(np.floor(p.min(0)).astype(int), 0)
    hi = np.minimum(np.ceil(p.max(0)).astype(int)+1, shape[::-1])
    if np.any(hi <= lo):
        return result
    yy, xx = np.mgrid[lo[1]:hi[1], lo[0]:hi[0]]
    xx, yy = xx+.5, yy+.5
    inside = np.zeros(xx.shape, dtype=bool)
    for a, b in zip(p, np.roll(p, -1, axis=0)):
        if abs(b[1]-a[1]) > 1e-9:
            inside ^= ((a[1] > yy) != (b[1] > yy)) & (xx < (b[0]-a[0])*(yy-a[1])/(b[1]-a[1])+a[0])
    result[lo[1]:hi[1], lo[0]:hi[0]] = inside
    return result


@dataclass
class FaceView:
    family: str
    points: np.ndarray
    world: np.ndarray
    valid: np.ndarray
    scale: float
    pixel_size: float
    visible: np.ndarray
    counts: np.ndarray
    head: np.ndarray
    parts: dict
    irises: dict
    quality: float


def from_points(points, ids, bary, vertices, faces, camera, family):
    """Lift actual visible samples. MediaPipe's predicted depth is discarded."""
    p = np.asarray(points, dtype=float)
    if p.shape != (478, 2) or not np.isfinite(p).all():
        return None
    interocular = np.linalg.norm((p[33]+p[133]-p[362]-p[263])/2)
    if interocular < 24:
        return None
    xy = np.floor(p).astype(int)
    in_frame = (xy[:,0]>=0) & (xy[:,1]>=0) & (xy[:,0]<ids.shape[1]) & (xy[:,1]<ids.shape[0])
    xy = np.clip(xy, [0,0], np.array(ids.shape[::-1])-1)
    selected = ids[xy[:,1],xy[:,0]]
    valid = in_frame & (selected >= 0)
    world = np.zeros((478,3), dtype=float)
    world[valid] = np.einsum('ij,ijk->ik', bary[xy[valid,1],xy[valid,0]], vertices[faces[selected[valid]]])
    if not valid[[33,133,362,263,1,61,291]].all():
        return None
    scale = np.linalg.norm((world[33]+world[133]-world[362]-world[263])/2)
    if not np.isfinite(scale) or scale <= 0:
        return None
    visible, counts = np.unique(ids[ids>=0], return_counts=True)
    oval = polygon_mask(p[OVAL], ids.shape)
    if oval.sum() < 100 or ((ids>=0)&oval).sum() < .95*oval.sum():
        return None
    head = np.unique(ids[oval & (ids>=0)])
    parts, irises = {}, {}
    def samples(mask):
        selected, inside = np.unique(ids[mask & (ids>=0)], return_counts=True)
        return selected, inside/counts[np.searchsorted(visible,selected)]
    for name, contour in CONTOURS.items():
        mask = polygon_mask(p[contour], ids.shape) & oval
        if name in ('re','le'):
            eye = eye_masks(p, ids.shape, EYES[0 if name=='re' else 1])
            if eye is None:
                continue  # Closed/too small eyes are not fabricated.
            mask, iris, _ = eye
            irises[name] = samples(iris & oval)
        if mask.sum() >= 12:
            parts[name] = samples(mask)
    # Frontal, resolved views have larger apparent eye spacing at equal scale.
    pixel_size = 2*camera.half_height/camera.size
    quality = interocular * min(1., interocular*pixel_size/scale)
    return FaceView(family,p,world,valid,float(scale),pixel_size,visible,counts,head,parts,irises,float(quality))


def observe(detector, rgb, ids, bary, vertices, faces, camera, family):
    if detector is None:
        return []
    import mediapipe as mp
    result = detector.detect(mp.Image(image_format=mp.ImageFormat.SRGB, data=np.ascontiguousarray(rgb)))
    views = []
    for landmarks in result.face_landmarks:
        p = np.array([[v.x*ids.shape[1],v.y*ids.shape[0]] for v in landmarks])
        observation = from_points(p,ids,bary,vertices,faces,camera,family)
        if observation is not None:
            views.append(observation)
    return views


def detail_cameras(vertices, faces, regions):
    """Bounded crops from accepted faces in the native Z-up coordinate system.

    Occluded/tilted faces still need detection and cross-view agreement. Cameras
    alone grant no labels. Limit extra work to the two largest detected faces.
    """
    from local_semantic_views import Camera
    regions = sorted((r for r in regions if r['label']=='face'), key=lambda r:-len(r['samples']))[:2]
    result = []
    for region in regions:
        indices = np.array([s[0] for s in region['samples']], dtype=int)
        if len(indices)<16:
            continue
        tri = vertices[faces[indices]].astype(float)
        normal = np.cross(tri[:,1]-tri[:,0],tri[:,2]-tri[:,0]).sum(0)
        normal[2] = 0
        length = np.linalg.norm(normal)
        height = np.ptp(tri[:,:,2])*.85
        if length<=1e-8 or height<=1e-8:
            continue
        normal /= length
        center = tri.mean((0,1))
        right = np.cross([0.,0.,1.],normal)
        for angle in (0,-25,25):
            radians = np.deg2rad(angle)
            direction = normal*np.cos(radians)+right*np.sin(radians)
            rr = np.cross([0.,0.,1.],direction)
            result.append(Camera('shape-'+region['subject_id']+'-'+str(angle),
                          np.stack((rr,np.cross(direction,rr),direction)),center,height,768))
    return result


def contour_distance(a, b, name):
    points = np.asarray(CONTOURS[name])
    shared = a.valid[points] & b.valid[points]
    if shared.sum() < max(4, len(points)*.7):
        return float('inf')
    distances = np.linalg.norm(a.world[points[shared]]-b.world[points[shared]],axis=1)
    # A robust contour discrepancy measured against eye spacing, with a bounded
    # raster sampling tolerance. Shared fixed contour points, not view silhouettes.
    return float(np.quantile(distances,.75)/max(a.scale,b.scale))


def consensus(views, name):
    candidates = [v for v in views if name in v.parts]
    best = []
    best_score = (-1,-1.)
    for seed in candidates:
        families = {}
        for v in candidates:
            tolerance = .045 + min(.035, 2*(seed.pixel_size+v.pixel_size)/max(seed.scale,v.scale))
            if contour_distance(seed,v,name) <= tolerance:
                if v.family not in families or v.quality > families[v.family].quality:
                    families[v.family] = v
        selected = list(families.values())
        score = (len(selected),sum(v.quality for v in selected))
        if score > best_score:
            best, best_score = selected, score
    return best if len(best)>=2 else []


def fuse_masks(views, name, iris=False):
    """Visible disagreement contributes negative votes; crops are not new views."""
    observed = [(v, (v.irises if iris else v.parts).get(name)) for v in views]
    observed = [(v,p) for v,p in observed if p is not None]
    if len(observed)<2:
        return np.array([],dtype=np.int64)
    union = np.unique(np.concatenate([p[0] for _,p in observed]))
    total, yes = np.zeros(len(union)),np.zeros(len(union))
    support = np.zeros(len(union),dtype=np.int32)
    max_quality = max(v.quality for v,_ in observed)
    for v, (faces,fraction) in observed:
        visible = np.isin(union,v.visible,assume_unique=True)
        weight = max(.1,v.quality/max_quality)
        total[visible] += weight
        at = np.searchsorted(union,faces)
        yes[at] += weight*fraction
        support[at[fraction>=.5]] += 1
    # No union-based growth. Two independent positive views and majority area
    # are necessary; even high-resolution crops cannot multiply their evidence.
    return union[(support>=2)&(yes>=.55*total)]


def surface_neighbors(vertices, faces):
    """Exact shared edges across UV seams; open/nonmanifold edges stop a path."""
    _, welded = np.unique(vertices, axis=0, return_inverse=True)
    triangles = welded[faces]
    edges = np.sort(triangles[:, [[0,1],[1,2],[2,0]]].reshape(-1,2), axis=1)
    _, inverse, counts = np.unique(edges, axis=0, return_inverse=True, return_counts=True)
    order = np.argsort(inverse, kind='stable')
    paired = order[counts[inverse[order]]==2].reshape(-1,2)
    neighbors = np.full(len(faces)*3, -1, dtype=np.int32)
    neighbors[paired[:,0]] = paired[:,1]//3
    neighbors[paired[:,1]] = paired[:,0]//3
    return neighbors.reshape(-1,3)


def context_paths(part, known, forbidden, visible, neighbors):
    """Keep context separate from paint: short visible paths to accepted skin.

    Two distinct known endpoints and at most eight shared edges are required.
    No camera/semantic confidence is changed and no ring becomes an eye label.
    """
    allowed = np.zeros(len(neighbors), dtype=bool)
    allowed[visible] = True
    allowed[list(forbidden)] = False
    targets = set(known)
    parents = {int(f):None for f in part if allowed[f]}
    frontier = sorted(parents)
    result = []
    for _ in range(8):
        next_frontier = []
        for f in frontier:
            for n in neighbors[f]:
                n = int(n)
                if n<0 or not allowed[n] or n in parents:
                    continue
                parents[n] = f
                if n in targets:
                    path = [n]
                    while parents[path[-1]] is not None:
                        path.append(parents[path[-1]])
                    result.append(path[::-1])
                    if len(result)==4:
                        return result
                else:
                    next_frontier.append(n)
        frontier = next_frontier
        if not frontier:
            break
    return result if len(result)>=2 else []


def associate(views, regions, vertices=None, triangles=None):
    subjects = {}
    blocked = {}
    for r in regions:
        sid = r['subject_id']
        subjects.setdefault(sid,[])
        blocked.setdefault(sid,[])
        target = subjects if r['label'] in HEAD_LABELS else blocked
        target[sid].extend(s[0] for s in r['samples'])
    groups = {sid:[] for sid in subjects}
    for view in views:
        scores = sorted(((len(np.intersect1d(view.head,faces)),sid) for sid,faces in subjects.items()),reverse=True)
        if not scores or scores[0][0] < max(16,.35*len(view.head)):
            continue
        if len(scores)>1 and scores[1][0]>.1*scores[0][0]:
            continue
        groups[scores[0][1]].append(view)
    hints = []
    neighbors = None
    for sid, observations in groups.items():
        forbidden = set(blocked[sid])
        for other,faces in subjects.items():
            if other!=sid:
                forbidden.update(faces)
                forbidden.update(blocked[other])
        for name in CONTOURS:
            selected = consensus(observations,name)
            if not selected:
                continue
            faces = np.setdiff1d(fuse_masks(selected,name),list(forbidden))
            if len(faces)<4:
                continue
            iris = np.intersect1d(faces,fuse_masks(selected,name,True)) if name in ('re','le') else np.array([],dtype=int)
            hint = {'subject_id':sid,'label':name,'faces':faces.tolist(),
                    'iris_faces':iris.tolist(),'view_support':len(selected)}
            if not len(np.intersect1d(faces,subjects[sid])):
                if vertices is None or triangles is None:
                    continue
                if neighbors is None:
                    neighbors = surface_neighbors(vertices, triangles)
                # Context itself must be visible from at least two agreeing
                # cameras, inside their face ovals, never through an occluder.
                head, support = np.unique(np.concatenate([v.head for v in selected]),return_counts=True)
                paths = context_paths(faces, subjects[sid], forbidden, head[support>=2], neighbors)
                if not paths:
                    continue
                hint['anchor_paths'] = paths
            hints.append(hint)
    # Conflicting boundaries remain unknown rather than depending on class order.
    if hints:
        faces, counts = np.unique(np.concatenate([h['faces'] for h in hints]),return_counts=True)
        conflicts = faces[counts>1]
        for h in hints:
            h['faces'] = np.setdiff1d(h['faces'],conflicts).tolist()
            h['iris_faces'] = np.intersect1d(h['faces'],h['iris_faces']).tolist()
            if 'anchor_paths' in h:
                h['anchor_paths'] = [p for p in h['anchor_paths'] if p[0] in h['faces']]
    return [h for h in hints if len(h['faces'])>=4 and
            (len(np.intersect1d(h['faces'],subjects[h['subject_id']]))>0 or len(h.get('anchor_paths',[]))>=2)]
