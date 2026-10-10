"""Legacy portrait material correction on explicitly supplied model files."""
from __future__ import annotations

import math
import os
from array import array
from collections import Counter
from model_color_space import _portrait_material_palette_indices
from model_contracts import (
    PORTRAIT_FACE_DETAIL_GRID_SIZE,
    PORTRAIT_FACE_DETAIL_HALF_WIDTH_RATIO,
    PORTRAIT_FACE_DETAIL_MAX_HEIGHT_RATIO,
    PORTRAIT_FACE_DETAIL_MIN_HEIGHT_RATIO,
    PORTRAIT_FACE_DETAIL_SURFACE_TOLERANCE_MM,
    PORTRAIT_FRONT_SURFACE_QUANTILE,
    PORTRAIT_GARMENT_SMOOTHING_PASSES,
    PORTRAIT_HAND_BOUNDARY_MAX_REMOVAL_RATIO,
    PORTRAIT_HAND_BOUNDARY_MIN_PRIMARY_SUPPORT,
    PORTRAIT_HAND_BOUNDARY_PASSES,
    PORTRAIT_HAND_COMPACT_EXTENT_RATIO,
    PORTRAIT_HAND_DIFFUSE_SIZE_RATIO,
    PORTRAIT_HAND_MIN_HEIGHT_RATIO,
    PORTRAIT_REAR_GARMENT_HEIGHT_RATIO,
    PORTRAIT_REAR_HAIR_HEIGHT_RATIO,
    PORTRAIT_REAR_PLATE_MAX_START_RATIO,
    PORTRAIT_REAR_PLATE_MIN_RUN_RATIO,
    PORTRAIT_STRUCTURE_FRONT_QUANTILE,
    PortraitGeometryGateError,
)
from model_obj_io import _resolve_obj_index, _write_mesh_repair_report
from pathlib import Path
from tripo_client import TripoError
from typing import Any, Mapping

def _stabilize_portrait_obj_materials(
    path: Path,
    report_path: Path,
    palette: tuple[str, ...],
    palette_roles: Mapping[str, str] | None,
    enabled: bool,
) -> dict[str, Any]:
    """Lock a detected portrait bust's lowest band to its semantic material.

    Tripo can paint a source-invisible lower transition with garment, skin, or accent colours.
    This pass is intentionally narrower than generic colour cleanup: it requires
    upstream portrait evidence, an explicit neutral-garment/skin palette, and a
    substantial bottom band that is already predominantly the dark structure colour.
    """
    portrait_indices = _portrait_material_palette_indices(palette, palette_roles, enabled)
    if portrait_indices is None or not palette_roles:
        return {"status": "not_applicable", "recolored_vertices": 0}
    try:
        primary_hex = str(palette_roles["primary"]).strip().upper()
        structure_hex = str(palette_roles["structure"]).strip().upper()
        primary = tuple(int(primary_hex[index:index + 2], 16) for index in (1, 3, 5))
        structure = tuple(int(structure_hex[index:index + 2], 16) for index in (1, 3, 5))
    except (KeyError, ValueError):
        return {"status": "not_applicable", "recolored_vertices": 0}

    positions: list[tuple[float, float, float]] = []
    colors: list[tuple[int, int, int]] = []
    try:
        with path.open("r", encoding="utf-8", errors="strict") as stream:
            for line in stream:
                fields = line.strip().split()
                if not fields or fields[0].lower() != "v":
                    continue
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
    except (OSError, UnicodeDecodeError, ValueError):
        raise TripoError("The generated OBJ could not be read for portrait material cleanup.") from None
    if not positions:
        raise TripoError("The generated OBJ has no vertices for portrait material cleanup.")

    minimum_z = min(position[2] for position in positions)
    maximum_z = max(position[2] for position in positions)
    span_z = maximum_z - minimum_z
    if span_z <= 1e-9:
        raise TripoError("The generated portrait has invalid dimensions for material cleanup.")
    base_top = minimum_z + span_z * 0.065
    base_indices = [index for index, position in enumerate(positions) if position[2] <= base_top]
    base_structure_ratio = (
        sum(colors[index] == structure for index in base_indices) / len(base_indices)
        if base_indices else 0.0
    )
    primary_ratio = sum(color == primary for color in colors) / len(colors)
    activated = (
        len(base_indices) / len(positions) >= 0.02
        and base_structure_ratio >= 0.65
        and primary_ratio >= 0.25
    )
    report: dict[str, Any] = {
        "status": "not_applicable",
        "activated": activated,
        "vertex_count": len(positions),
        "primary_vertex_ratio": round(primary_ratio, 6),
        "base_vertex_ratio": round(len(base_indices) / len(positions), 6),
        "base_structure_ratio": round(base_structure_ratio, 6),
        "base_height_ratio": 0.065,
        "recolored_vertices": 0,
        "recolored_by_rule": {"base": 0},
        "recolored_by_source": {},
    }
    if not activated:
        _write_mesh_repair_report(report_path, report)
        return report

    changed_by_rule: Counter[str] = Counter()
    changed_by_source: Counter[tuple[int, int, int]] = Counter()
    updated = list(colors)
    for index, (position, source) in enumerate(zip(positions, colors)):
        target = source
        rule = ""
        if position[2] <= base_top:
            target = structure
            rule = "base"
        if target != source:
            updated[index] = target
            changed_by_rule[rule] += 1
            changed_by_source[source] += 1

    if changed_by_rule:
        temporary = path.with_name(path.name + ".portrait-materials")
        vertex_index = 0
        try:
            with path.open("r", encoding="utf-8", errors="strict") as source, temporary.open(
                "w", encoding="ascii", newline="\n"
            ) as output:
                for line in source:
                    fields = line.strip().split()
                    if fields and fields[0].lower() == "v":
                        red, green, blue = updated[vertex_index]
                        fields[4:7] = [f"{channel / 255.0:.6f}" for channel in (red, green, blue)]
                        output.write(" ".join(fields) + "\n")
                        vertex_index += 1
                    else:
                        output.write(line if line.endswith("\n") else line + "\n")
            os.replace(temporary, path)
        except (OSError, UnicodeDecodeError):
            raise TripoError("Portrait materials could not be stabilized safely.") from None
        finally:
            try:
                temporary.unlink(missing_ok=True)
            except OSError:
                pass

    report["status"] = "stabilized" if changed_by_rule else "not_needed"
    report["recolored_vertices"] = sum(changed_by_rule.values())
    report["recolored_by_rule"] = {
        "base": changed_by_rule["base"],
    }
    report["recolored_by_source"] = {
        "#{:02X}{:02X}{:02X}".format(*color): count
        for color, count in sorted(changed_by_source.items())
    }
    _write_mesh_repair_report(report_path, report)
    return report

