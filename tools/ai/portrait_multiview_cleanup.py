from __future__ import annotations

from array import array
from collections import Counter
import json
import math
import os
from pathlib import Path
from typing import Any, Mapping

from PIL import Image


class PortraitProjectionError(ValueError):
    pass


_GEOMETRY_VIEW_DIRECTIONS = {
    "front": (1.0, 0.0, 0.0),
    "right": (0.0, 1.0, 0.0),
    "back": (-1.0, 0.0, 0.0),
    "left": (0.0, -1.0, 0.0),
}


def _role_colors(palette_roles: Mapping[str, str]) -> dict[str, tuple[int, int, int]]:
    result: dict[str, tuple[int, int, int]] = {}
    for role in ("primary", "structure", "light", "accent"):
        value = str(palette_roles.get(role, "")).strip().upper()
        if len(value) != 7 or not value.startswith("#"):
            raise PortraitProjectionError("The portrait palette roles are incomplete.")
        try:
            result[role] = tuple(int(value[index:index + 2], 16) for index in (1, 3, 5))
        except ValueError:
            raise PortraitProjectionError("The portrait palette contains an invalid color.") from None
    if len(set(result.values())) != 4:
        raise PortraitProjectionError("The portrait palette roles must use four distinct colors.")
    return result


def _resolve_index(token: str, vertex_count: int) -> int:
    try:
        raw = int(token.split("/", 1)[0])
    except ValueError:
        raise PortraitProjectionError("The portrait OBJ has an invalid face index.") from None
    index = raw - 1 if raw > 0 else vertex_count + raw
    if raw == 0 or index < 0 or index >= vertex_count:
        raise PortraitProjectionError("The portrait OBJ has an out-of-range face index.")
    return index


def _write_report(path: Path, report: Mapping[str, Any]) -> None:
    temporary = path.with_name(path.name + ".part")
    try:
        temporary.write_text(json.dumps(dict(report), ensure_ascii=False, indent=2), encoding="utf-8")
        os.replace(temporary, path)
    except OSError:
        try:
            temporary.unlink(missing_ok=True)
        except OSError:
            pass
        raise PortraitProjectionError("The portrait projection report could not be written.") from None


def quantize_geometry_aligned_material_reference(
    source_path: Path | str,
    geometry_mask_path: Path | str,
    output_directory: Path | str,
    palette_roles: Mapping[str, str],
    *,
    view_name: str = "",
) -> dict[str, Any]:
    """Convert an AI semantic repaint into exact, geometry-aligned materials.

    The repaint is allowed to simplify surface appearance, but it never owns the
    silhouette.  The mask rendered from the exact OBJ therefore replaces any AI
    background or edge alpha before the labels are projected back to vertices.
    """

    colors_by_role = _role_colors(palette_roles)
    palette = tuple(colors_by_role[role] for role in ("primary", "structure", "light", "accent"))
    destination = Path(output_directory)
    try:
        with Image.open(source_path) as opened:
            source = opened.convert("RGB")
        with Image.open(geometry_mask_path) as opened_mask:
            mask = opened_mask.convert("L")
    except (OSError, ValueError):
        raise PortraitProjectionError("A geometry-aligned material reference could not be read.") from None
    if min(source.size) < 128:
        raise PortraitProjectionError("A geometry-aligned material reference is too small.")
    if mask.size != source.size:
        mask = mask.resize(source.size, Image.Resampling.NEAREST)
    if mask.getbbox() is None:
        raise PortraitProjectionError("The geometry-aligned material mask is empty.")

    if view_name and view_name not in _GEOMETRY_VIEW_DIRECTIONS:
        raise PortraitProjectionError("The geometry-aligned portrait view name is invalid.")

    primary = colors_by_role["primary"]
    quantized = Image.new("RGB", source.size, primary)
    source_pixels = source.load()
    mask_pixels = mask.load()
    destination_pixels = quantized.load()
    counts: Counter[tuple[int, int, int]] = Counter()
    for y in range(source.height):
        for x in range(source.width):
            if mask_pixels[x, y] <= 127:
                continue
            sample = source_pixels[x, y]
            target = min(
                palette,
                key=lambda color: sum(
                    (color[channel] - sample[channel]) ** 2 for channel in range(3)
                ),
            )
            destination_pixels[x, y] = target
            counts[target] += 1

    nape_repair: dict[str, Any] = {
        "activated": False,
        "recolored_pixels": 0,
        "reason": "semantic_internal_boundary_preserved",
    }
    # Do not infer the nape/collar boundary from a dark-hair silhouette alone.
    # On the real beta model that geometry-unaware heuristic repainted the
    # subject's raised jacket collar as skin.  Preserve the cleaned semantic
    # image here; downstream projection may only protect skin that the image
    # explicitly labels.

    aligned = quantized.convert("RGBA")
    aligned.putalpha(mask)
    try:
        destination.mkdir(parents=True, exist_ok=True)
        quantized.save(destination / "clean_preview.png", format="PNG", optimize=True)
        mask.save(destination / "mask_subject.png", format="PNG", optimize=True)
        aligned.save(destination / "aligned_reference.png", format="PNG", optimize=True)
    except OSError:
        raise PortraitProjectionError("The geometry-aligned material reference could not be saved.") from None
    return {
        "status": "prepared",
        "size": list(source.size),
        "subject_pixels": sum(counts.values()),
        "role_pixels": {
            role: counts[color] for role, color in colors_by_role.items()
        },
        "nape_repair": nape_repair,
        "clean_preview": "clean_preview.png",
        "mask_subject": "mask_subject.png",
        "aligned_reference": "aligned_reference.png",
    }


