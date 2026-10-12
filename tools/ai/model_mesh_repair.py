"""Bounded local mesh repair; never creates a model or submits a provider task."""
from __future__ import annotations

import math
import os
from collections import Counter
from model_contracts import (
    MAX_LOCAL_BOUNDARY_EDGES,
    MAX_LOCAL_REPAIR_DIAGONAL_RATIO,
    MAX_LOCAL_REPAIR_FACE_RATIO,
    MAX_NOISE_COMPONENT_DIAGONAL_RATIO,
    MAX_NOISE_COMPONENT_FACE_RATIO,
)
from model_job_support import MAX_MODEL_FACES
from model_obj_io import _resolve_obj_index, _write_mesh_repair_report
from pathlib import Path
from tripo_client import TripoError
from typing import Any

def _remove_small_detached_obj_components(path: Path, report_path: Path) -> dict[str, Any]:
    vertex_lines: list[str] = []
    vertices: list[tuple[float, float, float]] = []
    faces: list[tuple[int, int, int]] = []
    face_sections: list[tuple[str, str]] = []
    current_object = ""
    current_group = ""
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
                    except ValueError:
                        raise TripoError("The generated OBJ has an invalid vertex position.") from None
                    if not all(math.isfinite(value) for value in position):
                        raise TripoError("The generated OBJ has an invalid vertex position.")
                    vertices.append(position)
                    vertex_lines.append(" ".join(fields))
                elif keyword == "f":
                    if len(fields) != 4:
                        raise TripoError("The generated OBJ must contain only triangular faces.")
                    face = tuple(_resolve_obj_index(value, len(vertices), "vertex") for value in fields[1:])
                    faces.append(face)
                    face_sections.append((current_object, current_group))
                elif keyword == "o":
                    current_object = " ".join(fields)
                elif keyword == "g":
                    current_group = " ".join(fields)
    except UnicodeDecodeError:
        raise TripoError("The generated OBJ is not valid UTF-8 text.") from None
    except OSError:
        raise TripoError("The generated OBJ could not be read.") from None

    if not vertices or not faces:
        raise TripoError("The generated OBJ does not contain usable geometry.")

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

    for face in faces:
        unite(face[0], face[1])
        unite(face[1], face[2])

    component_faces: dict[int, list[tuple[int, int, int]]] = {}
    component_vertices: dict[int, set[int]] = {}
    for face in faces:
        root = find(face[0])
        component_faces.setdefault(root, []).append(face)
        component_vertices.setdefault(root, set()).update(face)

    def diagonal(indices: set[int]) -> float:
        minimum = [min(vertices[index][axis] for index in indices) for axis in range(3)]
        maximum = [max(vertices[index][axis] for index in indices) for axis in range(3)]
        return math.sqrt(sum((maximum[axis] - minimum[axis]) ** 2 for axis in range(3)))

    components = sorted(
        component_faces,
        key=lambda root: (len(component_faces[root]), len(component_vertices[root])),
        reverse=True,
    )
    main = components[0]
    main_face_count = len(component_faces[main])
    mesh_vertices = {index for indices in component_vertices.values() for index in indices}
    mesh_diagonal = diagonal(mesh_vertices)
    removable: set[int] = set()
    component_report: list[dict[str, Any]] = []
    for root in components:
        face_count = len(component_faces[root])
        vertex_count = len(component_vertices[root])
        component_diagonal = diagonal(component_vertices[root])
        face_ratio = face_count / len(faces)
        diagonal_ratio = component_diagonal / mesh_diagonal if mesh_diagonal > 0.0 else 0.0
        remove = (
            root != main
            and face_ratio <= MAX_NOISE_COMPONENT_FACE_RATIO
            and diagonal_ratio <= MAX_NOISE_COMPONENT_DIAGONAL_RATIO
        )
        if remove:
            removable.add(root)
        component_report.append({
            "faces": face_count,
            "vertices": vertex_count,
            "diagonal_mm": round(component_diagonal, 6),
            "face_ratio": round(face_ratio, 8),
            "diagonal_ratio": round(diagonal_ratio, 8),
            "removed": remove,
        })

    kept_face_records = [
        (face, section)
        for face, section in zip(faces, face_sections)
        if find(face[0]) not in removable
    ]
    if not kept_face_records:
        raise TripoError("Detached-component cleanup would remove all generated geometry.")
    kept_vertex_indices = sorted({index for face, _section in kept_face_records for index in face})
    vertex_map = {old: new for new, old in enumerate(kept_vertex_indices)}
    removed_vertices = len(vertices) - len(kept_vertex_indices)
    if removable or removed_vertices:
        temporary = path.with_name(path.name + ".components")
        try:
            with temporary.open("w", encoding="ascii", newline="\n") as output:
                output.write("# OrcaSlicer AI removed bounded detached mesh noise\n")
                for old_index in kept_vertex_indices:
                    output.write(vertex_lines[old_index] + "\n")
                previous_section = ("", "")
                for face, section in kept_face_records:
                    if section != previous_section:
                        if section[0]:
                            output.write(section[0] + "\n")
                        if section[1]:
                            output.write(section[1] + "\n")
                        previous_section = section
                    output.write("f {} {} {}\n".format(*(vertex_map[index] + 1 for index in face)))
            os.replace(temporary, path)
        except OSError:
            raise TripoError("Detached mesh noise could not be removed safely.") from None
        finally:
            try:
                temporary.unlink(missing_ok=True)
            except OSError:
                pass

    removed_faces = sum(len(component_faces[root]) for root in removable)
    report: dict[str, Any] = {
        "status": (
            "removed"
            if removable or removed_vertices
            else "not_needed" if len(components) == 1 else "preserved"
        ),
        "original_components": len(components),
        "kept_vertices": len(kept_vertex_indices),
        "kept_faces": len(kept_face_records),
        "removed_components": len(removable),
        "removed_vertices": removed_vertices,
        "removed_faces": removed_faces,
        "largest_component_faces": main_face_count,
        "largest_component_diagonal": diagonal(component_vertices[main]),
        "maximum_noise_component_face_ratio": MAX_NOISE_COMPONENT_FACE_RATIO,
        "maximum_noise_component_diagonal_ratio": MAX_NOISE_COMPONENT_DIAGONAL_RATIO,
        "components": component_report,
    }

    _write_mesh_repair_report(report_path, report)
    return report

