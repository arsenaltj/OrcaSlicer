"""Local vertex-color region cleanup; explicit input and report files."""
from __future__ import annotations

import json
import math
import os
from collections import Counter
from model_contracts import (
    MAX_COLOR_BOUNDARY_PASSES,
    MAX_COLOR_BOUNDARY_SOURCE_AREA_RATIO,
    MAX_COLOR_BOUNDARY_SOURCE_NEIGHBORS,
    MAX_COLOR_BOUNDARY_SURFACE_AREA_RATIO,
    MAX_COLOR_CLEANUP_PASSES,
    MAX_COLOR_CLEANUP_SOURCE_AREA_RATIO,
    MAX_COLOR_CLEANUP_SURFACE_AREA_RATIO,
    MAX_TINY_COLOR_COMPONENT_AREA_RATIO,
    MAX_TINY_COLOR_COMPONENT_VERTEX_RATIO,
    MEANINGFUL_COLOR_SURFACE_AREA_RATIO,
    MIN_COLOR_BOUNDARY_SUPPORT_RATIO,
)
from model_obj_io import _obj_triangle_area, _resolve_obj_index, _write_mesh_repair_report
from pathlib import Path
from tripo_client import TripoError
from typing import Any

def _color_boundary_metrics(
    colors: list[tuple[int, int, int]],
    faces: list[tuple[int, int, int]],
    face_areas: list[float],
) -> dict[str, Any]:
    mixed = [
        (face, area) for face, area in zip(faces, face_areas)
        if len({colors[index] for index in face}) > 1
    ]
    surface_area = sum(face_areas)
    mixed_area = sum(area for _face, area in mixed)
    return {
        "mixed_face_count": len(mixed),
        "mixed_face_ratio": round(len(mixed) / len(faces), 6) if faces else 0.0,
        "mixed_face_surface_area_mm2": round(mixed_area, 6),
        "mixed_face_surface_area_ratio": round(mixed_area / surface_area, 6) if surface_area > 0.0 else 0.0,
    }

