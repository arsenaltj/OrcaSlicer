"""Conservative visible-surface projection for FaRL face19 observations.

Pure NumPy data processing, adapted from the offline multi-view prototype. This
does not establish mesh correspondence, recognize a whole body, or calibrate ML
confidence. The host must prove the render/native face binding separately.
Background and unsupported labels participate in voting but never become regions.
No teeth, pupil, or eye-white labels are inferred from the model's eye/mouth labels.
"""
from __future__ import annotations

from dataclasses import dataclass
import hashlib
import re

import numpy as np

LABEL_SCHEMA = "farl-celebm-face19-subset-v1"
LABEL_NAMES = ("background", "neck", "face", "cloth", "rr", "lr", "rb", "lb",
               "re", "le", "nose", "imouth", "llip", "ulip", "hair", "eyeg",
               "hat", "earr", "neck_l")
SUPPORTED_LABELS = ("neck", "face", "rr", "lr", "rb", "lb", "re", "le",
                    "nose", "imouth", "llip", "ulip", "hair")
_SUPPORTED = np.array([name in SUPPORTED_LABELS for name in LABEL_NAMES])
MIN_CONFIDENCE = .9
MIN_DOMINANCE = .85
MAX_PIXELS = 16 * 1024 * 1024
MAX_FACES = 2_000_000


@dataclass(frozen=True)
class Observation:
    """One detection in one view; all arrays share the full raster's H x W shape.

    face_ids uses -1 for background; confidence is the winning label probability.
    Separate detections in the same view must use the same view_id. Cropped arrays
    must already be aligned to actual raster pixels by the caller. No array is
    modified or retained in the returned result. view_family groups correlated
    parent/focus views for voting; None preserves the view_id as its own family.
    """
    view_id: str
    face_ids: np.ndarray
    labels: np.ndarray
    confidence: np.ndarray
    detection_score: float
    view_family: str | None = None


def _validate(observations, face_count):
    if type(face_count) is not int or not 0 < face_count <= MAX_FACES:
        raise ValueError("Invalid native face count")
    if not isinstance(observations, (list, tuple)) or len(observations) > 128:
        raise ValueError("Observations must be a bounded list or tuple")
    views, rasters, families, pixels = {}, {}, {}, 0
    for item in observations:
        if not isinstance(item, Observation):
            raise ValueError("Invalid observation")
        if not isinstance(item.view_id, str) or not re.fullmatch(r"[A-Za-z0-9_.-]{1,96}", item.view_id):
            raise ValueError("Invalid view identifier")
        family = item.view_id if item.view_family is None else item.view_family
        if not isinstance(family, str) or not re.fullmatch(r"[A-Za-z0-9_.-]{1,96}", family):
            raise ValueError("Invalid view family identifier")
        if item.view_id in families and families[item.view_id] != family:
            raise ValueError("One view cannot belong to different families")
        families[item.view_id] = family
        views[item.view_id] = views.get(item.view_id, 0) + 1
        if len(views) > 16 or views[item.view_id] > 8:
            raise ValueError("View or detection limit exceeded")
        ids, labels, scores = item.face_ids, item.labels, item.confidence
        if any(type(a) is not np.ndarray for a in (ids, labels, scores)):
            raise ValueError("Observation arrays must be NumPy arrays")
        if ids.ndim != 2 or not all(0 < n <= 1024 for n in ids.shape):
            raise ValueError("Invalid raster shape")
        pixels += ids.size
        if pixels > MAX_PIXELS:
            raise ValueError("Total pixel limit exceeded")
        if labels.shape != ids.shape or scores.shape != ids.shape:
            raise ValueError("Observation arrays have different shapes")
        if ids.dtype.kind not in "iu" or labels.dtype != np.dtype("uint8") or scores.dtype.kind != "f":
            raise ValueError("Invalid observation array dtype")
        if np.any(ids < -1) or np.any(ids >= face_count) or np.any(labels >= len(LABEL_NAMES)):
            raise ValueError("Face or label index out of range")
        if item.view_id in rasters and not np.array_equal(ids, rasters[item.view_id]):
            raise ValueError("Detections of one view must share the same face raster")
        rasters[item.view_id] = ids
        if not np.isfinite(scores).all() or np.any(scores < 0) or np.any(scores > 1):
            raise ValueError("Invalid pixel confidence")
        score = item.detection_score
        if isinstance(score, (bool, np.bool_)) or not isinstance(score, (int, float, np.integer, np.floating)):
            raise ValueError("Invalid detection confidence")
        if not np.isfinite(score) or not 0 <= score <= 1:
            raise ValueError("Invalid detection confidence")
    return len(views), pixels


