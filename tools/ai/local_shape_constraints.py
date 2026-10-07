"""Geometry-only gates for facial detail evidence.

The semantic parser proposes a coarse surface.  This module checks the proposal
against the landmark envelope and the already verified multi-view surface.  It
does not select colors, slots, or materials.
"""
from __future__ import annotations

import math
import re

import numpy as np


VALID_SHAPE = "VALID_SHAPE"
PROTECTED_SHAPE_UNCERTAIN = "PROTECTED_SHAPE_UNCERTAIN"
INVALID_SHAPE_CONFLICT = "INVALID_SHAPE_CONFLICT"
SHAPE_STATUSES = {VALID_SHAPE, PROTECTED_SHAPE_UNCERTAIN, INVALID_SHAPE_CONFLICT}
SHAPE_LABELS = {"re", "le", "ulip", "llip", "imouth", "lip-line-corner", "lb", "rb"}

DEFAULTS = {
    "iris_axis_ratio_min": 0.72,
    "iris_fit_residual_max": 0.35,
    "iris_center_offset_max": 0.40,
    "iris_area_fraction_min": 0.08,
    "iris_area_fraction_max": 0.80,
    "envelope_coverage_min": 0.90,
    "boundary_outside_max": 0.10,
    "component_min_faces": 4,
    "view_support_min": 2,
    "lip_cross_max": 0.05,
    "oral_boundary_mix_max": 0.10,
}


def _finite(value):
    return isinstance(value, (int, float, np.integer, np.floating)) and math.isfinite(float(value))


def ellipse_metrics(points, world, eye):
    """Fit a normalized ellipse to the eye rim.

    World-space landmarks are preferred because they remove most camera
    foreshortening.  The 2-D landmarks are a deterministic fallback for unit
    tests and installations without a complete world projection.
    """
    p = np.asarray(world if world is not None else points, dtype=float)
    image = np.asarray(points, dtype=float)
    contour, center_index, rim = eye
    if p.ndim != 2 or image.ndim != 2 or len(p) <= max([center_index, *rim]) or not np.isfinite(p).all():
        return None
    samples = p[np.asarray(rim, dtype=int)]
    center = p[center_index]
    centered = samples - center
    if centered.shape[0] < 4 or not np.isfinite(centered).all():
        return None
    covariance = np.cov(centered, rowvar=False)
    if covariance.ndim != 2 or covariance.shape[0] != covariance.shape[1]:
        return None
    eigenvalues = np.sort(np.maximum(np.linalg.eigvalsh(covariance), 0.0))
    # World landmarks lie on an approximately planar eye surface. Ignore the
    # normal-axis zero eigenvalue and compare the two in-plane axes.
    positive = eigenvalues[eigenvalues > eigenvalues[-1] * 1e-8]
    if len(positive) < 2 or positive[-1] <= 1e-12:
        return None
    axis_ratio = float(math.sqrt(positive[-2] / positive[-1]))
    radii = np.linalg.norm(centered, axis=1)
    mean_radius = float(np.mean(radii))
    if mean_radius <= 1e-12:
        return None
    fit_residual = float(np.std(radii) / mean_radius)
    image_center = image[center_index]
    image_rim = image[np.asarray(rim, dtype=int)]
    image_width = float(np.ptp(image_rim[:, 0]))
    center_offset = float(np.linalg.norm(image_center - image_rim.mean(axis=0)) /
                          max(image_width, 1e-12))
    return {
        "axis_ratio": axis_ratio,
        "circularity": axis_ratio,
        "fit_residual": fit_residual,
        "center_offset": center_offset,
    }


def _component_count(faces, neighbors):
    if neighbors is None:
        return 1 if len(faces) else 0
    selected = set(int(f) for f in faces)
    components = 0
    while selected:
        components += 1
        stack = [selected.pop()]
        while stack:
            face = stack.pop()
            for neighbor in neighbors[face]:
                neighbor = int(neighbor)
                if neighbor in selected:
                    selected.remove(neighbor)
                    stack.append(neighbor)
    return components