def _regularize_obj_color_boundaries(path: Path, report_path: Path) -> dict[str, Any]:
    positions: list[tuple[float, float, float]] = []
    colors: list[tuple[int, int, int]] = []
    faces: list[tuple[int, int, int]] = []
    try:
        with path.open("r", encoding="utf-8", errors="strict") as stream:
            for line in stream:
                fields = line.strip().split()
                if not fields or fields[0].startswith("#"):
                    continue
                keyword = fields[0].lower()
                if keyword == "v":
                    if len(fields) not in {7, 8}:
                        raise TripoError("The generated OBJ does not provide valid vertex colors.")
                    position = tuple(float(value) for value in fields[1:4])
                    color = tuple(round(float(value) * 255) for value in fields[4:7])
                    if not all(math.isfinite(value) for value in position) or not all(
                        0 <= value <= 255 for value in color
                    ):
                        raise TripoError("The generated OBJ has an invalid colored vertex.")
                    positions.append(position)
                    colors.append(color)
                elif keyword == "f":
                    if len(fields) != 4:
                        raise TripoError("The generated OBJ must contain only triangular faces.")
                    faces.append(tuple(_resolve_obj_index(value, len(positions), "vertex") for value in fields[1:]))
    except (OSError, UnicodeDecodeError, ValueError):
        raise TripoError("The generated OBJ could not be read for color-boundary cleanup.") from None
    if not positions or not faces:
        raise TripoError("The generated OBJ does not contain usable colored geometry.")

    face_areas = [_obj_triangle_area(positions, face) for face in faces]
    surface_area = sum(face_areas)
    if not math.isfinite(surface_area) or surface_area <= 0.0:
        raise TripoError("The generated OBJ has invalid surface area for color-boundary cleanup.")

    incident_faces: list[list[int]] = [[] for _ in positions]
    vertex_surface_area = [0.0] * len(positions)
    edge_lengths: dict[tuple[int, int], float] = {}
    for face_index, (face, area) in enumerate(zip(faces, face_areas)):
        for vertex in face:
            incident_faces[vertex].append(face_index)
            vertex_surface_area[vertex] += area / 3.0
        for left, right in ((face[0], face[1]), (face[1], face[2]), (face[2], face[0])):
            edge = (left, right) if left < right else (right, left)
            if edge in edge_lengths:
                continue
            length = math.sqrt(sum((positions[left][axis] - positions[right][axis]) ** 2 for axis in range(3)))
            if length > 0.0 and math.isfinite(length):
                edge_lengths[edge] = length
    neighbors: list[list[tuple[int, float]]] = [[] for _ in positions]
    for (left, right), length in edge_lengths.items():
        neighbors[left].append((right, length))
        neighbors[right].append((left, length))

    original_color_area: Counter[tuple[int, int, int]] = Counter()
    for color, area in zip(colors, vertex_surface_area):
        original_color_area[color] += area
    current_color_area = original_color_area.copy()
    before = _color_boundary_metrics(colors, faces, face_areas)
    global_budget = surface_area * MAX_COLOR_BOUNDARY_SURFACE_AREA_RATIO
    changed_area_by_color: Counter[tuple[int, int, int]] = Counter()
    changed_surface_area = 0.0
    recolored_vertices = 0
    protected_meaningful_candidates = 0
    budget_limited_candidates = 0
    pass_reports: list[dict[str, Any]] = []

    def is_mixed(face_index: int, override_vertex: int = -1, override_color: tuple[int, int, int] | None = None) -> bool:
        face_colors = {
            override_color if vertex == override_vertex else colors[vertex]
            for vertex in faces[face_index]
        }
        return len(face_colors) > 1

    def evaluate(vertex: int) -> tuple[float, float, tuple[int, int, int]] | None:
        source = colors[vertex]
        source_neighbors = sum(1 for neighbor, _length in neighbors[vertex] if colors[neighbor] == source)
        if source_neighbors > MAX_COLOR_BOUNDARY_SOURCE_NEIGHBORS:
            return None
        candidates = {colors[neighbor] for neighbor, _length in neighbors[vertex] if colors[neighbor] != source}
        if not candidates:
            return None
        before_area = sum(face_areas[index] for index in incident_faces[vertex] if is_mixed(index))
        source_support = sum(length for neighbor, length in neighbors[vertex] if colors[neighbor] == source)
        best: tuple[float, float, tuple[int, int, int]] | None = None
        for target in candidates:
            target_support = sum(length for neighbor, length in neighbors[vertex] if colors[neighbor] == target)
            if target_support <= source_support * MIN_COLOR_BOUNDARY_SUPPORT_RATIO + 1e-12:
                continue
            before_color_counts = [len({colors[index] for index in faces[face_index]}) for face_index in incident_faces[vertex]]
            after_color_counts = [
                len({target if index == vertex else colors[index] for index in faces[face_index]})
                for face_index in incident_faces[vertex]
            ]
            if any(after > before for before, after in zip(before_color_counts, after_color_counts)):
                continue
            after_area = sum(
                face_areas[index] for index, color_count in zip(incident_faces[vertex], after_color_counts)
                if color_count > 1
            )
            improvement = before_area - after_area
            if improvement <= 1e-12:
                continue
            candidate = (improvement, target_support, target)
            if best is None or (-candidate[0], -candidate[1], candidate[2]) < (-best[0], -best[1], best[2]):
                best = candidate
        return best

    for pass_index in range(MAX_COLOR_BOUNDARY_PASSES):
        candidates = []
        for vertex in range(len(positions)):
            evaluated = evaluate(vertex)
            if evaluated is not None:
                improvement, support, target = evaluated
                candidates.append((-improvement, -support, vertex, target))
        candidates.sort()
        changed_this_pass = 0
        improved_area = 0.0
        for _negative_improvement, _negative_support, vertex, _target in candidates:
            evaluated = evaluate(vertex)
            if evaluated is None:
                continue
            improvement, _support, target = evaluated
            source = colors[vertex]
            contribution = vertex_surface_area[vertex]
            meaningful_floor = surface_area * MEANINGFUL_COLOR_SURFACE_AREA_RATIO
            if (
                original_color_area[source] >= meaningful_floor
                and current_color_area[source] - contribution < meaningful_floor - 1e-12
            ):
                protected_meaningful_candidates += 1
                continue
            source_budget = min(
                original_color_area[source],
                original_color_area[source] * MAX_COLOR_BOUNDARY_SOURCE_AREA_RATIO
                if original_color_area[source] >= meaningful_floor else original_color_area[source],
            )
            if (
                changed_surface_area + contribution > global_budget + 1e-12
                or changed_area_by_color[source] + contribution > source_budget + 1e-12
            ):
                budget_limited_candidates += 1
                continue
            colors[vertex] = target
            current_color_area[source] -= contribution
            current_color_area[target] += contribution
            changed_area_by_color[source] += contribution
            changed_surface_area += contribution
            recolored_vertices += 1
            changed_this_pass += 1
            improved_area += improvement
        pass_reports.append({
            "pass": pass_index + 1,
            "candidate_vertices": len(candidates),
            "recolored_vertices": changed_this_pass,
            "mixed_surface_area_improvement_mm2": round(improved_area, 6),
        })
        if not changed_this_pass:
            break

    if recolored_vertices:
        temporary = path.with_name(path.name + ".boundaries")
        vertex_index = 0
        try:
            with path.open("r", encoding="utf-8", errors="strict") as source, temporary.open(
                "w", encoding="ascii", newline="\n"
            ) as output:
                for line in source:
                    fields = line.strip().split()
                    if fields and fields[0].lower() == "v":
                        red, green, blue = colors[vertex_index]
                        fields[4:7] = [f"{channel / 255.0:.6f}" for channel in (red, green, blue)]
                        output.write(" ".join(fields) + "\n")
                        vertex_index += 1
                    else:
                        output.write(line if line.endswith("\n") else line + "\n")
            os.replace(temporary, path)
        except (OSError, UnicodeDecodeError):
            raise TripoError("Printable color boundaries could not be regularized safely.") from None
        finally:
            try:
                temporary.unlink(missing_ok=True)
            except OSError:
                pass

    after = _color_boundary_metrics(colors, faces, face_areas)
    report = {
        "status": "regularized" if recolored_vertices else "not_needed",
        "passes": pass_reports,
        "recolored_vertices": recolored_vertices,
        "changed_surface_area_mm2": round(changed_surface_area, 6),
        "changed_surface_area_ratio": round(changed_surface_area / surface_area, 6),
        "maximum_surface_area_ratio": MAX_COLOR_BOUNDARY_SURFACE_AREA_RATIO,
        "maximum_source_area_ratio": MAX_COLOR_BOUNDARY_SOURCE_AREA_RATIO,
        "minimum_neighbor_support_ratio": MIN_COLOR_BOUNDARY_SUPPORT_RATIO,
        "maximum_source_same_color_neighbors": MAX_COLOR_BOUNDARY_SOURCE_NEIGHBORS,
        "meaningful_color_surface_area_ratio": MEANINGFUL_COLOR_SURFACE_AREA_RATIO,
        "protected_meaningful_candidates": protected_meaningful_candidates,
        "budget_limited_candidates": budget_limited_candidates,
        "before": before,
        "after": after,
        "changed_surface_area_by_color_mm2": {
            "#{:02X}{:02X}{:02X}".format(*color): round(area, 6)
            for color, area in sorted(changed_area_by_color.items())
        },
    }
    temporary_report = report_path.with_suffix(report_path.suffix + ".part")
    try:
        temporary_report.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
        os.replace(temporary_report, report_path)
    except OSError:
        raise TripoError("The color-boundary cleanup report could not be saved.") from None
    finally:
        try:
            temporary_report.unlink(missing_ok=True)
        except OSError:
            pass
    return report

