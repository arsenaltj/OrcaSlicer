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
GATE_VERSION = "structural-v4"


@dataclass(frozen=True)
class ModelQualityThresholds:
    max_faces: int = 1_000_000
    ground_band_mm: float = 0.5
    min_contact_span_ratio: float = 0.005
    min_contact_area_ratio: float = 0.002
    max_aspect_ratio: float = 20.0
    max_downward_area_ratio: float = 0.35
    min_overhang_region_area_mm2: float = 4.0
    min_overhang_region_area_ratio: float = 0.0005
    short_edge_mm: float = 0.15
    max_short_edge_ratio: float = 0.50
    tiny_component_face_ratio: float = 0.001
    tiny_component_diagonal_ratio: float = 0.05
    component_contact_tolerance_mm: float = 0.2
    tiny_color_region_face_ratio: float = 0.0005
    tiny_color_region_area_ratio: float = 0.0001
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


def _parse_obj(
    path: Path, max_faces: int
) -> tuple[
    list[tuple[float, float, float]],
    list[tuple[float, float, float] | None],
    list[tuple[int, int, int]],
]:
    vertices: list[tuple[float, float, float]] = []
    vertex_colors: list[tuple[float, float, float] | None] = []
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
                    color = None
                    if len(fields) >= 7:
                        try:
                            parsed_color = tuple(float(value) for value in fields[4:7])
                        except ValueError:
                            parsed_color = ()
                        if len(parsed_color) == 3 and all(math.isfinite(value) for value in parsed_color):
                            color = parsed_color
                    vertex_colors.append(color)
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
    return vertices, vertex_colors, faces


def _printable_color_key(color: tuple[float, float, float] | None) -> tuple[int, int, int] | None:
    if color is None:
        return None
    if all(0.0 <= value <= 1.0 for value in color):
        return tuple(round(value * 255.0) for value in color)
    if all(0.0 <= value <= 255.0 for value in color):
        return tuple(round(value) for value in color)
    return None


def _vector_length(x: float, y: float, z: float) -> float:
    return math.sqrt(x * x + y * y + z * z)