def _coverage(selected, label, faces):
    target = set(int(f) for f in faces)
    if not target:
        return 0.0, 0.0
    coverages = []
    outside = []
    for view in selected:
        item = view.parts.get(label)
        if item is None:
            continue
        available = {int(face): float(fraction) for face, fraction in zip(item[0], item[1])}
        inside = sum(max(0.0, min(1.0, available.get(face, 0.0))) for face in target) / len(target)
        coverages.append(inside)
        outside.append(1.0 - inside)
    if not coverages:
        return 0.0, 1.0
    return float(min(coverages)), float(max(outside))


def _overlap_with_parts(selected, label, faces, other_labels):
    """Measure a proposed detail's overlap with neighboring semantic bands.

    The raster parser may give the same root face to two broad landmark masks.
    Comparing the fused face set against the other masks catches that ambiguity
    before it becomes a shape detail. Missing neighboring masks are unknown,
    rather than evidence of a conflict.
    """
    target = set(int(f) for f in faces)
    if not target:
        return 0.0
    values = []
    for view in selected:
        current = view.parts.get(label)
        if current is None:
            continue
        for other_label in other_labels:
            other = view.parts.get(other_label)
            if other is None:
                continue
            other_faces = set(int(f) for f in other[0])
            values.append(len(target & other_faces) / len(target))
    return float(max(values, default=0.0))


_COMPLETION_HOPS = {
    "lb": 2,
    "rb": 2,
    "re": 1,
    "le": 1,
    "ulip": 1,
    "llip": 1,
    "imouth": 1,
    "lip-line-corner": 0,
}


def _base_record(label, status, faces, view_support, metrics, reasons,
                 nested_faces=None, accepted_faces=None, rejected_faces=None):
    accepted = faces if accepted_faces is None else accepted_faces
    rejected = [] if rejected_faces is None else rejected_faces
    record = {
        "subject_id": "",
        "label": label,
        "status": status,
        "accepted_faces": sorted(int(f) for f in accepted),
        "rejected_faces": sorted(int(f) for f in rejected),
        "view_support": int(view_support),
        "metrics": {key: float(value) for key, value in metrics.items() if _finite(value)},
        "reasons": list(dict.fromkeys(reasons)),
    }
    if nested_faces is not None:
        record["nested_faces"] = sorted(int(f) for f in nested_faces)
    return record


def _projected_faces(selected, label):
    result = set()
    for view in selected:
        item = view.parts.get(label)
        if item is not None:
            result.update(int(face) for face in item[0])
    return result


def _bounded_completion(label, faces, parent_faces, selected, neighbors, forbidden=None):
    """Close sparse raster seeds without leaving the semantic parent.

    The first ring around the projected envelope is allowed to bridge raster
    gaps; subsequent rings can only reach faces connected to an existing seed.
    This makes the result useful for coloring while keeping topology bounded.
    """
    seed = {int(face) for face in faces}
    if not seed or neighbors is None:
        return np.asarray(sorted(seed), dtype=np.int64)
    parent = {int(face) for face in parent_faces}
    blocked = {int(face) for face in (forbidden or ())}
    projected = _projected_faces(selected, label)
    if projected:
        envelope = set(projected)
        for face in tuple(projected):
            envelope.update(int(neighbor) for neighbor in neighbors[face] if int(neighbor) >= 0)
    else:
        envelope = set(parent)
    allowed = (parent & envelope) - blocked
    current = seed & allowed
    completed = set(current)
    hops = _COMPLETION_HOPS.get(label, 0)
    for _ in range(hops):
        frontier = set()
        for face in current:
            frontier.update(int(neighbor) for neighbor in neighbors[face] if int(neighbor) >= 0)
        current = (frontier & allowed) - completed
        if not current:
            break
        completed.update(current)
    return np.asarray(sorted(completed), dtype=np.int64)