def _stabilize_portrait_obj_garment_regions(
    path: Path,
    report_path: Path,
    palette: tuple[str, ...],
    palette_roles: Mapping[str, str] | None,
    enabled: bool,
) -> dict[str, Any]:
    """Remove sparse Tripo material noise from a detected portrait bust.

    The generic colour-island pass intentionally has a small global budget and
    cannot clean thin colour tendrils that remain connected through shared
    vertices. A portrait bust gives us stronger evidence: below the head, its
    source-invisible rear surface belongs to the dominant garment, while real
    hands and the front accent garment form locally coherent regions. This pass
    erodes only non-garment vertices that are surrounded by garment vertices and
    cleans weakly supported rear projections. It stays disabled for every model
    without the upstream portrait and low-band evidence.
    """
    portrait_indices = _portrait_material_palette_indices(palette, palette_roles, enabled)
    if portrait_indices is None or not palette_roles:
        return {"status": "not_applicable", "activated": False, "recolored_vertices": 0}
    try:
        role_colors = {
            role: tuple(
                int(str(palette_roles[role]).strip().upper()[index:index + 2], 16)
                for index in (1, 3, 5)
            )
            for role in ("primary", "structure", "light", "accent")
        }
    except (KeyError, ValueError):
        return {"status": "not_applicable", "activated": False, "recolored_vertices": 0}
    if len(set(role_colors.values())) != 4:
        return {"status": "not_applicable", "activated": False, "recolored_vertices": 0}

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
        raise TripoError("The generated OBJ could not be read for portrait garment cleanup.") from None
    if not positions or not faces:
        raise TripoError("The generated OBJ has no usable geometry for portrait garment cleanup.")

    minimum_x = min(position[0] for position in positions)
    maximum_x = max(position[0] for position in positions)
    minimum_z = min(position[2] for position in positions)
    maximum_z = max(position[2] for position in positions)
    span_x = maximum_x - minimum_x
    span_z = maximum_z - minimum_z
    if span_x <= 1e-9 or span_z <= 1e-9:
        raise TripoError("The generated portrait has invalid dimensions for garment cleanup.")

    primary = role_colors["primary"]
    structure = role_colors["structure"]
    base_top = minimum_z + span_z * 0.065
    base_indices = [index for index, position in enumerate(positions) if position[2] <= base_top]
    base_structure_ratio = (
        sum(colors[index] == structure for index in base_indices) / len(base_indices)
        if base_indices else 0.0
    )
    primary_ratio = sum(color == primary for color in colors) / len(colors)
    activated = (
        len(base_indices) / len(positions) >= 0.02
        and base_structure_ratio >= 0.65
        and primary_ratio >= 0.25
    )
    report: dict[str, Any] = {
        "status": "not_applicable",
        "activated": activated,
        "vertex_count": len(positions),
        "face_count": len(faces),
        "primary_vertex_ratio": round(primary_ratio, 6),
        "base_structure_ratio": round(base_structure_ratio, 6),
        "rear_garment_height_ratio": PORTRAIT_REAR_GARMENT_HEIGHT_RATIO,
        "passes": [],
        "recolored_vertices": 0,
        "recolored_by_source": {},
    }
    if not activated:
        _write_mesh_repair_report(report_path, report)
        return report

    body_x = sorted(
        position[0]
        for position in positions
        if base_top < position[2] < minimum_z + span_z * PORTRAIT_REAR_GARMENT_HEIGHT_RATIO
    )
    rear_threshold = (
        body_x[round((len(body_x) - 1) * PORTRAIT_FRONT_SURFACE_QUANTILE)]
        if body_x else minimum_x + span_x * PORTRAIT_FRONT_SURFACE_QUANTILE
    )
    body_y = [
        position[1]
        for position in positions
        if base_top < position[2] < minimum_z + span_z * PORTRAIT_REAR_GARMENT_HEIGHT_RATIO
    ]
    minimum_body_y = min(body_y) if body_y else min(position[1] for position in positions)
    maximum_body_y = max(body_y) if body_y else max(position[1] for position in positions)
    body_y_center = (minimum_body_y + maximum_body_y) * 0.5
    body_y_span = max(maximum_body_y - minimum_body_y, 1e-9)
    changed_by_source: Counter[tuple[int, int, int]] = Counter()
    changed_by_rule: Counter[str] = Counter()
    pass_reports: list[dict[str, Any]] = []
    for pass_index in range(PORTRAIT_GARMENT_SMOOTHING_PASSES):
        same_support = array("I", [0]) * len(positions)
        primary_support = array("I", [0]) * len(positions)
        for face in faces:
            for left, right in ((face[0], face[1]), (face[1], face[2]), (face[2], face[0])):
                left_color, right_color = colors[left], colors[right]
                if left_color == right_color:
                    same_support[left] += 1
                    same_support[right] += 1
                if right_color == primary:
                    primary_support[left] += 1
                if left_color == primary:
                    primary_support[right] += 1

        changed_indices: list[int] = []
        rear_changes = 0
        support_changes = 0
        for index, (position, source) in enumerate(zip(positions, colors)):
            if source == primary or position[2] <= base_top:
                continue
            height_ratio = (position[2] - minimum_z) / span_z
            if height_ratio >= PORTRAIT_REAR_GARMENT_HEIGHT_RATIO:
                continue
            rear = position[0] < rear_threshold
            front_accent_region = (
                source == role_colors["accent"]
                and position[0] >= rear_threshold - span_x * 0.05
                and abs(position[1] - body_y_center) <= body_y_span * 0.22
                and 0.18 <= height_ratio < PORTRAIT_REAR_GARMENT_HEIGHT_RATIO
            )
            weak_rear_projection = (
                source != role_colors["light"]
                and rear
                and (
                    source == role_colors["accent"]
                    or height_ratio < PORTRAIT_REAR_HAIR_HEIGHT_RATIO
                    or same_support[index] < 4
                )
            )
            surrounded_by_garment = (
                source != role_colors["light"]
                and not front_accent_region
                and primary_support[index] >= 4
                and primary_support[index] >= same_support[index] + 2
                and primary_support[index] * 2 >= same_support[index] * 3
            )
            if not weak_rear_projection and not surrounded_by_garment:
                continue
            changed_indices.append(index)
            rear_changes += int(weak_rear_projection)
            support_changes += int(not weak_rear_projection and surrounded_by_garment)

        for index in changed_indices:
            changed_by_source[colors[index]] += 1
            colors[index] = primary
        changed_by_rule["rear_or_sparse_garment"] += len(changed_indices)
        pass_reports.append({
            "pass": pass_index + 1,
            "recolored_vertices": len(changed_indices),
            "rear_garment_vertices": rear_changes,
            "surrounded_vertices": support_changes,
        })
        if not changed_indices:
            break

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

    component_vertices: Counter[int] = Counter()
    component_color: dict[int, tuple[int, int, int]] = {}
    component_minimum_y: dict[int, float] = {}
    component_maximum_y: dict[int, float] = {}
    component_minimum_z: dict[int, float] = {}
    component_maximum_z: dict[int, float] = {}
    component_sum_x: Counter[int] = Counter()
    component_sum_y: Counter[int] = Counter()
    component_sum_z: Counter[int] = Counter()
    for index, (position, color) in enumerate(zip(positions, colors)):
        root = find(index)
        component_vertices[root] += 1
        component_color[root] = color
        component_minimum_y[root] = min(component_minimum_y.get(root, position[1]), position[1])
        component_maximum_y[root] = max(component_maximum_y.get(root, position[1]), position[1])
        component_minimum_z[root] = min(component_minimum_z.get(root, position[2]), position[2])
        component_maximum_z[root] = max(component_maximum_z.get(root, position[2]), position[2])
        component_sum_x[root] += position[0]
        component_sum_y[root] += position[1]
        component_sum_z[root] += position[2]

    skin = role_colors["light"]
    skin_components = sorted(
        (root for root, color in component_color.items() if color == skin),
        key=lambda root: (-component_vertices[root], root),
    )
    protected_skin_roots: set[int] = set()
    face_skin_root: int | None = None
    for root in skin_components:
        maximum_height = (component_maximum_z[root] - minimum_z) / span_z
        if maximum_height >= 0.65:
            protected_skin_roots.add(root)
            face_skin_root = root
            break
    minimum_hand_vertices = max(8, int(len(positions) * 0.001))
    for root in skin_components:
        if root in protected_skin_roots or len(protected_skin_roots) >= 3:
            continue
        minimum_height = (component_minimum_z[root] - minimum_z) / span_z
        maximum_height = (component_maximum_z[root] - minimum_z) / span_z
        centroid_x = component_sum_x[root] / component_vertices[root]
        if (
            component_vertices[root] >= minimum_hand_vertices
            and minimum_height >= PORTRAIT_HAND_MIN_HEIGHT_RATIO
            and maximum_height <= 0.65
            and centroid_x >= rear_threshold
        ):
            protected_skin_roots.add(root)

    protected_hand_roots = set(protected_skin_roots)
    if face_skin_root is not None:
        protected_hand_roots.discard(face_skin_root)
    compact_hand_roots = {
        root for root in protected_hand_roots
        if component_vertices[root] >= minimum_hand_vertices
        and (
            (component_maximum_y[root] - component_minimum_y[root]) / body_y_span
            + (component_maximum_z[root] - component_minimum_z[root]) / span_z
        ) <= PORTRAIT_HAND_COMPACT_EXTENT_RATIO
    }
    largest_compact_hand = max(
        (component_vertices[root] for root in compact_hand_roots),
        default=0,
    )
    discarded_diffuse_skin_roots = {
        root for root in protected_hand_roots
        if largest_compact_hand > 0
        and root not in compact_hand_roots
        and component_vertices[root] >= largest_compact_hand * PORTRAIT_HAND_DIFFUSE_SIZE_RATIO
        and (
            (component_maximum_y[root] - component_minimum_y[root]) / body_y_span
            + (component_maximum_z[root] - component_minimum_z[root]) / span_z
        ) > PORTRAIT_HAND_COMPACT_EXTENT_RATIO
    }
    protected_hand_roots.difference_update(discarded_diffuse_skin_roots)
    protected_skin_roots.difference_update(discarded_diffuse_skin_roots)
    structure_front_threshold = (
        body_x[round((len(body_x) - 1) * PORTRAIT_STRUCTURE_FRONT_QUANTILE)]
        if body_x else rear_threshold
    )
    accent = role_colors["accent"]
    accent_candidates: list[tuple[int, int, float]] = []
    # Tripo frequently splits one coherent scarf or shirt panel across UV seams.
    # Keep small front-centre fragments long enough to be grouped semantically;
    # off-centre and rear accent speckles are still removed below.
    minimum_accent_vertices = max(4, int(len(positions) * 0.00001))
    for root, color in component_color.items():
        if color != accent:
            continue
        count = component_vertices[root]
        centroid_x = component_sum_x[root] / count
        centroid_y = component_sum_y[root] / count
        centroid_z_ratio = (component_sum_z[root] / count - minimum_z) / span_z
        if (
            centroid_x >= rear_threshold - span_x * 0.05
            and abs(centroid_y - body_y_center) <= body_y_span * 0.22
            and 0.18 <= centroid_z_ratio < PORTRAIT_REAR_GARMENT_HEIGHT_RATIO
            and count >= minimum_accent_vertices
        ):
            accent_candidates.append((root, count, centroid_z_ratio))
    protected_accent_roots: set[int] = set()
    accent_anchor_height_ratio = 0.0
    if accent_candidates:
        anchor_root, anchor_count, _ = max(accent_candidates, key=lambda item: (item[1], -item[0]))
        accent_anchor_height_ratio = (component_minimum_z[anchor_root] - minimum_z) / span_z
        minimum_related_height = max(0.18, accent_anchor_height_ratio - 0.07)
        protected_accent_roots = {
            root
            for root, count, centroid_z_ratio in accent_candidates
            if root == anchor_root
            or centroid_z_ratio >= minimum_related_height
            or count >= anchor_count * 0.75
        }
    component_neighbor_colors: dict[int, Counter[tuple[int, int, int]]] = {}
    for face in faces:
        for left, right in ((face[0], face[1]), (face[1], face[2]), (face[2], face[0])):
            left_root, right_root = find(left), find(right)
            if left_root == right_root:
                continue
            component_neighbor_colors.setdefault(left_root, Counter())[colors[right]] += 1
            component_neighbor_colors.setdefault(right_root, Counter())[colors[left]] += 1
    maximum_accent_shadow_vertices = max(24, int(len(positions) * 0.003))
    enclosed_accent_shadow_roots: set[int] = set()
    for root, color in component_color.items():
        if color != structure:
            continue
        count = component_vertices[root]
        centroid_x = component_sum_x[root] / count
        centroid_y = component_sum_y[root] / count
        centroid_z_ratio = (component_sum_z[root] / count - minimum_z) / span_z
        accent_boundary = component_neighbor_colors.get(root, Counter())[accent]
        required_boundary = max(4, min(16, round(math.sqrt(count))))
        if (
            count <= maximum_accent_shadow_vertices
            and accent_boundary >= required_boundary
            and centroid_x >= structure_front_threshold
            and abs(centroid_y - body_y_center) <= body_y_span * 0.18
            and 0.24 <= centroid_z_ratio < PORTRAIT_REAR_GARMENT_HEIGHT_RATIO
            and (component_maximum_y[root] - component_minimum_y[root]) <= body_y_span * 0.16
            and (component_maximum_z[root] - component_minimum_z[root]) <= span_z * 0.12
        ):
            enclosed_accent_shadow_roots.add(root)
    semantic_updated = list(colors)
    for index, (position, source) in enumerate(zip(positions, colors)):
        if source == primary or position[2] <= base_top:
            continue
        height_ratio = (position[2] - minimum_z) / span_z
        front_centered = (
            position[0] >= structure_front_threshold
            and abs(position[1] - body_y_center) <= body_y_span * 0.22
        )
        keep = True
        if source == skin:
            root = find(index)
            keep = root in protected_skin_roots
        elif source == accent:
            keep = find(index) in protected_accent_roots
        elif source == structure:
            if find(index) in enclosed_accent_shadow_roots:
                semantic_updated[index] = accent
                changed_by_source[source] += 1
                changed_by_rule["enclosed_accent_shadow"] += 1
                continue
            keep = (
                height_ratio >= PORTRAIT_REAR_GARMENT_HEIGHT_RATIO
                or (
                    front_centered
                    and 0.26 <= height_ratio < PORTRAIT_REAR_GARMENT_HEIGHT_RATIO
                )
            )
        if not keep:
            semantic_updated[index] = primary
            changed_by_source[source] += 1
            changed_by_rule[
                "diffuse_skin_component" if source == skin and root in discarded_diffuse_skin_roots
                else "skin_component" if source == skin
                else "accent_ownership" if source == accent
                else "structure_ownership"
            ] += 1
    hand_component_sizes = Counter(
        find(index)
        for index, source in enumerate(semantic_updated)
        if source == skin and find(index) in protected_hand_roots
    )
    hand_trimmed_by_root: Counter[int] = Counter()
    hand_pass_reports: list[dict[str, Any]] = []
    for pass_index in range(PORTRAIT_HAND_BOUNDARY_PASSES):
        hand_same_support = array("I", [0]) * len(positions)
        hand_primary_support = array("I", [0]) * len(positions)
        for face in faces:
            for left, right in ((face[0], face[1]), (face[1], face[2]), (face[2], face[0])):
                left_color, right_color = semantic_updated[left], semantic_updated[right]
                if left_color == right_color:
                    hand_same_support[left] += 1
                    hand_same_support[right] += 1
                if right_color == primary:
                    hand_primary_support[left] += 1
                if left_color == primary:
                    hand_primary_support[right] += 1

        candidates_by_root: dict[int, list[int]] = {}
        for index, source in enumerate(semantic_updated):
            if source != skin:
                continue
            root = find(index)
            if root not in protected_hand_roots:
                continue
            primary_neighbors = hand_primary_support[index]
            same_neighbors = hand_same_support[index]
            if (
                primary_neighbors >= PORTRAIT_HAND_BOUNDARY_MIN_PRIMARY_SUPPORT
                and primary_neighbors > same_neighbors
            ):
                candidates_by_root.setdefault(root, []).append(index)

        changed_this_pass = 0
        for root, candidates in candidates_by_root.items():
            maximum_trim = max(
                1,
                int(hand_component_sizes[root] * PORTRAIT_HAND_BOUNDARY_MAX_REMOVAL_RATIO),
            )
            remaining_budget = maximum_trim - hand_trimmed_by_root[root]
            if remaining_budget <= 0:
                continue
            candidates.sort(
                key=lambda index: (
                    hand_primary_support[index] - hand_same_support[index],
                    hand_primary_support[index],
                    -hand_same_support[index],
                    -index,
                ),
                reverse=True,
            )
            selected = candidates[:remaining_budget]
            for index in selected:
                semantic_updated[index] = primary
                changed_by_source[skin] += 1
            hand_trimmed_by_root[root] += len(selected)
            changed_this_pass += len(selected)
        hand_pass_reports.append({
            "pass": pass_index + 1,
            "candidate_vertices": sum(len(items) for items in candidates_by_root.values()),
            "recolored_vertices": changed_this_pass,
        })
        changed_by_rule["hand_boundary"] += changed_this_pass
        if not changed_this_pass:
            break

    minimum_face_detail_vertices = max(4, int(len(positions) * 0.0001))
    maximum_face_detail_vertices = max(16, int(len(positions) * 0.002))
    face_primary_roots = []
    for root, color in component_color.items():
        if color != primary:
            continue
        count = component_vertices[root]
        if not minimum_face_detail_vertices <= count <= maximum_face_detail_vertices:
            continue
        centroid_x = component_sum_x[root] / count
        centroid_y = component_sum_y[root] / count
        minimum_height = (component_minimum_z[root] - minimum_z) / span_z
        maximum_height = (component_maximum_z[root] - minimum_z) / span_z
        if (
            centroid_x >= structure_front_threshold
            and abs(centroid_y - body_y_center) <= body_y_span * 0.25
            and minimum_height >= 0.78
        ):
            face_primary_roots.append((root, centroid_y, minimum_height, maximum_height))

    forehead_highlight_roots = {
        root for root, _, minimum_height, _ in face_primary_roots
        if minimum_height >= 0.86
    }
    eye_candidates = [
        item for item in face_primary_roots
        if 0.78 <= item[2] and item[3] <= 0.86
    ]
    left_eyes = [item for item in eye_candidates if item[1] < body_y_center]
    right_eyes = [item for item in eye_candidates if item[1] > body_y_center]
    paired_eye_roots: set[int] = set()
    if left_eyes and right_eyes:
        pair = min(
            ((left, right) for left in left_eyes for right in right_eyes),
            key=lambda items: (
                abs((items[0][2] + items[0][3]) - (items[1][2] + items[1][3])),
                abs(math.log(max(1, component_vertices[items[0][0]]) / max(1, component_vertices[items[1][0]]))),
            ),
        )
        left_root, right_root = pair[0][0], pair[1][0]
        size_ratio = max(component_vertices[left_root], component_vertices[right_root]) / max(
            1, min(component_vertices[left_root], component_vertices[right_root])
        )
        if size_ratio <= 3.0:
            paired_eye_roots.update((left_root, right_root))

    eye_upper_cuts = {
        root: component_minimum_z[root]
        + (component_maximum_z[root] - component_minimum_z[root]) * 0.45
        for root in paired_eye_roots
    }
    face_highlight_changes = 0
    eye_upper_changes = 0
    for index, (position, source) in enumerate(zip(positions, semantic_updated)):
        if source != primary:
            continue
        root = find(index)
        if root in forehead_highlight_roots:
            semantic_updated[index] = skin
            changed_by_source[primary] += 1
            changed_by_rule["face_highlight"] += 1
            face_highlight_changes += 1
        elif root in paired_eye_roots and position[2] > eye_upper_cuts[root]:
            semantic_updated[index] = skin
            changed_by_source[primary] += 1
            changed_by_rule["eye_upper_white"] += 1
            eye_upper_changes += 1
    colors = semantic_updated

    recolored_vertices = sum(changed_by_source.values())
    if recolored_vertices:
        temporary = path.with_name(path.name + ".portrait-garment")
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
            raise TripoError("Portrait garment regions could not be stabilized safely.") from None
        finally:
            try:
                temporary.unlink(missing_ok=True)
            except OSError:
                pass

    report["status"] = "stabilized" if recolored_vertices else "not_needed"
    report["passes"] = pass_reports
    report["rear_threshold_x"] = round(rear_threshold, 6)
    report["structure_front_threshold_x"] = round(structure_front_threshold, 6)
    report["protected_skin_components"] = len(protected_skin_roots)
    report["protected_hand_components"] = len(protected_hand_roots)
    report["discarded_diffuse_skin_components"] = len(discarded_diffuse_skin_roots)
    report["discarded_diffuse_skin_vertices"] = sum(
        component_vertices[root] for root in discarded_diffuse_skin_roots
    )
    report["hand_compact_extent_ratio"] = PORTRAIT_HAND_COMPACT_EXTENT_RATIO
    report["hand_diffuse_size_ratio"] = PORTRAIT_HAND_DIFFUSE_SIZE_RATIO
    report["hand_minimum_height_ratio"] = PORTRAIT_HAND_MIN_HEIGHT_RATIO
    report["hand_boundary_passes"] = hand_pass_reports
    report["hand_boundary_max_removal_ratio"] = PORTRAIT_HAND_BOUNDARY_MAX_REMOVAL_RATIO
    report["forehead_highlight_components"] = len(forehead_highlight_roots)
    report["paired_eye_white_components"] = len(paired_eye_roots)
    report["face_highlight_recolored_vertices"] = face_highlight_changes
    report["eye_upper_recolored_vertices"] = eye_upper_changes
    report["protected_accent_components"] = len(protected_accent_roots)
    report["accent_candidate_components"] = len(accent_candidates)
    report["accent_anchor_height_ratio"] = round(accent_anchor_height_ratio, 6)
    report["minimum_accent_vertices"] = minimum_accent_vertices
    report["enclosed_accent_shadow_components"] = len(enclosed_accent_shadow_roots)
    report["maximum_accent_shadow_vertices"] = maximum_accent_shadow_vertices
    report["enclosed_accent_shadow_recolored_vertices"] = changed_by_rule["enclosed_accent_shadow"]
    report["recolored_vertices"] = recolored_vertices
    report["recolored_by_rule"] = dict(sorted(changed_by_rule.items()))
    report["recolored_by_source"] = {
        "#{:02X}{:02X}{:02X}".format(*color): count
        for color, count in sorted(changed_by_source.items())
    }
    _write_mesh_repair_report(report_path, report)
    return report