def _consolidate_tiny_obj_color_components(path: Path, report_path: Path) -> dict[str, Any]:
    positions: list[tuple[float, float, float]] = []
    colors: list[tuple[int, int, int]] = []
    faces: list[tuple[int, int, int]] = []
    try:
        with path.open("r", encoding="utf-8", errors="strict") as stream:
            for line in stream:
                fields = line.strip().split()
                if not fields or fields[0].startswith("#"):
                    continue
                keyword = fields[0].lower()
                if keyword == "v":
                    if len(fields) not in {7, 8}:
                        raise TripoError("The generated OBJ does not provide valid vertex colors.")
                    try:
                        position = tuple(float(value) for value in fields[1:4])
                        color = tuple(round(float(value) * 255) for value in fields[4:7])
                    except ValueError:
                        raise TripoError("The generated OBJ has an invalid colored vertex.") from None
                    if not all(math.isfinite(value) for value in position) or not all(
                        0 <= value <= 255 for value in color
                    ):
                        raise TripoError("The generated OBJ has an invalid colored vertex.")
                    positions.append(position)
                    colors.append(color)
                elif keyword == "f":
                    if len(fields) != 4:
                        raise TripoError("The generated OBJ must contain only triangular faces.")
                    faces.append(tuple(_resolve_obj_index(value, len(positions), "vertex") for value in fields[1:]))
    except UnicodeDecodeError:
        raise TripoError("The generated OBJ is not valid UTF-8 text.") from None
    except OSError:
        raise TripoError("The generated OBJ could not be read for color cleanup.") from None
    if not positions or not faces:
        raise TripoError("The generated OBJ does not contain usable colored geometry.")

    def triangle_area(face: tuple[int, int, int]) -> float:
        left = positions[face[0]]
        right = positions[face[1]]
        third = positions[face[2]]
        ab = tuple(right[axis] - left[axis] for axis in range(3))
        ac = tuple(third[axis] - left[axis] for axis in range(3))
        cross = (
            ab[1] * ac[2] - ab[2] * ac[1],
            ab[2] * ac[0] - ab[0] * ac[2],
            ab[0] * ac[1] - ab[1] * ac[0],
        )
        return 0.5 * math.sqrt(sum(value * value for value in cross))

    face_areas = [triangle_area(face) for face in faces]
    surface_area = sum(face_areas)
    if not math.isfinite(surface_area) or surface_area <= 0.0:
        raise TripoError("The generated OBJ has invalid surface area for color cleanup.")
    original_usage = Counter(colors)
    total_merged_components = 0
    total_recolored_vertices = 0
    merged_area_by_color: Counter[tuple[int, int, int]] = Counter()
    pass_reports: list[dict[str, Any]] = []

    for pass_index in range(MAX_COLOR_CLEANUP_PASSES):
        parent = list(range(len(positions)))

        def find(index: int) -> int:
            while parent[index] != index:
                parent[index] = parent[parent[index]]
                index = parent[index]
            return index

        def unite(left: int, right: int) -> None:
            left_root, right_root = find(left), find(right)
            if left_root != right_root:
                parent[right_root] = left_root

        for face in faces:
            for left, right in ((face[0], face[1]), (face[1], face[2]), (face[2], face[0])):
                if colors[left] == colors[right]:
                    unite(left, right)

        component_area: Counter[int] = Counter()
        component_vertices: Counter[int] = Counter()
        color_area: Counter[tuple[int, int, int]] = Counter()
        for face, area in zip(faces, face_areas):
            contribution = area / 3.0
            for vertex in face:
                root = find(vertex)
                component_area[root] += contribution
                color_area[colors[vertex]] += contribution
        for index in range(len(positions)):
            component_vertices[find(index)] += 1

        boundary: dict[int, Counter[tuple[int, int, int]]] = {}
        for face in faces:
            for left, right in ((face[0], face[1]), (face[1], face[2]), (face[2], face[0])):
                left_root, right_root = find(left), find(right)
                if left_root == right_root or colors[left] == colors[right]:
                    continue
                edge_length = math.sqrt(sum(
                    (positions[left][axis] - positions[right][axis]) ** 2 for axis in range(3)
                ))
                if edge_length <= 0.0 or not math.isfinite(edge_length):
                    continue
                boundary.setdefault(left_root, Counter())[colors[right]] += edge_length
                boundary.setdefault(right_root, Counter())[colors[left]] += edge_length

        candidates: list[tuple[float, int, tuple[int, int, int]]] = []
        for root, area in component_area.items():
            neighbors = boundary.get(root)
            if not neighbors:
                continue
            if (
                area / surface_area <= MAX_TINY_COLOR_COMPONENT_AREA_RATIO
                and component_vertices[root] / len(positions) <= MAX_TINY_COLOR_COMPONENT_VERTEX_RATIO
            ):
                target = min(neighbors, key=lambda color: (-neighbors[color], color))
                candidates.append((area, root, target))
        candidates.sort(key=lambda item: (item[0], item[1], item[2]))

        merged_this_pass: dict[int, tuple[int, int, int]] = {}
        pass_area_by_color: Counter[tuple[int, int, int]] = Counter()
        for area, root, target in candidates:
            source = colors[root]
            if color_area[source] < surface_area * MEANINGFUL_COLOR_SURFACE_AREA_RATIO:
                budget = min(color_area[source], surface_area * MAX_COLOR_CLEANUP_SURFACE_AREA_RATIO)
            else:
                budget = min(
                    color_area[source] * MAX_COLOR_CLEANUP_SOURCE_AREA_RATIO,
                    surface_area * MAX_COLOR_CLEANUP_SURFACE_AREA_RATIO,
                    color_area[source] - surface_area * MEANINGFUL_COLOR_SURFACE_AREA_RATIO,
                )
            if pass_area_by_color[source] + area > budget + 1e-12:
                continue
            merged_this_pass[root] = target
            pass_area_by_color[source] += area

        recolored_this_pass = 0
        for index in range(len(colors)):
            target = merged_this_pass.get(find(index))
            if target is not None and colors[index] != target:
                colors[index] = target
                recolored_this_pass += 1
        pass_reports.append({
            "pass": pass_index + 1,
            "candidate_components": len(candidates),
            "merged_components": len(merged_this_pass),
            "recolored_vertices": recolored_this_pass,
            "merged_surface_area_mm2": round(sum(pass_area_by_color.values()), 6),
        })
        total_merged_components += len(merged_this_pass)
        total_recolored_vertices += recolored_this_pass
        merged_area_by_color.update(pass_area_by_color)
        if not recolored_this_pass:
            break

    if total_recolored_vertices:
        temporary = path.with_name(path.name + ".colors")
        vertex_index = 0
        try:
            with path.open("r", encoding="utf-8", errors="strict") as source, temporary.open(
                "w", encoding="ascii", newline="\n"
            ) as output:
                for line in source:
                    fields = line.strip().split()
                    if fields and fields[0].lower() == "v":
                        red, green, blue = colors[vertex_index]
                        fields[4:7] = [f"{channel / 255.0:.6f}" for channel in (red, green, blue)]
                        output.write(" ".join(fields) + "\n")
                        vertex_index += 1
                    else:
                        output.write(line if line.endswith("\n") else line + "\n")
            os.replace(temporary, path)
        except (OSError, UnicodeDecodeError):
            raise TripoError("Tiny printable-color regions could not be consolidated safely.") from None
        finally:
            try:
                temporary.unlink(missing_ok=True)
            except OSError:
                pass

    final_usage = Counter(colors)
    report = {
        "status": "consolidated" if total_recolored_vertices else "not_needed",
        "passes": pass_reports,
        "merged_components": total_merged_components,
        "recolored_vertices": total_recolored_vertices,
        "surface_area_mm2": round(surface_area, 6),
        "maximum_component_area_ratio": MAX_TINY_COLOR_COMPONENT_AREA_RATIO,
        "maximum_component_vertex_ratio": MAX_TINY_COLOR_COMPONENT_VERTEX_RATIO,
        "maximum_source_area_ratio": MAX_COLOR_CLEANUP_SOURCE_AREA_RATIO,
        "maximum_surface_area_ratio": MAX_COLOR_CLEANUP_SURFACE_AREA_RATIO,
        "original_vertex_color_usage": {
            "#{:02X}{:02X}{:02X}".format(*color): count for color, count in sorted(original_usage.items())
        },
        "final_vertex_color_usage": {
            "#{:02X}{:02X}{:02X}".format(*color): count for color, count in sorted(final_usage.items())
        },
        "merged_surface_area_by_color_mm2": {
            "#{:02X}{:02X}{:02X}".format(*color): round(area, 6)
            for color, area in sorted(merged_area_by_color.items())
        },
    }
    _write_mesh_repair_report(report_path, report)
    return report