def _hard_conflict(reasons, metrics, label):
    hard_markers = ("CROSS_SUBJECT", "CROSS_EYE", "PARENT_UNBOUND", "SOURCE_MAPPING")
    if any(any(marker in reason for marker in hard_markers) for reason in reasons):
        return True
    # Severe boundary mixing is an explicit ownership conflict. Small overlap
    # remains a risk so a usable conservative region can still be colored.
    if label == "lip-line-corner" and metrics.get("lip_surface_mix_fraction", 0.0) > 0.50:
        return True
    if label == "imouth" and metrics.get("oral_boundary_mix_fraction", 0.0) > 0.50:
        return True
    if label in ("ulip", "llip") and metrics.get("lip_cross_fraction", 0.0) > 0.25:
        return True
    return False


def evaluate(label, selected, faces, iris_faces=None, neighbors=None, eye=None, subject_id="",
             parent_faces=None, forbidden_faces=None):
    """Return a shape record and the accepted feature faces.

    `selected` contains one representative view per independent camera family.
    The function deliberately rejects missing support instead of inferring a
    shape from a single raster.
    """
    faces = np.unique(np.asarray(faces, dtype=np.int64))
    parent = faces if parent_faces is None else np.unique(np.asarray(parent_faces, dtype=np.int64))
    iris = np.unique(np.asarray(iris_faces if iris_faces is not None else [], dtype=np.int64))
    metrics = {"component_count": float(_component_count(faces, neighbors)),
               "face_count": float(len(faces)), "view_support": float(len(selected))}
    reasons = []
    if label not in SHAPE_LABELS:
        reasons.append("UNSUPPORTED_SHAPE_LABEL")
    if len(selected) < DEFAULTS["view_support_min"]:
        reasons.append("SHAPE_VIEW_SUPPORT_INSUFFICIENT")
    if len(faces) < DEFAULTS["component_min_faces"]:
        reasons.append("SHAPE_COMPONENT_TOO_SMALL")
    coverage, outside = _coverage(selected, label, faces)
    metrics["envelope_coverage"] = coverage
    metrics["boundary_outside"] = outside
    if coverage < DEFAULTS["envelope_coverage_min"]:
        reasons.append("SHAPE_ENVELOPE_COVERAGE_INSUFFICIENT")
    if outside > DEFAULTS["boundary_outside_max"]:
        reasons.append("SHAPE_BOUNDARY_OUTSIDE_ENVELOPE")
    if metrics["component_count"] > 1:
        reasons.append("SHAPE_COMPONENT_DISCONNECTED")

    if label in ("ulip", "llip"):
        other = "llip" if label == "ulip" else "ulip"
        metrics["lip_cross_fraction"] = _overlap_with_parts(selected, label, faces, (other,))
        if metrics["lip_cross_fraction"] > DEFAULTS["lip_cross_max"]:
            reasons.append("SHAPE_LIP_CROSSING")
    elif label == "lip-line-corner":
        metrics["lip_surface_mix_fraction"] = _overlap_with_parts(
            selected, label, faces, ("ulip", "llip"))
        if metrics["lip_surface_mix_fraction"] > DEFAULTS["oral_boundary_mix_max"]:
            reasons.append("SHAPE_LIP_LINE_SURFACE_MIX")
    elif label == "imouth":
        metrics["oral_boundary_mix_fraction"] = _overlap_with_parts(
            selected, label, faces, ("ulip", "llip", "lip-line-corner"))
        if metrics["oral_boundary_mix_fraction"] > DEFAULTS["oral_boundary_mix_max"]:
            reasons.append("SHAPE_ORAL_BOUNDARY_MIX")

    nested = []
    if label in ("re", "le"):
        eye_metrics = []
        for view in selected:
            required = [] if eye is None else [eye[1], *eye[2]]
            valid = getattr(view, "valid", None)
            # Minimal synthetic views used by offline contract tests predate
            # the explicit validity vector. Real FaceView instances always
            # provide it, so absence remains the legacy permissive path.
            has_projection = (valid is None or all(0 <= index < len(valid) and bool(valid[index])
                                                    for index in required))
            value = ellipse_metrics(view.points, view.world, eye) if has_projection else None
            if value is not None:
                eye_metrics.append(value)
        if not eye_metrics:
            reasons.append("PUPIL_SHAPE_UNAVAILABLE")
        else:
            metrics["iris_axis_ratio"] = min(v["axis_ratio"] for v in eye_metrics)
            metrics["iris_fit_residual"] = max(v["fit_residual"] for v in eye_metrics)
            metrics["iris_center_offset"] = max(v["center_offset"] for v in eye_metrics)
            if metrics["iris_axis_ratio"] < DEFAULTS["iris_axis_ratio_min"]:
                reasons.append("PUPIL_NOT_ROUND")
            if metrics["iris_fit_residual"] > DEFAULTS["iris_fit_residual_max"]:
                reasons.append("PUPIL_FIT_UNSTABLE")
            if metrics["iris_center_offset"] > DEFAULTS["iris_center_offset_max"]:
                reasons.append("PUPIL_CENTER_OUTSIDE_SOCKET")
        nested = iris.tolist()
        fraction = len(iris) / max(len(faces), 1)
        metrics["iris_area_fraction"] = fraction
        if not DEFAULTS["iris_area_fraction_min"] <= fraction <= DEFAULTS["iris_area_fraction_max"]:
            reasons.append("PUPIL_AREA_OUTSIDE_SOCKET")
        if not nested:
            reasons.append("PUPIL_SURFACE_EMPTY")

    candidate = _bounded_completion(label, faces, parent, selected, neighbors, forbidden_faces)
    if candidate.size == 0:
        reasons.append("SHAPE_PARENT_UNBOUND")
    hard = _hard_conflict(reasons, metrics, label)
    if hard or candidate.size == 0:
        status = INVALID_SHAPE_CONFLICT
        accepted = np.array([], dtype=np.int64)
    elif reasons:
        status = PROTECTED_SHAPE_UNCERTAIN
        accepted = candidate
    else:
        status = VALID_SHAPE
        accepted = candidate
    accepted_set = set(int(face) for face in accepted)
    rejected = sorted(set(int(face) for face in faces) - accepted_set)
    nested = [int(face) for face in nested if int(face) in accepted_set]
    record = _base_record(label, status, faces.tolist(), len(selected), metrics, reasons,
                          nested, accepted, rejected)
    record["subject_id"] = subject_id
    return record, accepted, nested