def _capture_portrait_front_face_details(
    path: Path,
    palette_roles: Mapping[str, str] | None,
) -> tuple[dict[int, tuple[int, int, int]], dict[str, Any]]:
    """Remember trusted textured details on the actually visible front face.

    The provider texture already aligns eyes, brows, mouth and teeth with the
    generated geometry. Generic printable-colour cleanup can erase those tiny
    but meaningful regions. Capturing the original vertex labels after mesh
    repair lets us restore the same vertices later without projecting a flat
    image through the sides of a three-dimensional head.
    """

    try:
        role_colors = {
            role: tuple(
                int(str((palette_roles or {})[role]).strip().upper()[index:index + 2], 16)
                for index in (1, 3, 5)
            )
            for role in ("primary", "structure", "light", "accent")
        }
    except (KeyError, ValueError):
        return {}, {"status": "not_applicable", "reason": "palette_roles_missing"}
    if len(set(role_colors.values())) != 4:
        return {}, {"status": "not_applicable", "reason": "palette_roles_ambiguous"}

    positions: list[tuple[float, float, float]] = []
    colors: list[tuple[int, int, int]] = []
    try:
        with path.open("r", encoding="utf-8", errors="strict") as stream:
            for line in stream:
                fields = line.strip().split()
                if not fields or fields[0].lower() != "v":
                    continue
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
    except (OSError, UnicodeDecodeError, ValueError):
        raise TripoError("The portrait face details could not be captured safely.") from None
    if not positions:
        raise TripoError("The generated OBJ has no vertices for portrait face-detail capture.")

    minimum_y = min(position[1] for position in positions)
    maximum_y = max(position[1] for position in positions)
    minimum_z = min(position[2] for position in positions)
    maximum_z = max(position[2] for position in positions)
    span_y = maximum_y - minimum_y
    span_z = maximum_z - minimum_z
    if span_y <= 1e-9 or span_z <= 1e-9:
        raise TripoError("The generated portrait has invalid dimensions for face-detail capture.")
    center_y = (minimum_y + maximum_y) * 0.5

    def face_cell(position: tuple[float, float, float]) -> tuple[int, int]:
        horizontal = round(
            (position[1] - minimum_y) / span_y * (PORTRAIT_FACE_DETAIL_GRID_SIZE - 1)
        )
        vertical = round(
            (position[2] - minimum_z) / span_z * (PORTRAIT_FACE_DETAIL_GRID_SIZE - 1)
        )
        return (
            max(0, min(PORTRAIT_FACE_DETAIL_GRID_SIZE - 1, horizontal)),
            max(0, min(PORTRAIT_FACE_DETAIL_GRID_SIZE - 1, vertical)),
        )

    front_surface: dict[tuple[int, int], float] = {}
    for position in positions:
        height_ratio = (position[2] - minimum_z) / span_z
        if (
            not PORTRAIT_FACE_DETAIL_MIN_HEIGHT_RATIO <= height_ratio < PORTRAIT_FACE_DETAIL_MAX_HEIGHT_RATIO
            or abs(position[1] - center_y) > span_y * PORTRAIT_FACE_DETAIL_HALF_WIDTH_RATIO
        ):
            continue
        cell = face_cell(position)
        front_surface[cell] = max(front_surface.get(cell, -math.inf), position[0])

    captured: dict[int, tuple[int, int, int]] = {}
    captured_by_target: Counter[tuple[int, int, int]] = Counter()
    for index, (position, color) in enumerate(zip(positions, colors)):
        if color not in {role_colors["structure"], role_colors["primary"]}:
            continue
        height_ratio = (position[2] - minimum_z) / span_z
        minimum_height = (
            PORTRAIT_FACE_DETAIL_MIN_HEIGHT_RATIO + 0.015
            if color == role_colors["structure"]
            else PORTRAIT_FACE_DETAIL_MIN_HEIGHT_RATIO
        )
        if (
            not minimum_height <= height_ratio < PORTRAIT_FACE_DETAIL_MAX_HEIGHT_RATIO
            or abs(position[1] - center_y) > span_y * PORTRAIT_FACE_DETAIL_HALF_WIDTH_RATIO
        ):
            continue
        maximum_front_x = front_surface.get(face_cell(position))
        if (
            maximum_front_x is None
            or position[0] < maximum_front_x - PORTRAIT_FACE_DETAIL_SURFACE_TOLERANCE_MM
        ):
            continue
        captured[index] = color
        captured_by_target[color] += 1

    report = {
        "status": "captured" if captured else "not_needed",
        "vertex_count": len(positions),
        "captured_vertices": len(captured),
        "captured_by_target": {
            "#{:02X}{:02X}{:02X}".format(*color): count
            for color, count in sorted(captured_by_target.items())
        },
        "minimum_height_ratio": PORTRAIT_FACE_DETAIL_MIN_HEIGHT_RATIO,
        "structure_minimum_height_ratio": PORTRAIT_FACE_DETAIL_MIN_HEIGHT_RATIO + 0.015,
        "maximum_height_ratio": PORTRAIT_FACE_DETAIL_MAX_HEIGHT_RATIO,
        "half_width_ratio": PORTRAIT_FACE_DETAIL_HALF_WIDTH_RATIO,
        "surface_tolerance_mm": PORTRAIT_FACE_DETAIL_SURFACE_TOLERANCE_MM,
        "surface_grid_size": PORTRAIT_FACE_DETAIL_GRID_SIZE,
    }
    return captured, report