def _observation(item):
    visible = item.face_ids >= 0
    supported = visible & _SUPPORTED[item.labels]
    claim = np.unique(item.face_ids[supported]).astype(np.int64)
    support = np.unique(item.face_ids[supported & (item.confidence >= MIN_CONFIDENCE)]).astype(np.int64)
    # Preserve minority, low-score and unsupported observations in each claimed
    # face's denominator. Filtering them first would manufacture high dominance.
    select = visible & np.isin(item.face_ids, claim)
    keys = item.face_ids[select].astype(np.int64) * 19 + item.labels[select]
    keys, inverse, counts = np.unique(keys, return_inverse=True, return_counts=True)
    # Center near the acceptance boundary before accumulation. This avoids
    # adding a million copies of .9 and needlessly losing precision at the gate.
    centered = np.minimum(item.confidence[select], item.detection_score) - MIN_CONFIDENCE
    confidence = np.bincount(inverse, weights=centered) + MIN_CONFIDENCE * counts
    faces, face_inverse = np.unique(keys // 19, return_inverse=True)
    totals = np.bincount(face_inverse, weights=counts)
    return dict(view=item.view_id, family=item.view_id if item.view_family is None else item.view_family,
                claim=claim, support=support, keys=keys,
                counts=counts, votes=counts / totals[face_inverse],
                confidence=confidence / totals[face_inverse])


def project(observations, face_count):
    """Return {subjects, regions, statistics}, without an identity/proof envelope.

    Cross-view association requires >=3 shared high-confidence supported faces
    and >=20% of the smaller observed surface. A component containing two
    detections of one view is ambiguous in its entirety. Distinct components may
    survive independently, but any shared claimed face remains unknown.

    Within a component, each view family contributes at most one unit per face.
    Observations in one family share that unit equally wherever they observe the
    face. Winning dominance is its fraction of those units; confidence is the
    corresponding mean probability, capped by detection confidence. Winning view
    support counts distinct families; pixel support counts actual winning pixels,
    not independent rays. None defaults to view_id for existing callers. No
    resolution weighting, filling, smoothing, or user-authority fields are added.
    """
    view_count, pixel_count = _validate(observations, face_count)
    visible = np.unique(np.concatenate([o.face_ids[o.face_ids >= 0] for o in observations])) if observations else np.array([], dtype=np.int64)
    items = [_observation(o) for o in observations if o.detection_score >= MIN_CONFIDENCE]
    items = [o for o in items if len(o["support"])]
    # Canonical traversal makes floating accumulation and output independent of
    # input detection order. Same-view ties can only be separate or ambiguous.
    items.sort(key=lambda o: (o["view"], o["support"].astype("<u4").tobytes()))
    parent = list(range(len(items)))

    def find(i):
        while parent[i] != i:
            parent[i] = parent[parent[i]]
            i = parent[i]
        return i

    for i, a in enumerate(items):
        for j in range(i):
            b = items[j]
            if a["view"] == b["view"]:
                continue
            shared = np.intersect1d(a["support"], b["support"], assume_unique=True).size
            if shared >= 3 and shared * 5 >= min(len(a["support"]), len(b["support"])):
                parent[find(i)] = find(j)
    groups = {}
    for i in range(len(items)):
        groups.setdefault(find(i), []).append(items[i])
    if len(groups) > 32:
        raise ValueError("Subject limit exceeded")
    components = list(groups.values())
    claims = [np.unique(np.concatenate([o["claim"] for o in group])) for group in components]
    if claims:
        ownership, counts = np.unique(np.concatenate(claims), return_counts=True)
        cross_subject = ownership[counts > 1]
    else:
        cross_subject = np.array([], dtype=np.int64)
    ambiguous_parts = [cross_subject]
    regions, subjects = [], []
    rejected_parts, ambiguous_components = [], 0
    for group, claim in zip(components, claims):
        if len({o["view"] for o in group}) != len(group):
            ambiguous_parts.append(claim)
            ambiguous_components += 1
            continue
        sid = "surface-" + hashlib.sha256(claim.astype("<u4").tobytes()).hexdigest()
        keys, inverse = np.unique(np.concatenate([o["keys"] for o in group]), return_inverse=True)
        family_groups = {}
        for o in group:
            family_groups.setdefault(o["family"], []).append(o)
        divisors, family_keys = {}, []
        for family, members in family_groups.items():
            family_faces, counts = np.unique(np.concatenate([np.unique(o["keys"] // 19) for o in members]), return_counts=True)
            divisors[family] = (family_faces, counts)
            # A winning label present in multiple crops of one family still has
            # one view-support unit. Other labels remain in the vote denominator.
            family_keys.append(np.unique(np.concatenate([o["keys"] for o in members])))
        weights, confidence_weights = [], []
        for o in group:
            family_faces, counts = divisors[o["family"]]
            divisor = counts[np.searchsorted(family_faces, o["keys"] // 19)]
            weights.append(o["votes"] / divisor)
            confidence_weights.append(o["confidence"] / divisor)
        votes = np.bincount(inverse, weights=np.concatenate(weights))
        confidence = np.bincount(inverse, weights=np.concatenate(confidence_weights))
        pixels = np.bincount(inverse, weights=np.concatenate([o["counts"] for o in group])).astype(np.int64)
        support_keys, views = np.unique(np.concatenate(family_keys), return_counts=True)
        # Both unions enumerate exactly the same sorted face/label keys.
        assert np.array_equal(support_keys, keys)
        faces, starts, face_inverse = np.unique(keys // 19, return_index=True, return_inverse=True)
        totals = np.bincount(face_inverse, weights=votes)
        best = np.maximum.reduceat(votes, starts)
        # Lowest numeric label wins exact ties, but no tie can reach .85.
        winners = np.flatnonzero(votes == best[face_inverse])
        _, first = np.unique(keys[winners] // 19, return_index=True)
        winners = winners[first]
        label = keys[winners] % 19
        dominance = votes[winners] / totals
        score = confidence[winners] / votes[winners]
        # Only roundoff at the fixed gates, not a relaxed ML score threshold.
        score[np.abs(score - MIN_CONFIDENCE) <= 8 * np.finfo(np.float64).eps] = MIN_CONFIDENCE
        dominance[np.abs(dominance - MIN_DOMINANCE) <= 8 * np.finfo(np.float64).eps] = MIN_DOMINANCE
        eligible = (_SUPPORTED[label] & (score >= MIN_CONFIDENCE) & (dominance >= MIN_DOMINANCE)
                    & ((pixels[winners] >= 2) | (views[winners] >= 2)))
        eligible &= ~np.isin(faces, cross_subject)
        rejected_parts.append(faces[~eligible])
        if not np.any(eligible):
            continue
        subjects.append(sid)
        for number, name in enumerate(LABEL_NAMES):
            selected = np.flatnonzero(eligible & (label == number))
            if len(selected):
                samples = [[int(faces[i]), float(np.clip(score[i], 0, 1)),
                            float(np.clip(dominance[i], 0, 1)), int(pixels[winners[i]]), int(views[winners[i]])]
                           for i in selected]
                regions.append(dict(subject_id=sid, label=name, samples=samples))
    ambiguous = np.unique(np.concatenate(ambiguous_parts))
    rejected = np.unique(np.concatenate(rejected_parts)) if rejected_parts else np.array([], dtype=np.int64)
    known = sum(len(r["samples"]) for r in regions)
    statistics = dict(face_count=face_count, observations=len(observations), views=view_count,
                      view_families=len({o.view_id if o.view_family is None else o.view_family for o in observations}),
                      raw_pixels=pixel_count, visible_faces=len(visible), unseen_faces=face_count-len(visible),
                      associated_components=len(components), ambiguous_components=ambiguous_components,
                      ambiguous_faces=len(ambiguous), cross_subject_faces=len(cross_subject),
                      below_threshold_faces=int(np.setdiff1d(rejected, ambiguous).size),
                      known_faces=known, unknown_faces=face_count-known)
    return dict(subjects=sorted(subjects), regions=sorted(regions, key=lambda r: (r["subject_id"], r["label"])),
                statistics=statistics)