def validate_shape_record(record, face_count):
    """Validate the JSON-facing shape record without trusting its metrics."""
    if not isinstance(record, dict) or set(record) - {
            "subject_id", "label", "status", "accepted_faces", "rejected_faces",
            "view_support", "metrics", "reasons", "nested_faces"}:
        return False
    if not isinstance(record.get("subject_id"), str) or not re.fullmatch(r"[A-Za-z0-9_.-]{1,96}", record["subject_id"]):
        return False
    if record.get("label") not in SHAPE_LABELS or record.get("status") not in SHAPE_STATUSES:
        return False
    if type(record.get("view_support")) is not int or not 0 <= record["view_support"] <= 16:
        return False
    for key in ("accepted_faces", "rejected_faces", "nested_faces"):
        if key not in record:
            if key == "nested_faces":
                continue
            return False
        values = record[key]
        if (not isinstance(values, list) or values != sorted(set(values)) or
                any(type(v) is not int or not 0 <= v < face_count for v in values)):
            return False
    if set(record["accepted_faces"]) & set(record["rejected_faces"]):
        return False
    if "nested_faces" in record and not set(record["nested_faces"]) <= set(record["accepted_faces"]):
        return False
    if record.get("nested_faces") and record["label"] not in {"re", "le"}:
        return False
    if record["status"] == VALID_SHAPE and not record["accepted_faces"]:
        return False
    if not isinstance(record.get("metrics"), dict) or any(not isinstance(k, str) or not _finite(v)
                                                          for k, v in record["metrics"].items()):
        return False
    return isinstance(record.get("reasons"), list) and all(isinstance(v, str) for v in record["reasons"])
