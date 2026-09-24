"""Optional local eye shape hints, never semantic confidence or paint authority.

MediaPipe Face Landmarker (Apache-2.0) runs on existing original-texture renders.
Only raster-visible faces already accepted as a FaRL eye can become an iris.
The best resolved view defines a smooth ellipse clipped by its eyelid polygon;
silhouettes from different views are deliberately not unioned into a larger eye.
"""
import numpy as np

EYES = (
    ([33, 246, 161, 160, 159, 158, 157, 173, 133, 155, 154, 153, 145, 144, 163, 7], 468, [469, 470, 471, 472]),
    ([362, 398, 384, 385, 386, 387, 388, 466, 263, 249, 390, 373, 374, 380, 381, 382], 473, [474, 475, 476, 477]),
)


def load(path):
    import mediapipe as mp
    options = mp.tasks.vision.FaceLandmarkerOptions(
        base_options=mp.tasks.BaseOptions(model_asset_path=str(path)), num_faces=8,
        min_face_detection_confidence=.7, min_face_presence_confidence=.8)
    return mp.tasks.vision.FaceLandmarker.create_from_options(options)


def eye_masks(points, shape, eye):
    """Pixel-center ellipse/lid intersection; reject tiny, closed or invalid eyes."""
    contour, center_index, rim = eye
    p = np.asarray(points, dtype=float)
    if p.shape != (478, 2) or not np.isfinite(p).all():
        return None
    polygon, center = p[contour], p[center_index]
    axis = polygon[8] - polygon[0]
    width = np.linalg.norm(axis)
    if width < 12 or width > min(shape) * .45:
        return None
    axis /= width
    up = np.array([-axis[1], axis[0]])
    radii = np.max(np.abs((p[rim] - center) @ np.column_stack((axis, up))), axis=0)
    opening = np.ptp((polygon - center) @ up)
    if (np.any(radii < 2) or np.any(radii > width * .5) or
            opening < width * .10 or opening > width * .85 or
            np.linalg.norm(center - polygon.mean(axis=0)) > width * .4):
        return None
    yy, xx = np.indices(shape, dtype=float)
    xx += .5
    yy += .5
    inside = np.zeros(shape, dtype=bool)
    # Even-odd polygon fill with fixed pixel-center convention.
    for a, b in zip(polygon, np.roll(polygon, -1, axis=0)):
        if abs(b[1] - a[1]) > 1e-9:
            inside ^= ((a[1] > yy) != (b[1] > yy)) & (xx < (b[0]-a[0])*(yy-a[1])/(b[1]-a[1])+a[0])
    dx, dy = xx-center[0], yy-center[1]
    ellipse = ((dx*axis[0]+dy*axis[1])/radii[0])**2 + ((dx*up[0]+dy*up[1])/radii[1])**2 <= 1
    iris = inside & ellipse
    if inside.sum() < 24 or iris.sum() < 8 or not .08 <= iris.sum()/inside.sum() <= .8:
        return None
    return inside, iris, float(width)


def observe(detector, rgb, ids):
    if detector is None:
        return []
    import mediapipe as mp
    result = detector.detect(mp.Image(image_format=mp.ImageFormat.SRGB, data=np.ascontiguousarray(rgb)))
    observations = []
    for landmarks in result.face_landmarks:
        p = np.array([[v.x*ids.shape[1], v.y*ids.shape[0]] for v in landmarks])
        for eye in EYES:
            masks = eye_masks(p, ids.shape, eye)
            if masks is None:
                continue
            aperture, iris, width = masks
            valid = aperture & (ids >= 0)
            if valid.sum() < .95 * aperture.sum():
                continue
            # A majority of a visible triangle must lie in the ellipse. This
            # avoids selecting every sliver that merely touches its outline.
            face_ids, total = np.unique(ids[valid], return_counts=True)
            dark, counts = np.unique(ids[iris & valid], return_counts=True)
            chosen = dark[counts >= total[np.searchsorted(face_ids, dark)] * .5]
            observations.append((width, face_ids, chosen))
    return observations


def associate(observations, regions):
    hints = []
    for region in regions:
        if region['label'] not in ('re', 'le'):
            continue
        eye = np.array([s[0] for s in region['samples']], dtype=np.int64)
        best = None
        for width, aperture, iris in observations:
            allowed = np.intersect1d(eye, aperture)
            detail = np.intersect1d(allowed, iris)
            # Both models must agree on the location, not just one stray face.
            if (len(allowed) < max(8, len(eye)*.35) or len(allowed) < len(aperture)*.5 or
                    len(detail) < 4 or not .08 <= len(detail)/len(allowed) <= .85):
                continue
            # Confidence filtering leaves legitimate gaps in a frontal eye.
            # Require majority agreement, but do not prefer an occluded tiny
            # profile merely because its smaller polygon has fewer unknowns.
            score = width * len(allowed)/len(aperture) * np.sqrt(len(allowed)/len(eye))
            if best is None or score > best[0]:
                best = (score, allowed, detail)
        if best is not None:
            hints.append({'subject_id': region['subject_id'], 'label': region['label'],
                          'aperture_faces': best[1].tolist(), 'iris_faces': best[2].tolist()})
    return hints