def project_front_portrait_materials(
    obj_path: Path | str,
    front_reference_path: Path | str,
    report_path: Path | str,
    palette_roles: Mapping[str, str],
    *,
    repair_skin: bool = True,
    restore_accent: bool = False,
    restore_face_details: bool = False,
    normalize_face_details: bool = False,
    reference_is_geometry_aligned: bool = False,
    clear_bright_face_materials_only: bool = False,
    sculptural_face_finish: bool = False,
    sculptural_face_relief_only: bool = False,
) -> dict[str, Any]:
    """Repair only material ownership that has strong portrait-specific evidence.

    The normal pass changes non-face skin vertices when the approved front image
    says they belong to clothing. An optional second pass restores front-centre
    garment vertices when the reference says they are the accent garment.  A
    final, separately gated pass can restore printable pupils and eyebrows from
    an exact-palette reference after generic
    colour-island cleanup.
    For realistic portraits, a stricter normalization mode removes erroneous
    white/dark facial islands and restores only high-confidence dark linework.
    Bright eye islands are deliberately collapsed into the continuous skin
    material. A geometry-aligned reference may retain one compact tooth band in
    the lower-face smile zone; everywhere else bright facial labels still read
    as white seams and are removed. After an exact four-view projection, callers
    may request an authoritative bright-only pass. A realistic four-filament
    portrait can additionally request a sculptural face finish.  Its relief-only
    variant keeps the complete visible face in one continuous skin material so
    the mesh itself carries the eyes, nose, smile and teeth while large,
    connected hair regions remain dark. This avoids doll eyes, a black mouth
    cavity and a plastic-white tooth block on a four-filament portrait.
    """

    path = Path(obj_path)
    reference_path = Path(front_reference_path)
    destination_report = Path(report_path)
    colors_by_role = _role_colors(palette_roles)
    skin = colors_by_role["light"]
    palette = tuple(colors_by_role.values())

    positions: list[tuple[float, float, float]] = []
    colors: list[tuple[int, int, int]] = []
    faces: list[tuple[int, int, int]] = []
    lines: list[str] = []
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
        for line in lines:
            fields = line.strip().split()
            if not fields or fields[0].startswith("#"):
                continue
            if fields[0].lower() == "v":
                if len(fields) not in {7, 8}:
                    raise PortraitProjectionError("The portrait OBJ has invalid vertex colors.")
                position = tuple(float(value) for value in fields[1:4])
                color = tuple(round(float(value) * 255) for value in fields[4:7])
                if not all(math.isfinite(value) for value in position) or not all(
                    0 <= value <= 255 for value in color
                ):
                    raise PortraitProjectionError("The portrait OBJ has an invalid colored vertex.")
                positions.append(position)
                colors.append(color)
            elif fields[0].lower() == "f":
                if len(fields) != 4:
                    raise PortraitProjectionError("The portrait OBJ must contain triangular faces.")
                faces.append(tuple(_resolve_index(value, len(positions)) for value in fields[1:]))
    except (OSError, UnicodeError, ValueError):
        raise PortraitProjectionError("The portrait OBJ could not be read.") from None
    if not positions or not faces:
        raise PortraitProjectionError("The portrait OBJ has no usable geometry.")

    try:
        with Image.open(reference_path) as opened:
            reference = opened.convert("RGBA")
    except (OSError, ValueError):
        raise PortraitProjectionError("The approved front portrait reference could not be read.") from None
    reference_box = reference.getchannel("A").getbbox()
    if reference_box is None:
        raise PortraitProjectionError("The approved front portrait reference has no subject mask.")

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

    for first, second, third in faces:
        for left, right in ((first, second), (second, third), (third, first)):
            if colors[left] == colors[right]:
                unite(left, right)

    minimum_z = min(position[2] for position in positions)
    maximum_z = max(position[2] for position in positions)
    minimum_y = min(position[1] for position in positions)
    maximum_y = max(position[1] for position in positions)
    span_z = maximum_z - minimum_z
    span_y = maximum_y - minimum_y
    if span_z <= 1e-9 or span_y <= 1e-9:
        raise PortraitProjectionError("The portrait geometry has invalid dimensions.")

    skin_components: Counter[int] = Counter(
        find(index) for index, color in enumerate(colors) if color == skin
    )
    structure_color = colors_by_role["structure"]
    structure_components: Counter[int] = Counter(
        find(index) for index, color in enumerate(colors) if color == structure_color
    )
    protected_structure_component_min_vertices = max(
        8,
        math.ceil(sum(structure_components.values()) * 0.03),
    )
    component_maximum_z: dict[int, float] = {}
    for index, color in enumerate(colors):
        if color != skin:
            continue
        root = find(index)
        component_maximum_z[root] = max(component_maximum_z.get(root, minimum_z), positions[index][2])
    face_candidates = [
        root for root in skin_components
        if (component_maximum_z[root] - minimum_z) / span_z >= 0.65
    ]
    face_root = max(face_candidates, key=lambda root: (skin_components[root], -root)) if face_candidates else None
    if repair_skin and face_root is None:
        report = {
            "status": "not_applicable",
            "reason": "face_skin_component_missing",
            "vertex_count": len(positions),
            "skin_vertex_count": sum(skin_components.values()),
            "recolored_vertices": 0,
        }
        _write_report(destination_report, report)
        return report

    box_width = reference_box[2] - reference_box[0]
    box_height = reference_box[3] - reference_box[1]
    scale = min((box_width - 1) / span_y, (box_height - 1) / span_z)
    image_center_x = (reference_box[0] + reference_box[2] - 1) * 0.5
    image_center_y = (reference_box[1] + reference_box[3] - 1) * 0.5
    model_center_y = (minimum_y + maximum_y) * 0.5
    model_center_z = (minimum_z + maximum_z) * 0.5

    updated = list(colors)
    changed_by_target: Counter[tuple[int, int, int]] = Counter()
    changed_by_source: Counter[tuple[int, int, int]] = Counter()
    kept_skin_vertices = 0
    protected_face_vertices = 0
    unmatched_vertices = 0
    accent_candidates = 0
    accent_recolored_vertices = 0
    accent_recolored_by_source: Counter[tuple[int, int, int]] = Counter()
    accent_front_threshold: float | None = None
    face_detail_candidates = 0
    face_detail_recolored_vertices = 0
    face_detail_recolored_by_target: Counter[tuple[int, int, int]] = Counter()
    face_detail_cleared_vertices = 0
    face_detail_restored_vertices = 0
    sculptural_eye_line_vertices = 0
    sculptural_tooth_vertices = 0
    face_occluded_vertices = 0
    face_front_threshold: float | None = None
    face_surface_tolerance: float | None = None
    radius = 4

    def reference_samples(
        position: tuple[float, float, float], sample_radius: int = radius,
    ) -> list[tuple[int, int, int]]:
        pixel_x = round(image_center_x + (position[1] - model_center_y) * scale)
        pixel_y = round(image_center_y - (position[2] - model_center_z) * scale)
        sampled: list[tuple[int, int, int]] = []
        for y in range(max(0, pixel_y - sample_radius), min(reference.height, pixel_y + sample_radius + 1)):
            for x in range(max(0, pixel_x - sample_radius), min(reference.width, pixel_x + sample_radius + 1)):
                red, green, blue, alpha = reference.getpixel((x, y))
                if alpha <= 127:
                    continue
                sampled.append(min(
                    palette,
                    key=lambda color: sum(
                        (color[channel] - value) ** 2
                        for channel, value in enumerate((red, green, blue))
                    ),
                ))
        return sampled

    def sample_reference(
        position: tuple[float, float, float], sample_radius: int = radius,
    ) -> tuple[tuple[int, int, int] | None, float]:
        sampled = reference_samples(position, sample_radius)
        if not sampled:
            return None, 0.0
        target, count = Counter(sampled).most_common(1)[0]
        return target, count / len(sampled)

    if repair_skin:
        for index, (position, source) in enumerate(zip(positions, colors)):
            if source != skin:
                continue
            if find(index) == face_root:
                protected_face_vertices += 1
                continue
            target, confidence = sample_reference(position)
            if target is None:
                target = colors_by_role["primary"]
                confidence = 1.0
                unmatched_vertices += 1
            if target == skin and confidence >= 0.50:
                kept_skin_vertices += 1
                continue
            if target != skin and confidence >= 0.55:
                updated[index] = target
                changed_by_target[target] += 1
                changed_by_source[source] += 1

    if restore_accent:
        primary = colors_by_role["primary"]
        structure = colors_by_role["structure"]
        accent = colors_by_role["accent"]
        body_x = sorted(
            position[0]
            for position in positions
            if 0.20 <= (position[2] - minimum_z) / span_z < 0.70
        )
        if body_x:
            accent_front_threshold = body_x[round((len(body_x) - 1) * 0.50)]
            for index, (position, source) in enumerate(zip(positions, colors)):
                height_ratio = (position[2] - minimum_z) / span_z
                if (
                    source not in {structure, primary}
                    or position[0] < accent_front_threshold
                    or abs(position[1] - model_center_y) > span_y * 0.28
                    or not 0.20 <= height_ratio < 0.70
                ):
                    continue
                accent_candidates += 1
                target, confidence = sample_reference(position)
                if target == accent and confidence >= 0.60:
                    updated[index] = accent
                    changed_by_target[accent] += 1
                    changed_by_source[source] += 1
                    accent_recolored_by_source[source] += 1
                    accent_recolored_vertices += 1

    if restore_face_details or normalize_face_details:
        primary = colors_by_role["primary"]
        structure = colors_by_role["structure"]
        accent = colors_by_role["accent"]
        face_grid_size = 256
        cleanup_upper_height = (
            0.89
            if sculptural_face_relief_only
            else 0.95 if reference_is_geometry_aligned else 0.89
        )

        def face_grid_cell(position: tuple[float, float, float]) -> tuple[int, int]:
            column = min(
                face_grid_size - 1,
                max(0, round((position[1] - minimum_y) / span_y * (face_grid_size - 1))),
            )
            row = min(
                face_grid_size - 1,
                max(0, round((position[2] - minimum_z) / span_z * (face_grid_size - 1))),
            )
            return column, row

        face_x = sorted(
            position[0]
            for position in positions
            if 0.68 <= (position[2] - minimum_z) / span_z < 0.97
        )
        if face_x:
            face_front_threshold = face_x[round((len(face_x) - 1) * 0.58)]
            # A planar image must only label the surface visible from the
            # front. A quantile alone still reaches recessed or rear layers at
            # some Y/Z positions, which can create black seams around cheeks
            # and ears in side views. Keep a fine front-depth map and allow a
            # small physical tolerance for adjacent vertices on the same
            # curved surface.
            face_surface_tolerance = max(span_y, span_z) * 0.0035
            front_x_by_cell: dict[tuple[int, int], float] = {}
            for position in positions:
                height_ratio = (position[2] - minimum_z) / span_z
                if not 0.68 <= height_ratio < max(0.93, cleanup_upper_height):
                    continue
                cell = face_grid_cell(position)
                front_x_by_cell[cell] = max(front_x_by_cell.get(cell, position[0]), position[0])
            for index, (position, source) in enumerate(zip(positions, colors)):
                height_ratio = (position[2] - minimum_z) / span_z
                detail_half_width = 0.11 if height_ratio < 0.75 else 0.20
                cleanup_half_width = 0.14 if height_ratio < 0.75 else 0.26
                within_detail_core = (
                    abs(position[1] - model_center_y) <= span_y * detail_half_width
                )
                cell_front_x = front_x_by_cell.get(face_grid_cell(position), position[0])
                aligned_eye_surface = (
                    reference_is_geometry_aligned
                    and within_detail_core
                    and 0.79 <= height_ratio <= 0.865
                )
                # Eyeballs and the upper-lid crease sit behind the cheek/brow
                # envelope even though they are visible from the front.  The
                # generic front-surface guard treated that legitimate recess
                # as occluded geometry and removed virtually every pupil from
                # the real beta portrait.  Relax depth only inside the aligned
                # eye band; the rest of the face keeps the strict seam guard.
                visible_face_threshold = face_front_threshold - (
                    span_z * 0.060 if aligned_eye_surface else 0.0
                )
                visible_surface_tolerance = face_surface_tolerance + (
                    span_z * 0.060 if aligned_eye_surface else 0.0
                )
                if (
                    position[0] < visible_face_threshold
                    or position[0] < cell_front_x - visible_surface_tolerance
                    or abs(position[1] - model_center_y) > span_y * (
                        cleanup_half_width if normalize_face_details else 0.23
                    )
                    or not (
                        0.68 <= height_ratio < cleanup_upper_height
                        if normalize_face_details else 0.70 <= height_ratio < 0.93
                    )
                    or (
                        normalize_face_details
                        and source not in (
                            {skin, structure, primary, accent}
                            if clear_bright_face_materials_only
                            else {skin, structure, primary}
                        )
                    )
                    or (not normalize_face_details and source != skin)
                ):
                    if (
                        position[0] >= visible_face_threshold
                        and position[0] < cell_front_x - visible_surface_tolerance
                    ):
                        face_occluded_vertices += 1
                    continue
                face_detail_candidates += 1
                aligned_eye_band = (
                    reference_is_geometry_aligned
                    and 0.79 <= height_ratio <= 0.84
                )
                target, confidence = sample_reference(
                    position,
                    2 if aligned_eye_band and not reference_is_geometry_aligned else 1,
                )
                detail_samples = reference_samples(position, 1)
                detail_counts = Counter(detail_samples)
                detail_total = max(1, len(detail_samples))
                centre_samples = reference_samples(position, 0)
                centre_target = centre_samples[0] if centre_samples else None
                dark_support = (
                    detail_counts[structure] + detail_counts[accent]
                ) / detail_total
                dark_pixel_count = (
                    detail_counts[structure] + detail_counts[accent]
                )
                skin_support = detail_counts[skin] / detail_total
                primary_support = detail_counts[primary] / detail_total
                within_dark_feature_zone = (
                    reference_is_geometry_aligned
                    and within_detail_core
                    and 0.79 <= height_ratio <= 0.89
                )
                dark_reference_evidence = (
                    within_dark_feature_zone
                    and centre_target in {structure, accent}
                    # The cleaned, geometry-aligned reference is already an
                    # exact four-colour ownership map.  Requiring two thirds
                    # of a 3x3 neighbourhood erased the subject's narrow
                    # pupils, upper lids and brows on the real 1.9M-face beta
                    # portrait: those features are commonly only one or two
                    # raster pixels thick after alignment.  Keep a centre hit
                    # plus one supporting neighbour instead.  The vertical
                    # face gate below still excludes the mouth and clothing,
                    # while the two-pixel requirement rejects isolated noise.
                    and dark_pixel_count >= 2
                )
                within_smile_core = (
                    reference_is_geometry_aligned
                    and abs(position[1] - model_center_y) <= span_y * 0.18
                    and 0.70 <= height_ratio <= 0.785
                )
                tooth_reference_evidence = (
                    within_smile_core
                    and (
                        centre_target == primary
                        or primary_support >= 0.45
                    )
                )
                authoritative_skin_evidence = (
                    reference_is_geometry_aligned
                    and centre_target == skin
                    and skin_support >= 0.66
                )
                if normalize_face_details:
                    normalized_target: tuple[int, int, int] | None = None
                    dark_minimum_confidence = 0.96 if aligned_eye_band else 0.90
                    usable_dark_reference_evidence = (
                        dark_reference_evidence
                        or (
                            not reference_is_geometry_aligned
                            and target == structure
                            and confidence >= dark_minimum_confidence
                            and within_detail_core
                        )
                    )
                    if sculptural_face_finish:
                        if (
                            source not in {skin, structure, primary, accent}
                            or not 0.70 <= height_ratio < 0.93
                        ):
                            continue
                        if (
                            source == structure
                            and structure_components[find(index)]
                            >= protected_structure_component_min_vertices
                        ):
                            # Hair and other intentional structural regions are
                            # large connected components. Eye sockets, mouth
                            # shadows and raster freckles are small isolated
                            # components on the visible face and are cleared.
                            continue
                        if sculptural_face_relief_only:
                            # A real validation portrait scored materially
                            # higher when its face used the same treatment as a
                            # traditional single-colour sculpture. At four
                            # printable colours, raster-black eyes and a white
                            # tooth strip visually overpower the underlying
                            # likeness and make an adult face look cartoonish.
                            # Keep the hair and other large structural regions
                            # above, but let genuine mesh relief describe every
                            # visible facial feature in continuous skin.
                            normalized_target = skin
                        else:
                            sculptural_eye_line = (
                                reference_is_geometry_aligned
                                and within_detail_core
                                and 0.795 <= height_ratio <= 0.865
                                and usable_dark_reference_evidence
                            )
                            sculptural_tooth = (
                                reference_is_geometry_aligned
                                and within_detail_core
                                and 0.72 <= height_ratio <= 0.78
                                and centre_target == primary
                                and primary_support >= 0.45
                            )
                            # Keep only reference-backed pupil, upper-lid and brow
                            # cores plus the compact centre of a real tooth band.
                            # The disjoint vertical gates cannot restore black mouth
                            # cavities, eye whites or broad cheek seams, while the
                            # narrow tooth strip preserves the subject's smile.
                            normalized_target = (
                                structure if sculptural_eye_line
                                else primary if sculptural_tooth
                                else skin
                            )
                            if sculptural_eye_line:
                                sculptural_eye_line_vertices += 1
                            elif sculptural_tooth:
                                sculptural_tooth_vertices += 1
                    elif clear_bright_face_materials_only:
                        if source == primary:
                            normalized_target = primary if tooth_reference_evidence else skin
                        elif (
                            usable_dark_reference_evidence
                        ):
                            # Exact-palette quantization can map brown eyebrows
                            # to the green garment accent because both are dark.
                            # On an aligned face, coherent dark evidence in the
                            # eye/brow band is identity linework, never blouse.
                            normalized_target = structure
                        elif tooth_reference_evidence and source == skin:
                            normalized_target = primary
                        elif (
                            source == structure
                            and reference_is_geometry_aligned
                            and within_detail_core
                            and 0.70 <= height_ratio <= 0.89
                        ):
                            # A black mouth or eye socket has a much larger
                            # perceptual weight than the same raster region in a
                            # photograph. Keep only reference-backed brow/pupil
                            # cores; unsupported central dark material becomes
                            # continuous skin so mesh relief carries the smile.
                            normalized_target = skin
                        elif source == structure and authoritative_skin_evidence:
                            # The final geometry-aligned pass is the source of
                            # truth. Clear unsupported dark freckles and eye
                            # sockets here so no generic island cleanup needs to
                            # run after restoring the intended thin linework.
                            normalized_target = skin
                        elif source == accent and 0.76 <= height_ratio <= 0.93:
                            normalized_target = skin
                        else:
                            continue
                    elif source == primary:
                        # Bright palette labels already present in provider
                        # textures become doll-like eye whites, cheek seams or
                        # an oversized tooth band. A continuous skin material
                        # is both more recognizable and more printable.
                        normalized_target = primary if tooth_reference_evidence else (
                            structure
                            if usable_dark_reference_evidence
                            else skin
                        )
                    elif (
                        source == structure
                        and aligned_eye_band
                        and not usable_dark_reference_evidence
                    ):
                        # Natural textures often paint the whole eye opening
                        # nearly black. Erode only that aligned eye band to the
                        # high-confidence pupil/lid core so it cannot read as a
                        # hollow socket in a four-filament print.
                        normalized_target = skin
                    elif tooth_reference_evidence and source == skin:
                        normalized_target = primary
                    elif target == skin:
                        minimum_confidence = 0.82 if source == structure else 0.60
                        if source != skin and confidence >= minimum_confidence:
                            normalized_target = skin
                    elif usable_dark_reference_evidence:
                        # Geometry-aligned facial linework requires the exact
                        # centre plus at least a two-pixel-wide local trace.
                        # Single raster pixels become heavy speckles on a dense
                        # 100 mm portrait mesh.
                        if source != structure:
                            normalized_target = structure
                    if normalized_target is None or normalized_target == source:
                        continue
                    updated[index] = normalized_target
                    changed_by_target[normalized_target] += 1
                    changed_by_source[source] += 1
                    face_detail_recolored_by_target[normalized_target] += 1
                    face_detail_recolored_vertices += 1
                    if normalized_target == skin:
                        face_detail_cleared_vertices += 1
                    else:
                        face_detail_restored_vertices += 1
                    continue
                printable_detail = (
                    target == structure and confidence >= 0.50
                ) or (
                    target == primary and height_ratio <= 0.86 and confidence >= 0.62
                )
                if not printable_detail or target is None:
                    continue
                updated[index] = target
                changed_by_target[target] += 1
                changed_by_source[source] += 1
                face_detail_recolored_by_target[target] += 1
                face_detail_recolored_vertices += 1

    recolored_vertices = sum(changed_by_target.values())
    if recolored_vertices:
        temporary = path.with_name(path.name + ".front-material-projection")
        vertex_index = 0
        try:
            with temporary.open("w", encoding="ascii", newline="\n") as output:
                for line in lines:
                    fields = line.strip().split()
                    if fields and fields[0].lower() == "v":
                        red, green, blue = updated[vertex_index]
                        fields[4:7] = [f"{channel / 255.0:.6f}" for channel in (red, green, blue)]
                        output.write(" ".join(fields) + "\n")
                        vertex_index += 1
                    else:
                        output.write(line + "\n")
            os.replace(temporary, path)
        except OSError:
            raise PortraitProjectionError("The portrait material projection could not be saved.") from None
        finally:
            try:
                temporary.unlink(missing_ok=True)
            except OSError:
                pass

    report = {
        "status": "projected" if recolored_vertices else "not_needed",
        "vertex_count": len(positions),
        "skin_vertex_count": sum(skin_components.values()),
        "protected_face_vertices": protected_face_vertices,
        "kept_reference_skin_vertices": kept_skin_vertices,
        "unmatched_vertices": unmatched_vertices,
        "repair_skin": repair_skin,
        "restore_accent": restore_accent,
        "restore_face_details": restore_face_details,
        "normalize_face_details": normalize_face_details,
        "reference_is_geometry_aligned": reference_is_geometry_aligned,
        "clear_bright_face_materials_only": clear_bright_face_materials_only,
        "sculptural_face_finish": sculptural_face_finish,
        "sculptural_face_relief_only": sculptural_face_relief_only,
        "protected_structure_component_min_vertices": protected_structure_component_min_vertices,
        "accent_candidates": accent_candidates,
        "accent_recolored_vertices": accent_recolored_vertices,
        "accent_recolored_by_source": {
            "#{:02X}{:02X}{:02X}".format(*color): count
            for color, count in sorted(accent_recolored_by_source.items())
        },
        "accent_front_threshold_x": (
            round(accent_front_threshold, 6) if accent_front_threshold is not None else None
        ),
        "face_detail_candidates": face_detail_candidates,
        "face_detail_recolored_vertices": face_detail_recolored_vertices,
        "face_detail_cleared_vertices": face_detail_cleared_vertices,
        "face_detail_restored_vertices": face_detail_restored_vertices,
        "sculptural_eye_line_vertices": sculptural_eye_line_vertices,
        "sculptural_tooth_vertices": sculptural_tooth_vertices,
        "face_occluded_vertices": face_occluded_vertices,
        "face_detail_recolored_by_target": {
            "#{:02X}{:02X}{:02X}".format(*color): count
            for color, count in sorted(face_detail_recolored_by_target.items())
        },
        "face_front_threshold_x": (
            round(face_front_threshold, 6) if face_front_threshold is not None else None
        ),
        "face_surface_tolerance": (
            round(face_surface_tolerance, 6) if face_surface_tolerance is not None else None
        ),
        "recolored_vertices": recolored_vertices,
        "recolored_by_target": {
            "#{:02X}{:02X}{:02X}".format(*color): count
            for color, count in sorted(changed_by_target.items())
        },
        "recolored_by_source": {
            "#{:02X}{:02X}{:02X}".format(*color): count
            for color, count in sorted(changed_by_source.items())
        },
        "reference_box": list(reference_box),
        "sample_radius": radius,
    }
    _write_report(destination_report, report)
    return report