def _restore_portrait_front_face_details(
    path: Path,
    report_path: Path,
    captured: Mapping[int, tuple[int, int, int]],
    capture_report: Mapping[str, Any],
    *,
    allowed_targets: set[tuple[int, int, int]] | None = None,
) -> dict[str, Any]:
    report = dict(capture_report)
    report["allowed_targets"] = (
        ["#{:02X}{:02X}{:02X}".format(*color) for color in sorted(allowed_targets)]
        if allowed_targets is not None else "all"
    )
    if not captured:
        report["recolored_vertices"] = 0
        _write_mesh_repair_report(report_path, report)
        return report

    changed_by_source: Counter[tuple[int, int, int]] = Counter()
    changed_by_target: Counter[tuple[int, int, int]] = Counter()
    vertex_index = 0
    temporary = path.with_name(path.name + ".face-details")
    try:
        with path.open("r", encoding="utf-8", errors="strict") as source, temporary.open(
            "w", encoding="ascii", newline="\n"
        ) as output:
            for line in source:
                fields = line.strip().split()
                if fields and fields[0].lower() == "v":
                    target = captured.get(vertex_index)
                    if target is not None and (
                        allowed_targets is None or target in allowed_targets
                    ):
                        current = tuple(round(float(value) * 255) for value in fields[4:7])
                        if current != target:
                            fields[4:7] = [f"{channel / 255.0:.6f}" for channel in target]
                            changed_by_source[current] += 1
                            changed_by_target[target] += 1
                        output.write(" ".join(fields) + "\n")
                    else:
                        output.write(line if line.endswith("\n") else line + "\n")
                    vertex_index += 1
                else:
                    output.write(line if line.endswith("\n") else line + "\n")
        if vertex_index != int(capture_report.get("vertex_count", -1)):
            raise TripoError("Portrait face details no longer match the repaired mesh.")
        os.replace(temporary, path)
    except (OSError, UnicodeDecodeError, ValueError):
        raise TripoError("The portrait face details could not be restored safely.") from None
    finally:
        try:
            temporary.unlink(missing_ok=True)
        except OSError:
            pass

    recolored_vertices = sum(changed_by_target.values())
    report["status"] = "restored" if recolored_vertices else "not_needed"
    report["recolored_vertices"] = recolored_vertices
    report["recolored_by_source"] = {
        "#{:02X}{:02X}{:02X}".format(*color): count
        for color, count in sorted(changed_by_source.items())
    }
    report["recolored_by_target"] = {
        "#{:02X}{:02X}{:02X}".format(*color): count
        for color, count in sorted(changed_by_target.items())
    }
    _write_mesh_repair_report(report_path, report)
    return report