def _bounds_distance(left: list[list[float]], right: list[list[float]]) -> float:
    gaps = [
        max(0.0, left[0][axis] - right[1][axis], right[0][axis] - left[1][axis])
        for axis in range(3)
    ]
    return _vector_length(*gaps)


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
        vertices, vertex_colors, faces = _parse_obj(source, limits.max_faces)
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

    edge_uses: dict[tuple[int, int], list[tuple[int, int, int]]] = {}
    referenced: set[int] = set()
    face_areas: list[float] = []
    face_projected_areas: list[float] = []
    face_downward: list[bool] = []
    face_minimum_z: list[float] = []
    face_maximum_z: list[float] = []
    degenerate_faces = 0
    surface_area = 0.0
    downward_area = 0.0
    short_edges = 0
    edge_samples = 0
    minimum_edge = math.inf
    maximum_edge = 0.0

    for face_index, face in enumerate(faces):
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
        is_downward = False
        if len(set(face)) != 3 or area <= limits.degenerate_area_epsilon_mm2:
            degenerate_faces += 1
            face_areas.append(0.0)
            face_projected_areas.append(0.0)
        else:
            surface_area += area
            face_areas.append(area)
            face_projected_areas.append(abs(cross[2]) * 0.5)
            is_downward = cross[2] / double_area < -math.sqrt(0.5)
            if is_downward:
                downward_area += area
        face_downward.append(is_downward)
        face_minimum_z.append(min(a[2], b[2], c[2]))
        face_maximum_z.append(max(a[2], b[2], c[2]))

        for left, right in ((face[0], face[1]), (face[1], face[2]), (face[2], face[0])):
            edge = (left, right) if left < right else (right, left)
            edge_uses.setdefault(edge, []).append((face_index, left, right))
            start, end = vertices[left], vertices[right]
            length = _vector_length(end[0] - start[0], end[1] - start[1], end[2] - start[2])
            minimum_edge = min(minimum_edge, length)
            maximum_edge = max(maximum_edge, length)
            short_edges += length < limits.short_edge_mm
            edge_samples += 1

    boundary_edges = sum(len(uses) == 1 for uses in edge_uses.values())
    non_manifold_edges = sum(len(uses) > 2 for uses in edge_uses.values())
    inconsistent_winding_edges = sum(
        len(uses) == 2 and uses[0][1:] == uses[1][1:] for uses in edge_uses.values()
    )

    face_neighbors: list[set[int]] = [set() for _ in faces]
    for uses in edge_uses.values():
        if len(uses) != 2:
            continue
        left_face, right_face = uses[0][0], uses[1][0]
        face_neighbors[left_face].add(right_face)
        face_neighbors[right_face].add(left_face)

    geometric_boundary_faces: dict[
        tuple[tuple[float, float, float], tuple[float, float, float]], list[int]
    ] = {}
    for edge, uses in edge_uses.items():
        if len(uses) != 1:
            continue
        endpoints = (vertices[edge[0]], vertices[edge[1]])
        geometric_edge = endpoints if endpoints[0] < endpoints[1] else (endpoints[1], endpoints[0])
        geometric_boundary_faces.setdefault(geometric_edge, []).append(uses[0][0])
    for joined_faces in geometric_boundary_faces.values():
        if len(joined_faces) != 2 or joined_faces[0] == joined_faces[1]:
            continue
        face_neighbors[joined_faces[0]].add(joined_faces[1])
        face_neighbors[joined_faces[1]].add(joined_faces[0])

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

    supported_bounds = [[math.inf] * 3, [-math.inf] * 3]
    has_supported_bounds = False
    floating_components = 0
    minimum_floating_clearance: float | None = None

    def extend_supported(bounds: list[list[float]]) -> None:
        nonlocal has_supported_bounds
        for axis in range(3):
            supported_bounds[0][axis] = min(supported_bounds[0][axis], bounds[0][axis])
            supported_bounds[1][axis] = max(supported_bounds[1][axis], bounds[1][axis])
        has_supported_bounds = True

    contact_tolerance = max(0.0, limits.component_contact_tolerance_mm)
    ground_limit = minimum[2] + limits.ground_band_mm
    for _, bounds in sorted(component_bounds.items(), key=lambda item: (item[1][0][2], item[0])):
        if bounds[0][2] <= ground_limit:
            extend_supported(bounds)
            continue
        clearance = _bounds_distance(bounds, supported_bounds) if has_supported_bounds else math.inf
        if clearance <= contact_tolerance:
            extend_supported(bounds)
            continue
        floating_components += 1
        minimum_floating_clearance = (
            clearance
            if minimum_floating_clearance is None
            else min(minimum_floating_clearance, clearance)
        )

    contact_indices = [index for index in referenced if vertices[index][2] <= minimum[2] + limits.ground_band_mm]
    if contact_indices:
        contact_x = max(vertices[index][0] for index in contact_indices) - min(vertices[index][0] for index in contact_indices)
        contact_y = max(vertices[index][1] for index in contact_indices) - min(vertices[index][1] for index in contact_indices)
    else:
        contact_x = contact_y = 0.0
    x_ratio = contact_x / dimensions[0] if dimensions[0] > 0.0 else 0.0
    y_ratio = contact_y / dimensions[1] if dimensions[1] > 0.0 else 0.0
    contact_span_ratio = x_ratio * y_ratio
    bed_contact_area = sum(
        face_projected_areas[index]
        for index in range(len(faces))
        if face_downward[index] and face_maximum_z[index] <= ground_limit
    )
    footprint_area = dimensions[0] * dimensions[1]
    bed_contact_area_ratio = min(1.0, bed_contact_area / footprint_area) if footprint_area > 0.0 else 0.0

    overhang_candidates = {
        index
        for index in range(len(faces))
        if face_downward[index] and face_minimum_z[index] > ground_limit
    }
    overhang_region_count = 0
    significant_overhang_regions = 0
    largest_overhang_region_area_ratio = 0.0
    lowest_overhang_clearance: float | None = None
    visited_overhangs: set[int] = set()
    for seed in sorted(overhang_candidates):
        if seed in visited_overhangs:
            continue
        visited_overhangs.add(seed)
        pending = [seed]
        region_area = 0.0
        region_minimum_z = math.inf
        while pending:
            current = pending.pop()
            region_area += face_areas[current]
            region_minimum_z = min(region_minimum_z, face_minimum_z[current])
            for neighbor in face_neighbors[current]:
                if neighbor in overhang_candidates and neighbor not in visited_overhangs:
                    visited_overhangs.add(neighbor)
                    pending.append(neighbor)
        overhang_region_count += 1
        area_ratio = region_area / surface_area if surface_area > 0.0 else 0.0
        largest_overhang_region_area_ratio = max(largest_overhang_region_area_ratio, area_ratio)
        if (
            region_area >= limits.min_overhang_region_area_mm2
            and area_ratio >= limits.min_overhang_region_area_ratio
        ):
            significant_overhang_regions += 1
            clearance = max(0.0, region_minimum_z - minimum[2])
            lowest_overhang_clearance = (
                clearance
                if lowest_overhang_clearance is None
                else min(lowest_overhang_clearance, clearance)
            )

    positive_dimensions = [value for value in dimensions if value > 1e-9]
    aspect_ratio = max(positive_dimensions) / min(positive_dimensions) if len(positive_dimensions) == 3 else math.inf
    downward_ratio = downward_area / surface_area if surface_area > 0.0 else 0.0
    short_edge_ratio = short_edges / edge_samples if edge_samples else 0.0

    normalized_vertex_colors = [_printable_color_key(color) for color in vertex_colors]
    has_complete_vertex_colors = bool(normalized_vertex_colors) and all(
        color is not None for color in normalized_vertex_colors
    )
    printable_colors = (
        {normalized_vertex_colors[index] for index in referenced}
        if has_complete_vertex_colors
        else set()
    )
    face_colors: list[tuple[int, int, int] | None] = []
    if has_complete_vertex_colors:
        for face in faces:
            counts = Counter(normalized_vertex_colors[index] for index in face)
            maximum_count = max(counts.values())
            candidates = [color for color, count in counts.items() if count == maximum_count]
            face_colors.append(min(candidates) if maximum_count >= 2 else None)

    color_region_count = 0
    tiny_color_regions = 0
    smallest_color_region_face_ratio: float | None = None
    smallest_color_region_area_ratio: float | None = None
    if has_complete_vertex_colors:
        visited = [False] * len(faces)
        for seed, color in enumerate(face_colors):
            if visited[seed] or color is None:
                continue
            visited[seed] = True
            pending = [seed]
            region_faces = 0
            region_area = 0.0
            while pending:
                current = pending.pop()
                region_faces += 1
                region_area += face_areas[current]
                for neighbor in face_neighbors[current]:
                    if not visited[neighbor] and face_colors[neighbor] == color:
                        visited[neighbor] = True
                        pending.append(neighbor)
            face_ratio = region_faces / len(faces)
            area_ratio = region_area / surface_area if surface_area > 0.0 else 0.0
            color_region_count += 1
            smallest_color_region_face_ratio = (
                face_ratio
                if smallest_color_region_face_ratio is None
                else min(smallest_color_region_face_ratio, face_ratio)
            )
            smallest_color_region_area_ratio = (
                area_ratio
                if smallest_color_region_area_ratio is None
                else min(smallest_color_region_area_ratio, area_ratio)
            )
            if (
                len(printable_colors) > 1
                and region_area > limits.degenerate_area_epsilon_mm2
                and face_ratio <= limits.tiny_color_region_face_ratio
                and area_ratio <= limits.tiny_color_region_area_ratio
            ):
                tiny_color_regions += 1

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
    if floating_components:
        add_warning(
            "floating_disconnected_components",
            f"Model contains {floating_components} disconnected components with no detected bed or model contact.",
        )
    if (
        len(contact_indices) < 3
        or contact_span_ratio < limits.min_contact_span_ratio
        or bed_contact_area_ratio < limits.min_contact_area_ratio
    ):
        add_warning("weak_bed_contact", "The lowest 0.5 mm of the model has a very small bed-contact footprint.")
    if math.isfinite(aspect_ratio) and aspect_ratio > limits.max_aspect_ratio:
        add_warning("extreme_aspect_ratio", "Model has an extreme bounding-box aspect ratio.")
    if downward_ratio > limits.max_downward_area_ratio:
        add_warning("high_downward_surface_ratio", "A large share of surface area faces downward and may require support.")
    elif significant_overhang_regions:
        add_warning(
            "localized_overhang_regions",
            f"Model contains {significant_overhang_regions} elevated downward-facing regions that may require support.",
        )
    if short_edge_ratio > limits.max_short_edge_ratio:
        add_warning("dense_micro_triangles", "More than half of sampled triangle edges are shorter than 0.15 mm.")
    if tiny_color_regions:
        add_warning(
            "tiny_printable_color_regions",
            f"Model contains {tiny_color_regions} very small printable-color regions that may create noisy filament changes.",
        )

    status = "reject" if errors else "review" if warnings else "pass"
    metrics = {
        "vertex_count": len(vertices),
        "referenced_vertex_count": len(referenced),
        "unreferenced_vertex_count": len(vertices) - len(referenced),
        "face_count": face_count,
        "component_count": len(component_faces),
        "largest_component_face_ratio": round(largest_component_faces / face_count, 6),
        "tiny_component_count": tiny_components,
        "floating_component_count": floating_components,
        "minimum_floating_clearance_mm": (
            round(minimum_floating_clearance, 6)
            if minimum_floating_clearance is not None and math.isfinite(minimum_floating_clearance)
            else None
        ),
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
        "bed_contact_area_mm2": round(bed_contact_area, 6),
        "bed_contact_area_ratio": round(bed_contact_area_ratio, 6),
        "downward_surface_ratio": round(downward_ratio, 6),
        "overhang_region_count": overhang_region_count,
        "significant_overhang_region_count": significant_overhang_regions,
        "largest_overhang_region_area_ratio": round(largest_overhang_region_area_ratio, 6),
        "lowest_overhang_clearance_mm": (
            round(lowest_overhang_clearance, 6)
            if lowest_overhang_clearance is not None and math.isfinite(lowest_overhang_clearance)
            else None
        ),
        "short_edge_ratio": round(short_edge_ratio, 6),
        "minimum_edge_mm": round(minimum_edge, 6) if math.isfinite(minimum_edge) else None,
        "maximum_edge_mm": round(maximum_edge, 6),
        "has_complete_vertex_colors": has_complete_vertex_colors,
        "printable_color_count": len(printable_colors),
        "color_region_count": color_region_count,
        "tiny_color_region_count": tiny_color_regions,
        "smallest_color_region_face_ratio": (
            round(smallest_color_region_face_ratio, 6)
            if smallest_color_region_face_ratio is not None
            else None
        ),
        "smallest_color_region_area_ratio": (
            round(smallest_color_region_area_ratio, 6)
            if smallest_color_region_area_ratio is not None
            else None
        ),
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
