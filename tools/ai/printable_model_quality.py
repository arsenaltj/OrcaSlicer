#!/usr/bin/env python3
"""Deterministic structural quality checks for printable OBJ artifacts."""

from __future__ import annotations

from collections import Counter
from dataclasses import asdict, dataclass
import json
import math
import os
from pathlib import Path
from typing import Any


REPORT_SCHEMA_VERSION = 1
GATE_VERSION = "structural-v1"


@dataclass(frozen=True)
class ModelQualityThresholds:
    max_faces: int = 1_000_000
    ground_band_mm: float = 0.5
    min_contact_span_ratio: float = 0.005
    max_aspect_ratio: float = 20.0
    max_downward_area_ratio: float = 0.35
    short_edge_mm: float = 0.15
    max_short_edge_ratio: float = 0.50
    tiny_component_face_ratio: float = 0.001
    tiny_component_diagonal_ratio: float = 0.05
    degenerate_area_epsilon_mm2: float = 1e-10


class ModelQualityError(ValueError):
    def __init__(self, code: str, message: str):
        super().__init__(message)
        self.code = code


def _resolve_obj_index(token: str, vertex_count: int) -> int:
    raw = token.split("/", 1)[0]
    try:
        value = int(raw)
    except ValueError:
        raise ModelQualityError("invalid_vertex_index", "OBJ face contains an invalid vertex index.") from None
    index = value - 1 if value > 0 else vertex_count + value
    if value == 0 or index < 0 or index >= vertex_count:
        raise ModelQualityError("invalid_vertex_index", "OBJ face references a vertex outside the file.")
    return index


def _rejected_report(path: Path, thresholds: ModelQualityThresholds, error: ModelQualityError) -> dict[str, Any]:
    return {
        "schema_version": REPORT_SCHEMA_VERSION,
        "gate_version": GATE_VERSION,
        "status": "reject",
        "file_name": path.name,
        "errors": [error.code],
        "warnings": [],
        "messages": {error.code: str(error)},
        "thresholds": asdict(thresholds),
        "metrics": {},
    }


def _parse_obj(path: Path, max_faces: int) -> tuple[list[tuple[float, float, float]], list[tuple[int, int, int]]]:
    vertices: list[tuple[float, float, float]] = []
    faces: list[tuple[int, int, int]] = []
    try:
        with path.open("r", encoding="utf-8", errors="strict") as stream:
            for line in stream:
                fields = line.strip().split()
                if not fields or fields[0].startswith("#"):
                    continue
                keyword = fields[0].lower()
                if keyword == "v":
                    if len(fields) < 4:
                        raise ModelQualityError("invalid_vertex", "OBJ contains a vertex with fewer than three coordinates.")
                    try:
                        vertex = tuple(float(value) for value in fields[1:4])
                    except ValueError:
                        raise ModelQualityError("invalid_vertex", "OBJ contains a non-numeric vertex coordinate.") from None
                    if not all(math.isfinite(value) for value in vertex):
                        raise ModelQualityError("invalid_vertex", "OBJ contains a non-finite vertex coordinate.")
                    vertices.append(vertex)
                elif keyword == "f":
                    if len(fields) != 4:
                        raise ModelQualityError("non_triangular_face", "Printable OBJ must contain only triangular faces.")
                    face = tuple(_resolve_obj_index(token, len(vertices)) for token in fields[1:])
                    faces.append(face)
                    if len(faces) > max_faces:
                        raise ModelQualityError("too_many_faces", f"OBJ exceeds the {max_faces}-triangle limit.")
    except UnicodeDecodeError:
        raise ModelQualityError("invalid_encoding", "OBJ is not valid UTF-8 text.") from None
    except OSError:
        raise ModelQualityError("unreadable_file", "OBJ could not be read.") from None
    if not vertices or not faces:
        raise ModelQualityError("missing_geometry", "OBJ does not contain usable vertices and triangular faces.")
    return vertices, faces


def _vector_length(x: float, y: float, z: float) -> float:
    return math.sqrt(x * x + y * y + z * z)