def _review_portrait_rear_plate_masks(mask_directory: Path) -> dict[str, Any]:
    """Reject the long near-vertical silhouette left by an extruded backdrop."""

    try:
        from PIL import Image, UnidentifiedImageError
    except ImportError:
        raise PortraitGeometryGateError(
            "Pillow is required to inspect portrait geometry silhouettes."
        ) from None

    report: dict[str, Any] = {
        "version": "portrait-rear-plate-v1",
        "status": "pass",
        "views": {},
        "warnings": [],
    }
    try:
        for view in ("right", "left"):
            with Image.open(mask_directory / f"{view}.png") as opened:
                mask = opened.convert("L")
            bbox = mask.getbbox()
            if bbox is None:
                raise PortraitGeometryGateError(
                    f"The {view} portrait silhouette is empty."
                )
            left, top, right, bottom = bbox
            object_height = bottom - top
            analysis_bottom = min(bottom, top + int(round(object_height * 0.88)))
            tolerance = max(3, int(round(mask.width * 0.01)))
            rows: list[tuple[int, int, int]] = []
            pixels = mask.load()
            for y in range(top, analysis_bottom):
                occupied = [x for x in range(left, right) if pixels[x, y] >= 128]
                if occupied:
                    rows.append((y, occupied[0], occupied[-1]))

            view_report: dict[str, Any] = {
                "bbox": list(bbox),
                "tolerance_px": tolerance,
                "edges": {},
            }
            for edge_name, field_index in (("left", 1), ("right", 2)):
                best_run = 0
                best_center = 0
                best_start = top
                best_end = top
                centers = sorted({row[field_index] for row in rows})
                for center in centers:
                    run = 0
                    run_start = top
                    previous_y: int | None = None
                    for row in rows:
                        y = row[0]
                        matches = abs(row[field_index] - center) <= tolerance
                        if matches and (previous_y is None or y == previous_y + 1):
                            if run == 0:
                                run_start = y
                            run += 1
                        elif matches:
                            run = 1
                            run_start = y
                        else:
                            run = 0
                        previous_y = y
                        if run > best_run:
                            best_run = run
                            best_center = center
                            best_start = run_start
                            best_end = y
                run_ratio = best_run / max(1, object_height)
                start_ratio = (best_start - top) / max(1, object_height)
                suspicious = (
                    run_ratio >= PORTRAIT_REAR_PLATE_MIN_RUN_RATIO
                    and start_ratio <= PORTRAIT_REAR_PLATE_MAX_START_RATIO
                )
                view_report["edges"][edge_name] = {
                    "x": best_center,
                    "start_y": best_start,
                    "end_y": best_end,
                    "run_px": best_run,
                    "run_ratio": round(run_ratio, 6),
                    "start_ratio": round(start_ratio, 6),
                    "suspicious": suspicious,
                }
                if suspicious:
                    report["warnings"].append(f"{view}_{edge_name}_rear_plate")
            report["views"][view] = view_report
    except PortraitGeometryGateError:
        raise
    except (OSError, UnidentifiedImageError, Image.DecompressionBombError):
        raise PortraitGeometryGateError(
            "The portrait side silhouettes could not be inspected."
        ) from None
    if report["warnings"]:
        report["status"] = "reject"
    return report