def project_geometry_aligned_portrait_materials(
    obj_path: Path | str,
    view_directories: Mapping[str, Path | str],
    report_path: Path | str,
    palette_roles: Mapping[str, str],
    *,
    margin_ratio: float = 0.06,
) -> dict[str, Any]:
    """Project exact-mesh turntable material labels back onto visible vertices.

    Each directory must contain a geometry-aligned ``clean_preview.png`` and
    ``mask_subject.png`` produced from the same orthographic render. A compact
    vertex z-buffer prevents a front label from repainting an occluded rear
    surface; agreeing front/side/rear evidence resolves material ownership at
    sleeves, hands, the neck and the base without changing geometry.
    """

    if not 0.0 <= margin_ratio < 0.4:
        raise PortraitProjectionError("The geometry-aligned portrait margin is invalid.")
    requested_views = [
        view for view in _GEOMETRY_VIEW_DIRECTIONS if view in view_directories
    ]
    if len(requested_views) < 2:
        raise PortraitProjectionError("At least two geometry-aligned portrait views are required.")

    path = Path(obj_path)
    destination_report = Path(report_path)
    role_colors = _role_colors(palette_roles)
    palette = tuple(role_colors[role] for role in ("primary", "structure", "light", "accent"))
    color_to_index = {color: index for index, color in enumerate(palette)}
    positions: list[tuple[float, float, float]] = []
    colors: list[tuple[int, int, int]] = []
    faces: list[tuple[int, int, int]] = []
    lines: list[str] = []
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
        for line in lines:
            fields = line.strip().split()
            if not fields or fields[0].startswith("#"):
                continue
            if fields[0].lower() == "v":
                if len(fields) not in {7, 8}:
                    raise PortraitProjectionError("The portrait OBJ has invalid vertex colors.")
                position = tuple(float(value) for value in fields[1:4])
                color = tuple(round(float(value) * 255) for value in fields[4:7])
                if not all(math.isfinite(value) for value in position) or not all(
                    0 <= value <= 255 for value in color
                ):
                    raise PortraitProjectionError("The portrait OBJ has an invalid colored vertex.")
                positions.append(position)
                colors.append(color)
            elif fields[0].lower() == "f":
                if len(fields) != 4:
                    raise PortraitProjectionError("The portrait OBJ must contain triangular faces.")
                faces.append(tuple(_resolve_index(value, len(positions)) for value in fields[1:]))
    except (OSError, UnicodeError, ValueError):
        raise PortraitProjectionError("The portrait OBJ could not be read.") from None
    if not positions or not faces:
        raise PortraitProjectionError("The portrait OBJ has no usable geometry.")

    model_minimum_x = min(position[0] for position in positions)
    model_maximum_x = max(position[0] for position in positions)
    model_minimum_y = min(position[1] for position in positions)
    model_maximum_y = max(position[1] for position in positions)
    model_minimum_z = min(position[2] for position in positions)
    model_maximum_z = max(position[2] for position in positions)
    model_span_x = max(model_maximum_x - model_minimum_x, 1e-9)
    model_span_y = max(model_maximum_y - model_minimum_y, 1e-9)
    model_span_z = max(model_maximum_z - model_minimum_z, 1e-9)
    model_center_y = (model_minimum_y + model_maximum_y) * 0.5
    accent_index = color_to_index[role_colors["accent"]]
    structure_index = color_to_index[role_colors["structure"]]
    light_index = color_to_index[role_colors["light"]]
    accent_minimum_height_ratio = 0.08

    def dot(left: tuple[float, float, float], right: tuple[float, float, float]) -> float:
        return sum(left[index] * right[index] for index in range(3))

    def cross(
        left: tuple[float, float, float], right: tuple[float, float, float],
    ) -> tuple[float, float, float]:
        return (
            left[1] * right[2] - left[2] * right[1],
            left[2] * right[0] - left[0] * right[2],
            left[0] * right[1] - left[1] * right[0],
        )

    def normalized(vector: tuple[float, float, float]) -> tuple[float, float, float]:
        length = math.sqrt(dot(vector, vector))
        if length <= 1e-12:
            raise PortraitProjectionError("A geometry-aligned portrait view is invalid.")
        return tuple(value / length for value in vector)

    votes = array("f", [0.0]) * (len(positions) * len(palette))
    evidence_counts = array("B", [0]) * len(positions)
    strong_skin_evidence = array("B", [0]) * len(positions)
    authoritative_front_accent = array("B", [0]) * len(positions)
    authoritative_back_skin = array("B", [0]) * len(positions)
    authoritative_side_skin = array("B", [0]) * len(positions)
    per_view: dict[str, Any] = {}
    opened_images: list[Image.Image] = []
    try:
        for view in requested_views:
            directory = Path(view_directories[view])
            try:
                reference = Image.open(directory / "clean_preview.png").convert("RGB")
                subject_mask = Image.open(directory / "mask_subject.png").convert("L")
            except (OSError, ValueError):
                raise PortraitProjectionError(
                    f"The geometry-aligned {view} portrait reference could not be read."
                ) from None
            opened_images.extend((reference, subject_mask))
            if reference.size != subject_mask.size or min(reference.size) < 128:
                raise PortraitProjectionError("A geometry-aligned portrait reference has invalid dimensions.")
            width, height = reference.size
            camera = normalized(_GEOMETRY_VIEW_DIRECTIONS[view])
            right = normalized(cross((0.0, 0.0, 1.0), camera))
            up = normalized(cross(camera, right))
            minimum_x = minimum_y = minimum_depth = math.inf
            maximum_x = maximum_y = maximum_depth = -math.inf
            for position in positions:
                projected_x = dot(position, right)
                projected_y = dot(position, up)
                depth = dot(position, camera)
                minimum_x, maximum_x = min(minimum_x, projected_x), max(maximum_x, projected_x)
                minimum_y, maximum_y = min(minimum_y, projected_y), max(maximum_y, projected_y)
                minimum_depth, maximum_depth = min(minimum_depth, depth), max(maximum_depth, depth)
            span_x = max(maximum_x - minimum_x, 1e-9)
            span_y = max(maximum_y - minimum_y, 1e-9)
            scale = min(
                width * (1.0 - 2.0 * margin_ratio) / span_x,
                height * (1.0 - 2.0 * margin_ratio) / span_y,
            )
            center_x = (minimum_x + maximum_x) * 0.5
            center_y = (minimum_y + maximum_y) * 0.5
            screen_x = array("h")
            screen_y = array("h")
            screen_depth = array("f")
            # Pool a tiny 2x2 screen cell when resolving visibility. Exact
            # per-pixel sampling exposes isolated semantic-image errors as
            # salt-and-pepper material speckles on million-vertex meshes;
            # this conservative vote is visibly more stable on clothing.
            grid_step = 2
            grid_width = (width + grid_step - 1) // grid_step
            grid_height = (height + grid_step - 1) // grid_step
            depth_grid = array("f", [-3.4e38]) * (grid_width * grid_height)
            for position in positions:
                pixel_x = round(width * 0.5 + (dot(position, right) - center_x) * scale)
                pixel_y = round(height * 0.5 - (dot(position, up) - center_y) * scale)
                depth = dot(position, camera)
                screen_x.append(max(-32768, min(32767, pixel_x)))
                screen_y.append(max(-32768, min(32767, pixel_y)))
                screen_depth.append(depth)
                if 0 <= pixel_x < width and 0 <= pixel_y < height:
                    cell = (pixel_y // grid_step) * grid_width + pixel_x // grid_step
                    if depth > depth_grid[cell]:
                        depth_grid[cell] = depth

            depth_tolerance = max(1e-4, (maximum_depth - minimum_depth) * 0.006)
            reference_pixels = reference.load()
            mask_pixels = subject_mask.load()
            visible_vertices = sampled_vertices = 0
            target_counts: Counter[int] = Counter()
            for index, (pixel_x, pixel_y, depth) in enumerate(
                zip(screen_x, screen_y, screen_depth)
            ):
                if not (0 <= pixel_x < width and 0 <= pixel_y < height):
                    continue
                cell = (pixel_y // grid_step) * grid_width + pixel_x // grid_step
                if depth < depth_grid[cell] - depth_tolerance:
                    continue
                visible_vertices += 1
                counts = [0] * len(palette)
                samples = 0
                for sample_y in range(max(0, pixel_y - 1), min(height, pixel_y + 2)):
                    for sample_x in range(max(0, pixel_x - 1), min(width, pixel_x + 2)):
                        if mask_pixels[sample_x, sample_y] <= 127:
                            continue
                        sample = reference_pixels[sample_x, sample_y]
                        target = min(
                            range(len(palette)),
                            key=lambda item: sum(
                                (palette[item][channel] - sample[channel]) ** 2
                                for channel in range(3)
                            ),
                        )
                        counts[target] += 1
                        samples += 1
                if not samples:
                    continue
                target = max(range(len(palette)), key=lambda item: (counts[item], -item))
                confidence = counts[target] / samples
                if confidence < 0.55:
                    continue
                global_height_ratio = (
                    positions[index][2] - model_minimum_z
                ) / model_span_z
                if target == accent_index and (
                    view != "front"
                    or not accent_minimum_height_ratio <= global_height_ratio < 0.70
                    or abs(positions[index][1] - model_center_y) > model_span_y * 0.30
                ):
                    # The secondary garment is front-centre.  Letting a side
                    # repaint introduce this role is what previously made a
                    # green wrist, green mouth and green pedestal rim.
                    continue
                if view == "front" and target == accent_index and confidence >= 0.66:
                    authoritative_front_accent[index] = 1
                if target == light_index and confidence >= 0.78:
                    strong_skin_evidence[index] = min(
                        255, strong_skin_evidence[index] + 1
                    )
                    if (
                        view == "back"
                        and 0.40 <= global_height_ratio <= 0.62
                        and abs(positions[index][1] - model_center_y)
                        <= model_span_y * 0.18
                    ):
                        # The cleaned back reference explicitly separates the
                        # exposed nape from the white standing collar.  Keep
                        # that high-confidence central skin label authoritative
                        # through the generic rear-jacket cleanup below.
                        authoritative_back_skin[index] = 1
                    if (
                        view in {"right", "left"}
                        and 0.40 <= global_height_ratio <= 0.68
                    ):
                        # A side view is the only projection that can
                        # unambiguously see the exposed strip of neck between
                        # the hairline and a raised rear collar.  Preserve that
                        # exact skin ownership even when the back view's white
                        # collar vote ties it.
                        authoritative_side_skin[index] = 1
                weight = confidence * (1.2 if view == "front" else 1.0)
                # Printable brows, pupils, smile line and teeth need the exact
                # front view to win a side-view skin tie without widening them.
                height_ratio = (
                    positions[index][2] - model_minimum_z
                ) / model_span_z
                if view == "front" and height_ratio >= 0.68 and target in {
                    color_to_index[role_colors["structure"]],
                    color_to_index[role_colors["primary"]],
                }:
                    weight *= 1.6
                votes[index * len(palette) + target] += weight
                evidence_counts[index] = min(255, evidence_counts[index] + 1)
                sampled_vertices += 1
                target_counts[target] += 1
            per_view[view] = {
                "visible_vertices": visible_vertices,
                "sampled_vertices": sampled_vertices,
                "depth_tolerance": round(depth_tolerance, 6),
                "target_counts": {
                    "#{:02X}{:02X}{:02X}".format(*palette[index]): target_counts[index]
                    for index in sorted(target_counts)
                },
            }
    finally:
        for opened in opened_images:
            opened.close()

    updated = list(colors)
    for index, source in enumerate(colors):
        start = index * len(palette)
        vertex_votes = votes[start:start + len(palette)]
        total = sum(vertex_votes)
        if total < 0.55:
            continue
        target_index = max(range(len(palette)), key=lambda item: (vertex_votes[item], -item))
        if vertex_votes[target_index] < total * 0.50:
            continue
        target = palette[target_index]
        if target == source:
            continue
        updated[index] = target

    authoritative_side_skin_recolored = 0
    for index, has_side_skin in enumerate(authoritative_side_skin):
        if has_side_skin and updated[index] != role_colors["light"]:
            updated[index] = role_colors["light"]
            authoritative_side_skin_recolored += 1

    # Axis-aligned views cannot see every upward-facing crown vertex.  The old
    # central-head fallback consequently preserved warm texture pixels on dark
    # hair as isolated scalp patches.  Infer the crown material from the
    # visible ring immediately below it: this still keeps a genuinely bald
    # head when the semantic views vote for skin, while a clearly dark-haired
    # portrait defaults unseen crown vertices to the structure material.
    crown_structure_votes = 0.0
    crown_skin_votes = 0.0
    for index, position in enumerate(positions):
        height_ratio = (position[2] - model_minimum_z) / model_span_z
        if not 0.84 <= height_ratio <= 0.94:
            continue
        start = index * len(palette)
        crown_structure_votes += votes[start + structure_index]
        crown_skin_votes += votes[start + light_index]
    dark_hair_crown = (
        crown_structure_votes >= 3.0
        and crown_structure_votes >= crown_skin_votes * 1.35
    )

    # Natural portrait textures are excellent geometry evidence but their warm
    # ivory garment shadows can quantize to skin. Keep projected skin only when
    # at least one exact-mesh view saw a strong skin-majority neighborhood, or
    # when the vertex is in the central upper head region. This preserves both
    # crossed hands while removing long peach seams on white sleeves and the
    # pedestal. Unlike the old connected-component garment pass, the decision
    # is based on per-vertex semantic evidence and cannot erase a hand merely
    # because it touches or overlaps a sleeve in projection.
    skin = role_colors["light"]
    skin_guard_recolored = 0
    skin_guard_targets: Counter[tuple[int, int, int]] = Counter()
    crown_hair_guard_recolored = 0
    for index, target in enumerate(updated):
        if target != skin:
            continue
        height_ratio = (
            positions[index][2] - model_minimum_z
        ) / model_span_z
        central_face = (
            0.70 <= height_ratio < 0.91
            and abs(positions[index][1] - model_center_y) <= model_span_y * 0.28
        )
        if strong_skin_evidence[index]:
            continue
        if central_face:
            continue
        start = index * len(palette)
        fallback_index = max(
            (item for item in range(len(palette)) if item != light_index),
            key=lambda item: (votes[start + item], -item),
        )
        crown_hair_vertex = dark_hair_crown and height_ratio >= 0.91
        if crown_hair_vertex or height_ratio <= 0.14:
            fallback_index = structure_index
        elif votes[start + fallback_index] <= 0.0:
            fallback_index = 0
        fallback = palette[fallback_index]
        updated[index] = fallback
        skin_guard_recolored += 1
        skin_guard_targets[fallback] += 1
        if crown_hair_vertex:
            crown_hair_guard_recolored += 1

    # The skin-specific pass above cannot catch white or accent pixels that a
    # semantic image hallucinated along the top silhouette. Once the visible
    # crown ring has established dark hair, the very top of the head must be
    # one coherent structure material. Keep the threshold above the forehead
    # so real face skin is not flattened into the hair color.
    if dark_hair_crown:
        for index, target in enumerate(updated):
            height_ratio = (
                positions[index][2] - model_minimum_z
            ) / model_span_z
            if height_ratio < 0.93 or target == role_colors["structure"]:
                continue
            updated[index] = role_colors["structure"]
            crown_hair_guard_recolored += 1

    # Keep accent only inside the narrow front-centre blouse volume. Semantic
    # views can themselves hallucinate green onto a touching wrist/watch, so
    # direct pixel evidence alone is not sufficient ownership evidence. A
    # connected-component-only rule has the same failure mode because the real
    # blouse can drag a thin green tail across a touching hand or white sleeve.
    accent = role_colors["accent"]
    torso_x = sorted(
        position[0]
        for position in positions
        if accent_minimum_height_ratio
        <= (position[2] - model_minimum_z) / model_span_z < 0.70
    )
    front_threshold = (
        torso_x[round((len(torso_x) - 1) * 0.50)]
        if torso_x else model_minimum_x + model_span_x * 0.50
    )
    accent_spatial_guard_recolored = 0
    accent_spatial_guard_targets: Counter[tuple[int, int, int]] = Counter()
    authoritative_front_accent_recolored = 0
    for index, has_front_accent in enumerate(authoritative_front_accent):
        if not has_front_accent:
            continue
        height_ratio = (
            positions[index][2] - model_minimum_z
        ) / model_span_z
        inside_blouse_core = (
            accent_minimum_height_ratio <= height_ratio < 0.70
            and positions[index][0] >= front_threshold - model_span_x * 0.05
            and abs(positions[index][1] - model_center_y) <= model_span_y * 0.18
        )
        if inside_blouse_core and updated[index] != accent:
            updated[index] = accent
            authoritative_front_accent_recolored += 1
    for index, target in enumerate(updated):
        if target != accent:
            continue
        start = index * len(palette)
        height_ratio = (
            positions[index][2] - model_minimum_z
        ) / model_span_z
        inside_blouse_core = (
            accent_minimum_height_ratio <= height_ratio < 0.70
            and positions[index][0] >= front_threshold - model_span_x * 0.05
            and abs(positions[index][1] - model_center_y) <= model_span_y * 0.18
        )
        if inside_blouse_core:
            continue
        fallback_index = max(
            (item for item in range(len(palette)) if item != accent_index),
            key=lambda item: (votes[start + item], -item),
        )
        if height_ratio <= 0.14:
            fallback_index = structure_index
        elif votes[start + fallback_index] <= 0.0:
            fallback_index = 0
        fallback = palette[fallback_index]
        updated[index] = fallback
        accent_spatial_guard_recolored += 1
        accent_spatial_guard_targets[fallback] += 1

    def color_components(target_color: tuple[int, int, int]) -> list[list[int]]:
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

        for first, second, third in faces:
            for left, right in ((first, second), (second, third), (third, first)):
                if updated[left] == target_color and updated[right] == target_color:
                    unite(left, right)
        grouped: dict[int, list[int]] = {}
        for index, color in enumerate(updated):
            if color == target_color:
                grouped.setdefault(find(index), []).append(index)
        return list(grouped.values())

    # Keep only coherent front-centre accent components.  This second guard is
    # required for small disconnected vertices that still retained a bad green
    # label after the per-vertex ownership pass.
    accent_components = color_components(accent)
    minimum_accent_vertices = max(8, int(len(positions) * 0.00002))
    protected_accent_components: set[int] = set()
    for component_index, indices in enumerate(accent_components):
        count = len(indices)
        centroid_x = sum(positions[index][0] for index in indices) / count
        centroid_y = sum(positions[index][1] for index in indices) / count
        centroid_height = sum(
            (positions[index][2] - model_minimum_z) / model_span_z for index in indices
        ) / count
        if (
            count >= minimum_accent_vertices
            and centroid_x >= front_threshold - model_span_x * 0.05
            and abs(centroid_y - model_center_y) <= model_span_y * 0.18
            and accent_minimum_height_ratio <= centroid_height < 0.70
        ):
            protected_accent_components.add(component_index)

    role_guard_recolored = 0
    role_guard_targets: Counter[tuple[int, int, int]] = Counter()
    for component_index, indices in enumerate(accent_components):
        if component_index in protected_accent_components:
            continue
        component_scores = [0.0] * len(palette)
        for index in indices:
            start = index * len(palette)
            for target_index in range(len(palette)):
                if target_index != accent_index:
                    component_scores[target_index] += votes[start + target_index]
        target_index = max(
            (index for index in range(len(palette)) if index != accent_index),
            key=lambda index: (component_scores[index], -index),
        )
        if component_scores[target_index] <= 0.0:
            centroid_height = sum(
                (positions[index][2] - model_minimum_z) / model_span_z for index in indices
            ) / len(indices)
            target_index = (
                structure_index if centroid_height <= 0.14
                else light_index if centroid_height >= 0.70
                else 0
            )
        target = palette[target_index]
        for index in indices:
            updated[index] = target
        role_guard_recolored += len(indices)
        role_guard_targets[target] += len(indices)

    # A bust's rear torso is the outer garment. Exact semantic side views can
    # still scatter skin/accent labels across jacket folds, especially when a
    # crossed arm occludes the front. Keep the upper neck available, but make
    # the rear jacket below it one deterministic material region.
    rear_garment_guard_recolored = 0
    protected_upper_neck_skin_vertices = 0
    for index, target in enumerate(updated):
        height_ratio = (positions[index][2] - model_minimum_z) / model_span_z
        protected_upper_neck_skin = (
            target == skin
            and strong_skin_evidence[index]
            and 0.40 <= height_ratio <= 0.68
        )
        if protected_upper_neck_skin:
            protected_upper_neck_skin_vertices += 1
        if not (
            0.14 < height_ratio < 0.63
            and positions[index][0] < front_threshold - model_span_x * 0.05
            and target != role_colors["primary"]
            and not authoritative_back_skin[index]
            and not protected_upper_neck_skin
        ):
            continue
        updated[index] = role_colors["primary"]
        rear_garment_guard_recolored += 1

    # In the torso band, legitimate skin must form a sizeable front-side hand
    # or wrist region. Small disconnected skin islands are semantic projection
    # noise, not anatomy. Face, ears and the upper neck remain outside this
    # cleanup band.
    torso_skin_guard_recolored = 0
    minimum_skin_region_vertices = max(3, int(len(positions) * 0.00010))
    for indices in color_components(skin):
        torso_indices = [
            index for index in indices
            if 0.14 < (positions[index][2] - model_minimum_z) / model_span_z < 0.63
            and not authoritative_back_skin[index]
            and not (
                strong_skin_evidence[index]
                and 0.40
                <= (positions[index][2] - model_minimum_z) / model_span_z
                <= 0.68
            )
        ]
        if not torso_indices:
            continue
        centroid_height = sum(
            (positions[index][2] - model_minimum_z) / model_span_z
            for index in torso_indices
        ) / len(torso_indices)
        front_ratio = sum(
            positions[index][0] >= front_threshold - model_span_x * 0.03
            for index in torso_indices
        ) / len(torso_indices)
        if (
            len(torso_indices) >= minimum_skin_region_vertices
            and front_ratio >= 0.60
            and 0.27 <= centroid_height <= 0.60
        ):
            continue
        for index in torso_indices:
            updated[index] = role_colors["primary"]
        torso_skin_guard_recolored += len(torso_indices)

    # Preserve a real watch or deliberate dark seam, but collapse tiny dark
    # freckles on the jacket. Inside the blouse volume they belong to the
    # accent material; elsewhere they belong to the outer garment.
    torso_structure_guard_recolored = 0
    minimum_structure_region_vertices = max(16, int(len(positions) * 0.00005))
    for indices in color_components(role_colors["structure"]):
        torso_indices = [
            index for index in indices
            if 0.14 < (positions[index][2] - model_minimum_z) / model_span_z < 0.63
        ]
        if not torso_indices or len(torso_indices) >= minimum_structure_region_vertices:
            continue
        for index in torso_indices:
            height_ratio = (positions[index][2] - model_minimum_z) / model_span_z
            inside_blouse_core = (
                positions[index][0] >= front_threshold - model_span_x * 0.05
                and abs(positions[index][1] - model_center_y) <= model_span_y * 0.18
                and accent_minimum_height_ratio <= height_ratio < 0.70
            )
            updated[index] = accent if inside_blouse_core else role_colors["primary"]
        torso_structure_guard_recolored += len(torso_indices)

    changed_by_source: Counter[tuple[int, int, int]] = Counter()
    changed_by_target: Counter[tuple[int, int, int]] = Counter()
    for source, target in zip(colors, updated):
        if source == target:
            continue
        changed_by_source[source] += 1
        changed_by_target[target] += 1

    recolored_vertices = sum(changed_by_target.values())
    if recolored_vertices:
        temporary = path.with_name(path.name + ".multiview-material-projection")
        vertex_index = 0
        try:
            with temporary.open("w", encoding="ascii", newline="\n") as output:
                for line in lines:
                    fields = line.strip().split()
                    if fields and fields[0].lower() == "v":
                        red, green, blue = updated[vertex_index]
                        fields[4:7] = [f"{channel / 255.0:.6f}" for channel in (red, green, blue)]
                        output.write(" ".join(fields) + "\n")
                        vertex_index += 1
                    else:
                        output.write(line + "\n")
            os.replace(temporary, path)
        except OSError:
            raise PortraitProjectionError("The multiview portrait material projection could not be saved.") from None
        finally:
            try:
                temporary.unlink(missing_ok=True)
            except OSError:
                pass

    report = {
        "status": "projected" if recolored_vertices else "not_needed",
        "version": "geometry-material-v2",
        "vertex_count": len(positions),
        "face_count": len(faces),
        "views": per_view,
        "vertices_with_evidence": sum(value > 0 for value in evidence_counts),
        "recolored_vertices": recolored_vertices,
        "recolored_by_source": {
            "#{:02X}{:02X}{:02X}".format(*color): count
            for color, count in sorted(changed_by_source.items())
        },
        "recolored_by_target": {
            "#{:02X}{:02X}{:02X}".format(*color): count
            for color, count in sorted(changed_by_target.items())
        },
        "protected_accent_components": len(protected_accent_components),
        "removed_accent_components": len(accent_components) - len(protected_accent_components),
        "role_guard_recolored_vertices": role_guard_recolored,
        "role_guard_recolored_by_target": {
            "#{:02X}{:02X}{:02X}".format(*color): count
            for color, count in sorted(role_guard_targets.items())
        },
        "skin_guard_recolored_vertices": skin_guard_recolored,
        "skin_guard_recolored_by_target": {
            "#{:02X}{:02X}{:02X}".format(*color): count
            for color, count in sorted(skin_guard_targets.items())
        },
        "crown_material_votes": {
            "structure": round(crown_structure_votes, 4),
            "skin": round(crown_skin_votes, 4),
        },
        "dark_hair_crown": dark_hair_crown,
        "crown_hair_guard_recolored_vertices": crown_hair_guard_recolored,
        "accent_spatial_guard_recolored_vertices": accent_spatial_guard_recolored,
        "authoritative_front_accent_recolored_vertices": authoritative_front_accent_recolored,
        "accent_minimum_height_ratio": accent_minimum_height_ratio,
        "accent_spatial_guard_recolored_by_target": {
            "#{:02X}{:02X}{:02X}".format(*color): count
            for color, count in sorted(accent_spatial_guard_targets.items())
        },
        "rear_garment_guard_recolored_vertices": rear_garment_guard_recolored,
        "protected_upper_neck_skin_vertices": protected_upper_neck_skin_vertices,
        "torso_skin_guard_recolored_vertices": torso_skin_guard_recolored,
        "torso_structure_guard_recolored_vertices": torso_structure_guard_recolored,
        "strong_skin_evidence_vertices": sum(
            value > 0 for value in strong_skin_evidence
        ),
        "authoritative_back_skin_vertices": sum(
            value > 0 for value in authoritative_back_skin
        ),
        "authoritative_side_skin_vertices": sum(
            value > 0 for value in authoritative_side_skin
        ),
        "authoritative_side_skin_recolored_vertices": authoritative_side_skin_recolored,
        "margin_ratio": margin_ratio,
    }
    _write_report(destination_report, report)
    return report