def analyze_printable_obj(
    path: Path | str,
    thresholds: ModelQualityThresholds | None = None,
    *,
    allow_repairable_topology: bool = False,
) -> dict[str, Any]:
    """Analyze a normalized Z-up OBJ and return a stable quality report."""

    source = Path(path)
    limits = thresholds or ModelQualityThresholds()
    try:
        vertices, faces = _parse_obj(source, limits.max_faces)
    except ModelQualityError as error:
        return _rejected_report(source, limits, error)

    parent = list(range(len(vertices)))

    def find(index: int) -> int:
        while parent[index] != index:
            parent[index] = parent[parent[index]]
            index = parent[index]
        return index

    def unite(left: int, right: int) -> None:
        left_root, right_root = find(left), find(right)
        if left_root != right_root:
            parent[right_root] = left_root

    edge_uses: dict[tuple[int, int], list[tuple[int, int]]] = {}
    referenced: set[int] = set()
    degenerate_faces = 0
    surface_area = 0.0
    downward_area = 0.0
    short_edges = 0
    edge_samples = 0
    minimum_edge = math.inf
    maximum_edge = 0.0

    for face in faces:
        referenced.update(face)
        unite(face[0], face[1])
        unite(face[1], face[2])
        a, b, c = (vertices[index] for index in face)
        ab = (b[0] - a[0], b[1] - a[1], b[2] - a[2])
        ac = (c[0] - a[0], c[1] - a[1], c[2] - a[2])
        cross = (
            ab[1] * ac[2] - ab[2] * ac[1],
            ab[2] * ac[0] - ab[0] * ac[2],
            ab[0] * ac[1] - ab[1] * ac[0],
        )
        double_area = _vector_length(*cross)
        area = double_area * 0.5
        if len(set(face)) != 3 or area <= limits.degenerate_area_epsilon_mm2:
            degenerate_faces += 1
        else:
            surface_area += area
            if cross[2] / double_area < -math.sqrt(0.5):
                downward_area += area

        for left, right in ((face[0], face[1]), (face[1], face[2]), (face[2], face[0])):
            edge = (left, right) if left < right else (right, left)
            edge_uses.setdefault(edge, []).append((left, right))
            start, end = vertices[left], vertices[right]
            length = _vector_length(end[0] - start[0], end[1] - start[1], end[2] - start[2])
            minimum_edge = min(minimum_edge, length)
            maximum_edge = max(maximum_edge, length)
            short_edges += length < limits.short_edge_mm
            edge_samples += 1

    boundary_edges = sum(len(uses) == 1 for uses in edge_uses.values())
    non_manifold_edges = sum(len(uses) > 2 for uses in edge_uses.values())
    inconsistent_winding_edges = sum(len(uses) == 2 and uses[0] == uses[1] for uses in edge_uses.values())

    roots = {index: find(index) for index in referenced}
    component_faces = Counter(roots[face[0]] for face in faces)
    component_bounds: dict[int, list[list[float]]] = {}
    for index in referenced:
        root = roots[index]
        point = vertices[index]
        bounds = component_bounds.setdefault(root, [[math.inf] * 3, [-math.inf] * 3])
        for axis in range(3):
            bounds[0][axis] = min(bounds[0][axis], point[axis])
            bounds[1][axis] = max(bounds[1][axis], point[axis])

    minimum = [min(vertex[axis] for vertex in vertices) for axis in range(3)]
    maximum = [max(vertex[axis] for vertex in vertices) for axis in range(3)]
    dimensions = [maximum[axis] - minimum[axis] for axis in range(3)]
    diagonal = _vector_length(*dimensions)
    face_count = len(faces)
    largest_component_faces = max(component_faces.values(), default=0)
    tiny_components = 0
    for root, count in component_faces.items():
        bounds = component_bounds[root]
        component_size = [bounds[1][axis] - bounds[0][axis] for axis in range(3)]
        component_diagonal = _vector_length(*component_size)
        if count / face_count <= limits.tiny_component_face_ratio and (
            diagonal <= 0.0 or component_diagonal / diagonal <= limits.tiny_component_diagonal_ratio
        ):
            tiny_components += 1

    contact_indices = [index for index in referenced if vertices[index][2] <= minimum[2] + limits.ground_band_mm]
    if contact_indices:
        contact_x = max(vertices[index][0] for index in contact_indices) - min(vertices[index][0] for index in contact_indices)
        contact_y = max(vertices[index][1] for index in contact_indices) - min(vertices[index][1] for index in contact_indices)
    else:
        contact_x = contact_y = 0.0
    x_ratio = contact_x / dimensions[0] if dimensions[0] > 0.0 else 0.0
    y_ratio = contact_y / dimensions[1] if dimensions[1] > 0.0 else 0.0
    contact_span_ratio = x_ratio * y_ratio

    positive_dimensions = [value for value in dimensions if value > 1e-9]
    aspect_ratio = max(positive_dimensions) / min(positive_dimensions) if len(positive_dimensions) == 3 else math.inf
    downward_ratio = downward_area / surface_area if surface_area > 0.0 else 0.0
    short_edge_ratio = short_edges / edge_samples if edge_samples else 0.0

    errors: list[str] = []
    warnings: list[str] = []
    messages: dict[str, str] = {}

    def add_error(code: str, message: str) -> None:
        errors.append(code)
        messages[code] = message

    def add_warning(code: str, message: str) -> None:
        warnings.append(code)
        messages[code] = message

    if degenerate_faces:
        add_error("degenerate_faces", f"Model contains {degenerate_faces} zero-area or repeated-index triangles.")
    invalid_edges = boundary_edges + non_manifold_edges + inconsistent_winding_edges
    repairable_edge_limit = max(64, face_count // 100)
    repairable_topology = bool(invalid_edges) and allow_repairable_topology and invalid_edges <= repairable_edge_limit
    if boundary_edges:
        (add_warning if repairable_topology else add_error)(
            "repairable_boundary_edges" if repairable_topology else "boundary_edges",
            f"Model contains {boundary_edges} open boundary edges.",
        )
    if non_manifold_edges:
        (add_warning if repairable_topology else add_error)(
            "repairable_non_manifold_edges" if repairable_topology else "non_manifold_edges",
            f"Model contains {non_manifold_edges} edges shared by more than two faces.",
        )
    if inconsistent_winding_edges:
        (add_warning if repairable_topology else add_error)(
            "repairable_inconsistent_winding_edges" if repairable_topology else "inconsistent_winding_edges",
            f"Model contains {inconsistent_winding_edges} same-direction shared edges.",
        )
    if len(positive_dimensions) != 3:
        add_error("flat_or_empty_axis", "Model has no measurable extent on at least one axis.")
    if tiny_components:
        add_warning("tiny_detached_components", f"Model contains {tiny_components} very small connected components.")
    if len(contact_indices) < 3 or contact_span_ratio < limits.min_contact_span_ratio:
        add_warning("weak_bed_contact", "The lowest 0.5 mm of the model has a very small bed-contact footprint.")
    if math.isfinite(aspect_ratio) and aspect_ratio > limits.max_aspect_ratio:
        add_warning("extreme_aspect_ratio", "Model has an extreme bounding-box aspect ratio.")
    if downward_ratio > limits.max_downward_area_ratio:
        add_warning("high_downward_surface_ratio", "A large share of surface area faces downward and may require support.")
    if short_edge_ratio > limits.max_short_edge_ratio:
        add_warning("dense_micro_triangles", "More than half of sampled triangle edges are shorter than 0.15 mm.")

    status = "reject" if errors else "review" if warnings else "pass"
    metrics = {
        "vertex_count": len(vertices),
        "referenced_vertex_count": len(referenced),
        "unreferenced_vertex_count": len(vertices) - len(referenced),
        "face_count": face_count,
        "component_count": len(component_faces),
        "largest_component_face_ratio": round(largest_component_faces / face_count, 6),
        "tiny_component_count": tiny_components,
        "boundary_edges": boundary_edges,
        "non_manifold_edges": non_manifold_edges,
        "inconsistent_winding_edges": inconsistent_winding_edges,
        "degenerate_faces": degenerate_faces,
        "repairable_topology": repairable_topology,
        "repairable_edge_limit": repairable_edge_limit,
        "dimensions_mm": {"x": round(dimensions[0], 6), "y": round(dimensions[1], 6), "z": round(dimensions[2], 6)},
        "bounding_box_mm": {
            "minimum": [round(value, 6) for value in minimum],
            "maximum": [round(value, 6) for value in maximum],
        },
        "diagonal_mm": round(diagonal, 6),
        "aspect_ratio": round(aspect_ratio, 6) if math.isfinite(aspect_ratio) else None,
        "surface_area_mm2": round(surface_area, 6),
        "contact_vertex_count": len(contact_indices),
        "contact_span_ratio": round(contact_span_ratio, 6),
        "downward_surface_ratio": round(downward_ratio, 6),
        "short_edge_ratio": round(short_edge_ratio, 6),
        "minimum_edge_mm": round(minimum_edge, 6) if math.isfinite(minimum_edge) else None,
        "maximum_edge_mm": round(maximum_edge, 6),
    }
    return {
        "schema_version": REPORT_SCHEMA_VERSION,
        "gate_version": GATE_VERSION,
        "status": status,
        "file_name": source.name,
        "errors": errors,
        "warnings": warnings,
        "messages": messages,
        "thresholds": asdict(limits),
        "metrics": metrics,
    }


def write_model_quality_report(report: dict[str, Any], destination: Path | str) -> Path:
    path = Path(destination)
    temporary = path.with_name(path.name + ".part")
    try:
        temporary.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
        os.replace(temporary, path)
    except OSError:
        try:
            temporary.unlink(missing_ok=True)
        except OSError:
            pass
        raise ModelQualityError("report_write_failed", "Model quality report could not be saved.") from None
    return path