def _repair_small_obj_topology_defects(
    path: Path, report_path: Path, report: dict[str, Any] | None = None
) -> dict[str, Any]:
    report = dict(report or {})
    vertex_lines: list[str] = []
    positions: list[tuple[float, float, float]] = []
    colors: list[tuple[str, ...]] = []
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
                    except ValueError:
                        raise TripoError("The generated OBJ has an invalid vertex position.") from None
                    if not all(math.isfinite(value) for value in position):
                        raise TripoError("The generated OBJ has an invalid vertex position.")
                    vertex_lines.append(" ".join(fields))
                    positions.append(position)
                    colors.append(tuple(fields[4:]))
                elif keyword == "f":
                    if len(fields) != 4:
                        raise TripoError("The generated OBJ must contain only triangular faces.")
                    face = tuple(_resolve_obj_index(value, len(positions), "vertex") for value in fields[1:])
                    faces.append(face)
    except UnicodeDecodeError:
        raise TripoError("The generated OBJ is not valid UTF-8 text.") from None
    except OSError:
        raise TripoError("The generated OBJ could not be read.") from None

    def edge_usage(source_faces: list[tuple[int, int, int]]) -> dict[tuple[int, int], list[tuple[int, int, int]]]:
        usage: dict[tuple[int, int], list[tuple[int, int, int]]] = {}
        for face_index, face in enumerate(source_faces):
            for left, right in ((face[0], face[1]), (face[1], face[2]), (face[2], face[0])):
                edge = (left, right) if left < right else (right, left)
                usage.setdefault(edge, []).append((face_index, left, right))
        return usage

    if any(len(set(face)) != 3 for face in faces):
        report.update(topology_status="deferred", topology_deferred_reason="degenerate_faces")
        _write_mesh_repair_report(report_path, report)
        return report
    original_usage = edge_usage(faces)
    original_boundary = sum(len(uses) == 1 for uses in original_usage.values())
    original_non_manifold = sum(len(uses) > 2 for uses in original_usage.values())
    original_inconsistent_winding = sum(
        len(uses) == 2 and uses[0][1:] == uses[1][1:]
        for uses in original_usage.values()
    )
    report.update(
        topology_status=(
            "not_needed"
            if not original_boundary and not original_non_manifold and not original_inconsistent_winding
            else "deferred"
        ),
        original_boundary_edges=original_boundary,
        original_non_manifold_edges=original_non_manifold,
        original_inconsistent_winding_edges=original_inconsistent_winding,
        removed_non_manifold_faces=0,
        flipped_winding_faces=0,
        removed_topology_vertices=0,
        filled_boundary_loops=0,
        added_vertices=0,
        added_faces=0,
        remaining_invalid_edges=original_boundary + original_non_manifold + original_inconsistent_winding,
    )
    if not original_boundary and not original_non_manifold and not original_inconsistent_winding:
        _write_mesh_repair_report(report_path, report)
        return report

    mesh_minimum = [min(position[axis] for position in positions) for axis in range(3)]
    mesh_maximum = [max(position[axis] for position in positions) for axis in range(3)]
    mesh_diagonal = math.sqrt(sum((mesh_maximum[axis] - mesh_minimum[axis]) ** 2 for axis in range(3)))
    if mesh_diagonal <= 0.0:
        _write_mesh_repair_report(report_path, report)
        return report

    working_faces = list(faces)
    removed_face_count = 0
    if original_non_manifold:
        remove_indices = {
            face_index
            for uses in original_usage.values()
            if len(uses) > 2
            for face_index, _left, _right in uses
        }
        max_removed_faces = max(64, int(len(faces) * MAX_LOCAL_REPAIR_FACE_RATIO))
        if len(remove_indices) > max_removed_faces:
            report["topology_deferred_reason"] = "too_many_non_manifold_faces"
            _write_mesh_repair_report(report_path, report)
            return report

        vertex_faces: dict[int, list[int]] = {}
        for face_index in remove_indices:
            for vertex in faces[face_index]:
                vertex_faces.setdefault(vertex, []).append(face_index)
        remaining_faces = set(remove_indices)
        defect_regions: list[set[int]] = []
        while remaining_faces:
            pending = [min(remaining_faces)]
            remaining_faces.remove(pending[0])
            region: set[int] = set()
            while pending:
                face_index = pending.pop()
                region.add(face_index)
                for vertex in faces[face_index]:
                    for adjacent in vertex_faces[vertex]:
                        if adjacent in remaining_faces:
                            remaining_faces.remove(adjacent)
                            pending.append(adjacent)
            defect_regions.append(region)

        max_region_diagonal_ratio = 0.0
        for region in defect_regions:
            defect_vertices = {vertex for face_index in region for vertex in faces[face_index]}
            defect_minimum = [min(positions[index][axis] for index in defect_vertices) for axis in range(3)]
            defect_maximum = [max(positions[index][axis] for index in defect_vertices) for axis in range(3)]
            defect_diagonal = math.sqrt(
                sum((defect_maximum[axis] - defect_minimum[axis]) ** 2 for axis in range(3))
            )
            max_region_diagonal_ratio = max(max_region_diagonal_ratio, defect_diagonal / mesh_diagonal)
        report.update(
            non_manifold_regions=len(defect_regions),
            max_non_manifold_region_diagonal_ratio=max_region_diagonal_ratio,
        )
        if max_region_diagonal_ratio > MAX_LOCAL_REPAIR_DIAGONAL_RATIO:
            report["topology_deferred_reason"] = "non_manifold_region_too_large"
            _write_mesh_repair_report(report_path, report)
            return report
        working_faces = [face for index, face in enumerate(faces) if index not in remove_indices]
        removed_face_count = len(remove_indices)

    def normalize_face_winding(
        source_faces: list[tuple[int, int, int]],
    ) -> tuple[list[tuple[int, int, int]] | None, int, int]:
        usage = edge_usage(source_faces)
        adjacency: list[list[tuple[int, bool]]] = [[] for _ in source_faces]
        inconsistent_before = 0
        for uses in usage.values():
            if len(uses) != 2:
                continue
            left_face, left_from, left_to = uses[0]
            right_face, right_from, right_to = uses[1]
            same_direction = left_from == right_from and left_to == right_to
            inconsistent_before += int(same_direction)
            adjacency[left_face].append((right_face, same_direction))
            adjacency[right_face].append((left_face, same_direction))

        flips: list[bool | None] = [None] * len(source_faces)
        for start in range(len(source_faces)):
            if flips[start] is not None:
                continue
            flips[start] = False
            pending = [start]
            component: list[int] = []
            while pending:
                face_index = pending.pop()
                component.append(face_index)
                for adjacent, must_differ in adjacency[face_index]:
                    expected = bool(flips[face_index]) ^ must_differ
                    if flips[adjacent] is None:
                        flips[adjacent] = expected
                        pending.append(adjacent)
                    elif flips[adjacent] != expected:
                        return None, 0, inconsistent_before
            if sum(bool(flips[index]) for index in component) > len(component) // 2:
                for index in component:
                    flips[index] = not bool(flips[index])

        oriented_faces = [
            (face[0], face[2], face[1]) if flips[index] else face
            for index, face in enumerate(source_faces)
        ]
        oriented_usage = edge_usage(oriented_faces)
        inconsistent_after = sum(
            len(uses) == 2 and uses[0][1:] == uses[1][1:]
            for uses in oriented_usage.values()
        )
        return oriented_faces, sum(bool(value) for value in flips), inconsistent_after

    oriented_faces, flipped_face_count, remaining_inconsistent_winding = normalize_face_winding(working_faces)
    report.update(
        flipped_winding_faces=flipped_face_count,
        remaining_inconsistent_winding_edges=remaining_inconsistent_winding,
    )
    if oriented_faces is None or remaining_inconsistent_winding:
        report["topology_deferred_reason"] = "non_orientable_face_winding"
        _write_mesh_repair_report(report_path, report)
        return report
    working_faces = oriented_faces

    usage = edge_usage(working_faces)
    if any(len(uses) > 2 for uses in usage.values()):
        _write_mesh_repair_report(report_path, report)
        return report
    boundary = {edge: uses[0][1:] for edge, uses in usage.items() if len(uses) == 1}

    outgoing: dict[int, list[tuple[int, tuple[int, int]]]] = {}
    incoming_count: Counter[int] = Counter()
    outgoing_count: Counter[int] = Counter()
    for edge, (left, right) in boundary.items():
        outgoing.setdefault(left, []).append((right, edge))
        outgoing_count[left] += 1
        incoming_count[right] += 1
    boundary_vertices = set(incoming_count) | set(outgoing_count)
    if any(
        incoming_count[index] != outgoing_count[index] or incoming_count[index] not in {1, 2}
        for index in boundary_vertices
    ):
        _write_mesh_repair_report(report_path, report)
        return report
    for entries in outgoing.values():
        entries.sort(reverse=True)

    unused = set(boundary)
    circuits: list[list[int]] = []
    while unused:
        start = min(left for edge, (left, _right) in boundary.items() if edge in unused)
        stack = [start]
        circuit: list[int] = []
        while stack:
            current = stack[-1]
            entries = outgoing.get(current, [])
            while entries and entries[-1][1] not in unused:
                entries.pop()
            if entries:
                right, edge = entries.pop()
                unused.remove(edge)
                stack.append(right)
            else:
                circuit.append(stack.pop())
        circuit.reverse()
        if len(circuit) < 4 or circuit[0] != circuit[-1]:
            _write_mesh_repair_report(report_path, report)
            return report
        circuits.append(circuit)

    cycles: list[list[int]] = []
    for circuit in circuits:
        remainder = circuit
        while len(remainder) > 1:
            seen: dict[int, int] = {}
            for index, vertex in enumerate(remainder):
                if vertex not in seen:
                    seen[vertex] = index
                    continue
                begin = seen[vertex]
                cycle = remainder[begin:index + 1]
                if len(cycle) < 4:
                    _write_mesh_repair_report(report_path, report)
                    return report
                cycles.append(cycle)
                remainder = remainder[:begin + 1] + remainder[index + 1:]
                break
            else:
                _write_mesh_repair_report(report_path, report)
                return report

    if sum(len(cycle) - 1 for cycle in cycles) != len(boundary):
        _write_mesh_repair_report(report_path, report)
        return report
    for cycle in cycles:
        cycle_vertices = cycle[:-1]
        cycle_minimum = [min(positions[index][axis] for index in cycle_vertices) for axis in range(3)]
        cycle_maximum = [max(positions[index][axis] for index in cycle_vertices) for axis in range(3)]
        cycle_diagonal = math.sqrt(
            sum((cycle_maximum[axis] - cycle_minimum[axis]) ** 2 for axis in range(3))
        )
        if len(cycle_vertices) > MAX_LOCAL_BOUNDARY_EDGES or cycle_diagonal > mesh_diagonal * MAX_LOCAL_REPAIR_DIAGONAL_RATIO:
            _write_mesh_repair_report(report_path, report)
            return report

    patched_faces = list(working_faces)
    for cycle in cycles:
        cycle_vertices = cycle[:-1]
        center = tuple(
            sum(positions[index][axis] for index in cycle_vertices) / len(cycle_vertices)
            for axis in range(3)
        )
        color = Counter(colors[index] for index in cycle_vertices).most_common(1)[0][0]
        center_index = len(positions)
        positions.append(center)
        colors.append(color)
        vertex_lines.append(
            "v " + " ".join(f"{value:.9g}" for value in center) + " " + " ".join(color)
        )
        patched_faces.extend(
            (cycle[index + 1], cycle[index], center_index)
            for index in range(len(cycle_vertices))
        )

    final_usage = edge_usage(patched_faces)
    remaining_invalid = sum(len(uses) != 2 for uses in final_usage.values())
    if remaining_invalid or len(patched_faces) > MAX_MODEL_FACES:
        _write_mesh_repair_report(report_path, report)
        return report

    referenced = sorted({vertex for face in patched_faces for vertex in face})
    remap = {old_index: new_index + 1 for new_index, old_index in enumerate(referenced)}
    temporary = path.with_suffix(path.suffix + ".part")
    try:
        with temporary.open("w", encoding="ascii", newline="\n") as output:
            output.write("# OrcaSlicer AI repaired small local mesh defects\n")
            for index in referenced:
                output.write(vertex_lines[index] + "\n")
            for face in patched_faces:
                output.write("f " + " ".join(str(remap[index]) for index in face) + "\n")
        os.replace(temporary, path)
    except OSError:
        try:
            temporary.unlink(missing_ok=True)
        except OSError:
            pass
        raise TripoError("The generated OBJ could not be rewritten after topology repair.") from None

    original_vertex_count = len(vertex_lines) - len(cycles)
    removed_vertex_count = original_vertex_count - sum(index < original_vertex_count for index in referenced)
    report.update(
        status="repaired",
        topology_status="repaired",
        kept_vertices=len(referenced),
        kept_faces=len(patched_faces),
        removed_non_manifold_faces=removed_face_count,
        removed_topology_vertices=removed_vertex_count,
        filled_boundary_loops=len(cycles),
        added_vertices=len(cycles),
        added_faces=sum(len(cycle) - 1 for cycle in cycles),
        remaining_inconsistent_winding_edges=0,
        remaining_invalid_edges=0,
    )
    report.pop("topology_deferred_reason", None)
    _write_mesh_repair_report(report_path, report)
    return report
