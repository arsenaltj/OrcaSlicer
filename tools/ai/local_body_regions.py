"""Optional whole-person evidence; never expands ears or overwrites face parsing.

Only single-person, upright surface evidence is supported. Correlated crops are
not votes. Conflicts and poorly resolved triangles remain unknown.
"""
import copy

MODEL = 'selfie_multiclass_256x256.tflite'
SHA256 = 'c6748b1253a99067ef71f7e26ca71096cd449baefa8f101900ea23016507e0e0'
SIZE = 16371837


def load(path):
    from mediapipe.tasks import python
    from mediapipe.tasks.python import vision
    return vision.ImageSegmenter.create_from_options(vision.ImageSegmenterOptions(
        base_options=python.BaseOptions(model_asset_path=str(path)),
        running_mode=vision.RunningMode.IMAGE, output_confidence_masks=True,
        output_category_mask=False))


def observe(model, rgb, ids, family):
    import numpy as np
    import mediapipe as mp
    result = model.segment(mp.Image(image_format=mp.ImageFormat.SRGB,
                                   data=np.ascontiguousarray(rgb, dtype=np.uint8)))
    masks = np.stack([m.numpy_view().copy().squeeze() for m in result.confidence_masks], axis=-1)
    if masks.shape != (*ids.shape, 6) or not np.isfinite(masks).all():
        raise ValueError('Invalid body masks')
    return family, ids.copy(), masks.argmax(-1).astype(np.uint8), masks.max(-1)


def supplement(projection, observations, vertices, faces, blocked=()):
    """Fill only unknown faces with >=2 independent, agreeing high-quality views.

    FaceSkin/accessory predictions never enter the output. BodySkin is limited
    to a small neck zone, not reinterpreted as facial or ear evidence. The lowest
    10% of the object remains untouched (conservative pedestal exclusion, not a
    general-purpose base detector). No extrapolation onto unseen triangles.
    """
    import numpy as np
    if len(projection['subjects']) != 1 or not observations:
        return projection
    n = len(faces)
    if n > 2_000_000 or len(observations) > 8:
        raise ValueError('Body evidence budget exceeded')
    known = np.zeros(n, bool)
    head = []
    for region in projection['regions']:
        ids = [s[0] for s in region['samples']]
        known[ids] = True
        if region['label'] in ('face', 'nose'):
            head.extend(ids)
    known[list(blocked)] = True
    if not head:
        return projection
    centers = np.asarray(vertices)[np.asarray(faces)].mean(1)
    head_xyz = centers[head]
    low, high = np.quantile(head_xyz, [.02, .98], axis=0)
    head_height = high[2]-low[2]
    if head_height <= 0:
        return projection
    floor = vertices[:, 2].min()+np.ptp(vertices[:, 2])*.10
    totals = np.zeros(n, np.uint8)
    hits = np.zeros((n, 3), np.uint8)
    quality = np.zeros((n, 3), np.float32)
    pixels = np.zeros((n, 3), np.uint32)
    families = set()
    for family, ids, labels, confidence in observations:
        if family in families:
            raise ValueError('Correlated body views must not count twice')
        families.add(family)
        if (ids.shape != labels.shape or ids.shape != confidence.shape or ids.size > 1024**2
                or np.any(ids < -1) or np.any(ids >= n) or np.any(labels > 5)
                or not np.isfinite(confidence).all() or np.any(confidence < 0) or np.any(confidence > 1)):
            raise ValueError('Invalid body evidence')
        visible = ids >= 0
        count = np.bincount(ids[visible], minlength=n)
        totals[count > 0] += 1
        for k, label in enumerate((1, 2, 4)):
            selected = visible & (labels == label) & (confidence >= .9)
            amount = np.bincount(ids[selected], minlength=n)
            score = np.bincount(ids[selected], weights=confidence[selected], minlength=n)/np.maximum(count, 1)
            accepted = (score >= .9) & (amount >= 1)
            hits[:, k] += accepted
            quality[:, k] += np.where(accepted, score, 0)
            pixels[:, k] += np.where(accepted, amount, 0).astype(np.uint32)
    result = copy.deepcopy(projection)
    sid = result['subjects'][0]
    for k, label in enumerate(('hair', 'neck', 'cloth')):
        valid = (~known) & (hits[:, k] >= 2) & (hits[:, k] == totals) & (centers[:, 2] > floor)
        if label == 'cloth':
            valid &= centers[:, 2] < low[2]-.08*head_height
        elif label == 'neck':
            valid &= (centers[:, 2] < low[2]) & (centers[:, 2] > low[2]-.6*head_height)
            # Neck stays within the observed face's horizontal footprint.
            valid &= np.all((centers[:, :2] >= low[:2]-.15*head_height) &
                            (centers[:, :2] <= high[:2]+.15*head_height), axis=1)
        else:
            valid &= centers[:, 2] > low[2]-1.5*head_height
        selected = np.flatnonzero(valid)
        if not len(selected):
            continue
        samples = [[int(i), float(quality[i,k]/hits[i,k]), 1., int(pixels[i,k]), int(hits[i,k])] for i in selected]
        region = next((r for r in result['regions'] if r['subject_id']==sid and r['label']==label), None)
        if region is None:
            result['regions'].append(dict(subject_id=sid, label=label, samples=samples))
        else:
            region['samples'] = sorted(region['samples']+samples, key=lambda s:s[0])
        known[selected] = True
    result['regions'].sort(key=lambda r:(r['subject_id'],r['label']))
    count = sum(len(r['samples']) for r in result['regions'])
    result['statistics'].update(known_faces=count, unknown_faces=n-count)
    return result
