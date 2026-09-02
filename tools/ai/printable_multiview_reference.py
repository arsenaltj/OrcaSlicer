from __future__ import annotations

from collections import Counter
from dataclasses import asdict
import json
from pathlib import Path
from typing import Any, Callable, Mapping

from PIL import Image, ImageChops, ImageDraw, ImageFilter

try:
    from .printable_image_pipeline import PrintSettings, process_printable_image
    from .printable_palette import normalize_palette
except ImportError:
    from printable_image_pipeline import PrintSettings, process_printable_image
    from printable_palette import normalize_palette


SCHEMA_VERSION = 1
PORTRAIT_MATERIAL_GATE_VERSION = "portrait-material-v6"
MULTIVIEW_NORMALIZATION_VERSION = "multiview-normalization-v3"
# Tripo's portrait geometry is sensitive to the number of pixels available for
# the eyes, nose and mouth.  The generated four-view sheet can be smaller than
# the already-approved front reference, so quality mode normalizes every panel
# to a stable canvas without throwing away the locked front's source detail.
HIGH_QUALITY_PORTRAIT_CANVAS_SIZE = (1024, 1024)
VIEW_ORDER = ("front", "left", "back", "right")
VIEW_POSITIONS = {
    "front": "top-left",
    "left": "top-right",
    "back": "bottom-left",
    "right": "bottom-right",
}
CHECK_IDS = ("identity", "view_order", "geometry", "palette", "completeness")


class MultiviewReferenceError(RuntimeError):
    pass


def _rgb_from_hex(color: str) -> tuple[int, int, int]:
    value = color.lstrip("#")
    return tuple(int(value[offset : offset + 2], 16) for offset in (0, 2, 4))


def _remove_small_mask_components(mask: Image.Image) -> tuple[Image.Image, dict[str, int]]:
    """Drop detached segmentation dust while retaining substantial parts."""

    source = bytearray(mask.convert("L").tobytes())
    width, height = mask.size
    visited = bytearray(len(source))
    components: list[list[int]] = []
    for seed, alpha in enumerate(source):
        if alpha == 0 or visited[seed]:
            continue
        visited[seed] = 1
        stack = [seed]
        component: list[int] = []
        while stack:
            offset = stack.pop()
            component.append(offset)
            x = offset % width
            y = offset // width
            if x > 0:
                neighbor = offset - 1
                if source[neighbor] and not visited[neighbor]:
                    visited[neighbor] = 1
                    stack.append(neighbor)
            if x + 1 < width:
                neighbor = offset + 1
                if source[neighbor] and not visited[neighbor]:
                    visited[neighbor] = 1
                    stack.append(neighbor)
            if y > 0:
                neighbor = offset - width
                if source[neighbor] and not visited[neighbor]:
                    visited[neighbor] = 1
                    stack.append(neighbor)
            if y + 1 < height:
                neighbor = offset + width
                if source[neighbor] and not visited[neighbor]:
                    visited[neighbor] = 1
                    stack.append(neighbor)
        components.append(component)
    if not components:
        return Image.frombytes("L", mask.size, bytes(source)), {
            "component_count": 0,
            "removed_components": 0,
            "removed_pixels": 0,
            "largest_component_ratio_after": 0.0,
            "largest_detached_diagonal_ratio_after": 0.0,
            "subject_area_ratio_after": 0.0,
        }
    largest_area = max(len(component) for component in components)
    minimum_area = max(16, round(largest_area * 0.002))
    maximum_border_artifact_area = max(minimum_area, round(largest_area * 0.05))
    removed_components = 0
    removed_pixels = 0
    removed_border_components = 0
    removed_border_pixels = 0
    kept_components: list[list[int]] = []
    for component in components:
        touches_border = any(
            (offset % width) in {0, width - 1}
            or (offset // width) in {0, height - 1}
            for offset in component
        )
        border_artifact = touches_border and len(component) <= maximum_border_artifact_area
        if len(component) >= minimum_area and not border_artifact:
            kept_components.append(component)
            continue
        removed_components += 1
        removed_pixels += len(component)
        if border_artifact:
            removed_border_components += 1
            removed_border_pixels += len(component)
        for offset in component:
            source[offset] = 0
    subject_pixels = sum(len(component) for component in kept_components)
    largest_kept = max(kept_components, key=len, default=None)
    largest_component_ratio_after = (
        len(largest_kept) / subject_pixels
        if largest_kept is not None and subject_pixels
        else 0.0
    )

    def component_diagonal(component: list[int]) -> float:
        xs = [offset % width for offset in component]
        ys = [offset // width for offset in component]
        return ((max(xs) - min(xs) + 1) ** 2 + (max(ys) - min(ys) + 1) ** 2) ** 0.5

    if kept_components:
        all_offsets = [offset for component in kept_components for offset in component]
        subject_diagonal = component_diagonal(all_offsets)
        detached_diagonal = max(
            (component_diagonal(component) for component in kept_components if component is not largest_kept),
            default=0.0,
        )
        largest_detached_diagonal_ratio_after = (
            detached_diagonal / subject_diagonal if subject_diagonal else 0.0
        )
    else:
        largest_detached_diagonal_ratio_after = 0.0
    return Image.frombytes("L", mask.size, bytes(source)), {
        "component_count": len(components),
        "minimum_area": minimum_area,
        "removed_components": removed_components,
        "removed_pixels": removed_pixels,
        "removed_border_components": removed_border_components,
        "removed_border_pixels": removed_border_pixels,
        "largest_component_ratio_after": round(largest_component_ratio_after, 6),
        "largest_detached_diagonal_ratio_after": round(
            largest_detached_diagonal_ratio_after, 6
        ),
        "subject_area_ratio_after": round(subject_pixels / max(1, width * height), 6),
    }


def _remove_provider_chroma_key(
    source: Image.Image,
    subject_mask: Image.Image,
    palette: tuple[str, ...],
) -> tuple[Image.Image, dict[str, Any]]:
    """Remove chroma-key antialiasing and isolated key-colour pixels.

    Image2 normally follows the four-view prompt and emits a flat, saturated
    background that is deliberately far from the approved filament palette.
    Segmentation can still retain a one- or two-pixel antialiased rim. Tripo may
    turn that rim into geometry or texture, and palette projection can then map
    cyan to a green garment. Detect the dominant saturated border key and clear
    every retained pixel that is still closer to it than to any printable role.
    """

    source_rgba = source.convert("RGBA")
    mask = subject_mask.convert("L")
    width, height = source_rgba.size
    if width < 2 or height < 2 or mask.size != source_rgba.size:
        return mask, {"activated": 0, "removed_pixels": 0}

    rgb = source_rgba.convert("RGB")
    border = [rgb.getpixel((x, 0)) for x in range(width)]
    border.extend(rgb.getpixel((x, height - 1)) for x in range(width))
    border.extend(rgb.getpixel((0, y)) for y in range(1, height - 1))
    border.extend(rgb.getpixel((width - 1, y)) for y in range(1, height - 1))
    key, key_count = Counter(border).most_common(1)[0]
    dominance = key_count / max(1, len(border))
    chroma = max(key) - min(key)
    palette_rgb = tuple(_rgb_from_hex(color) for color in normalize_palette(palette))
    nearest_palette_distance = min(
        sum((channel - approved) ** 2 for channel, approved in zip(key, candidate)) ** 0.5
        for candidate in palette_rgb
    )
    # Image2 sometimes adds a gentle lightness gradient to the requested flat
    # key. The most-saturated endpoint is still repeated around the perimeter,
    # but may occupy only 5-15% of it; hue/distance checks below provide the
    # stronger guard against mistaking a normal photographic edge for the key.
    if dominance < 0.05 or chroma < 64 or nearest_palette_distance < 96:
        cleaned, component_cleanup = _remove_small_mask_components(mask)
        return cleaned, {
            "activated": 0,
            "removed_pixels": 0,
            "border_dominance": round(dominance, 6),
            "component_cleanup": component_cleanup,
        }

    distance_limit = min(170.0, nearest_palette_distance * 0.72)
    distance_limit_squared = distance_limit * distance_limit
    rgb_bytes = rgb.tobytes()
    mask_bytes = bytearray(mask.tobytes())
    removed = 0
    key_red, key_green, key_blue = key
    for offset, alpha in enumerate(mask_bytes):
        if alpha == 0:
            continue
        rgb_offset = offset * 3
        red = rgb_bytes[rgb_offset]
        green = rgb_bytes[rgb_offset + 1]
        blue = rgb_bytes[rgb_offset + 2]
        distance_squared = (
            (red - key_red) ** 2
            + (green - key_green) ** 2
            + (blue - key_blue) ** 2
        )
        if distance_squared <= distance_limit_squared:
            mask_bytes[offset] = 0
            removed += 1
    cleaned, component_cleanup = _remove_small_mask_components(
        Image.frombytes("L", mask.size, bytes(mask_bytes))
    )
    return cleaned, {
        "activated": 1,
        "key": "#%02X%02X%02X" % key,
        "border_dominance": round(dominance, 6),
        "nearest_palette_distance": round(nearest_palette_distance, 3),
        "distance_limit": round(distance_limit, 3),
        "removed_pixels": removed,
        "component_cleanup": component_cleanup,
    }


def evaluate_portrait_material_gate(
    metrics: Mapping[str, Any],
    palette_roles: Mapping[str, str],
    *,
    view_name: str = "",
) -> dict[str, Any]:
    """Apply a role-aware material gate to one portrait turntable view.

    A front view can legitimately split the primary garment into several large
    regions (two sleeves, torso and a light base).  The generic 2D fragmentation
    metric cannot distinguish those regions from speckles.  This stricter
    portrait gate therefore keeps skin and accent fragmentation blocking while
    allowing a small, already-cleaned primary-garment split when the silhouette
    remains coherent.
    """

    reasons: list[str] = []
    cleanup = metrics.get("portrait_skin_cleanup")
    cleanup = cleanup if isinstance(cleanup, Mapping) else {}
    normalized_roles = {
        str(role): str(color).upper()
        for role, color in palette_roles.items()
        if isinstance(role, str) and isinstance(color, str)
    }
    normalized_view = view_name.strip().lower()
    cleanup_active = cleanup.get("activated") == 1
    accepted_back_without_face_cleanup = normalized_view == "back" and not cleanup_active
    deferred_side_without_face_cleanup = (
        normalized_view in {"left", "right"} and not cleanup_active
    )
    # A true rear view normally contains no detectable face.  The cleanup
    # stage intentionally stays inactive there, so requiring activation would
    # reject every otherwise-correct back view.  This exception is limited to
    # the explicitly labelled back panel; all material fragmentation,
    # silhouette, detached-geometry and role checks below still apply.
    # Profile faces are intentionally allowed to reach the multimodal review:
    # the frontal face detector can miss a clean 60-90 degree view even when
    # its silhouette and materials are valid.  This is a deferral, not a pass;
    # the caller must still obtain a passing identity/material visual review.
    if not cleanup_active and not (
        accepted_back_without_face_cleanup or deferred_side_without_face_cleanup
    ):
        reasons.append("portrait_cleanup_not_active")
    if not deferred_side_without_face_cleanup:
        if str(cleanup.get("garment_color", "")).upper() != normalized_roles.get("primary", ""):
            reasons.append("garment_role_mismatch")
        if str(cleanup.get("skin_color", "")).upper() != normalized_roles.get("light", ""):
            reasons.append("skin_role_mismatch")
        cleanup_accent = str(cleanup.get("accent_color", "")).upper()
        if cleanup_accent and cleanup_accent != normalized_roles.get("accent", ""):
            reasons.append("accent_role_mismatch")

    meaningful_colors = metrics.get("meaningful_subject_color_count", 0)
    try:
        meaningful_colors = int(meaningful_colors)
    except (TypeError, ValueError):
        meaningful_colors = 0
    # A true rear portrait commonly exposes only the primary jacket plus
    # structural hair/base. Hidden skin and inner-garment colours must not be
    # invented merely to satisfy a per-view diversity metric. Front/profile
    # views still require at least three meaningful materials.
    minimum_meaningful_colors = 2 if normalized_view == "back" else 3
    diversity_ok = bool(metrics.get("palette_diversity_ok", False))
    if meaningful_colors < minimum_meaningful_colors or (
        normalized_view != "back" and not diversity_ok
    ):
        reasons.append("insufficient_material_colors")

    def numeric(name: str, fallback: float) -> float:
        value = metrics.get(name, fallback)
        try:
            return float(value)
        except (TypeError, ValueError):
            return fallback

    subject_area = numeric("printable_subject_area_ratio", 0.0)
    largest_component = numeric("largest_subject_component_ratio", 0.0)
    detached_diagonal = numeric("largest_detached_subject_diagonal_ratio", 1.0)
    if subject_area < 0.18:
        reasons.append("subject_too_small")
    if largest_component < 0.98:
        reasons.append("subject_is_disconnected")
    # Portrait hands, sleeve tips and base highlights can cross the generic
    # 0.08 cut-off by one or two pixels without representing detached geometry.
    if detached_diagonal > 0.10:
        reasons.append("large_detached_structure")

    severe_value = metrics.get("severe_fragmented_palette_roles", [])
    severe_roles = {
        str(role) for role in severe_value
        if isinstance(severe_value, (list, tuple, set)) and isinstance(role, str)
    }
    ratios = metrics.get("secondary_subject_color_component_ratio")
    ratios = ratios if isinstance(ratios, Mapping) else {}

    def secondary_ratio(role: str, fallback: float = 1.0) -> float:
        color = normalized_roles.get(role, "")
        try:
            return float(ratios.get(color, fallback))
        except (TypeError, ValueError):
            return fallback

    forbidden_roles = severe_roles.intersection({"light", "accent"})
    if forbidden_roles and not deferred_side_without_face_cleanup:
        reasons.append("skin_or_accent_is_fragmented")
    unexpected_roles = severe_roles.difference({"primary", "light", "accent"})
    if unexpected_roles:
        reasons.append("unknown_material_is_fragmented")

    # When a true profile cannot run the frontal cleanup, small disconnected
    # pieces can be legitimate: the green inner garment appears on both sides
    # of crossed arms and skin can be split by an occluding sleeve.  Keep hard
    # numeric ceilings, then require the visual reviewer to confirm that no
    # skin leaked onto clothing and no garment colour leaked onto face/hands.
    if deferred_side_without_face_cleanup:
        side_fragment_limits = {"primary": 0.12, "light": 0.03, "accent": 0.06}
        for role in severe_roles.intersection(side_fragment_limits):
            if secondary_ratio(role) > side_fragment_limits[role]:
                reasons.append(f"{role}_material_is_fragmented")

    accepted_primary_fragmentation = False
    if "primary" in severe_roles:
        primary_secondary_ratio = secondary_ratio("primary")
        small_region_ratio = numeric("small_region_ratio_after", 1.0)
        if deferred_side_without_face_cleanup:
            accepted_primary_fragmentation = (
                primary_secondary_ratio <= 0.12
                and small_region_ratio <= 0.005
                and largest_component >= 0.98
                and detached_diagonal <= 0.10
            )
        else:
            accepted_primary_fragmentation = (
                not forbidden_roles
                and not unexpected_roles
                and primary_secondary_ratio <= 0.04
                and small_region_ratio <= 0.005
                and largest_component >= 0.98
                and detached_diagonal <= 0.10
            )
        if not accepted_primary_fragmentation and "primary_material_is_fragmented" not in reasons:
            reasons.append("primary_material_is_fragmented")

    status = "reject" if reasons else (
        "review" if deferred_side_without_face_cleanup else "pass"
    )
    return {
        "version": PORTRAIT_MATERIAL_GATE_VERSION,
        "status": status,
        "accepted_primary_fragmentation": accepted_primary_fragmentation,
        "accepted_back_without_face_cleanup": accepted_back_without_face_cleanup and not reasons,
        "requires_visual_material_review": deferred_side_without_face_cleanup and not reasons,
        "minimum_meaningful_colors": minimum_meaningful_colors,
        "reasons": reasons,
    }


def evaluate_multiview_review_acceptance(review: Mapping[str, Any]) -> dict[str, Any]:
    """Accept harmless left/right naming uncertainty without accepting a swap."""

    try:
        score = int(review.get("score", 0))
    except (TypeError, ValueError):
        score = 0
    warnings_value = review.get("warnings", [])
    warnings = {
        str(value) for value in warnings_value
        if isinstance(warnings_value, (list, tuple, set)) and isinstance(value, str)
    }
    if review.get("status") == "pass" and score >= 85:
        return {"status": "pass", "accepted_view_order_ambiguity": False, "reason": "all_checks_passed"}

    checks = review.get("checks")
    checks = checks if isinstance(checks, Mapping) else {}
    core_checks_pass = all(
        isinstance(checks.get(check_id), Mapping)
        and checks[check_id].get("status") == "pass"
        for check_id in ("identity", "geometry", "palette")
    )
    view_check = checks.get("view_order")
    view_check = view_check if isinstance(view_check, Mapping) else {}
    reason = str(view_check.get("reason", ""))
    lower_reason = reason.lower()
    uncertainty_markers = (
        "无法完全确认", "仅凭", "左/右", "左右", "uncertain", "cannot confirm", "cannot fully confirm",
    )
    actual_error_markers = (
        "错误", "重复", "缺失", "顺序不符", "调换", "颠倒", "wrong", "duplicate", "missing", "swapped",
    )
    view_ambiguity_only = (
        "view_order" in warnings
        and score >= 88
        and any(marker in lower_reason for marker in uncertainty_markers)
        and not any(marker in lower_reason for marker in actual_error_markers)
    )
    completeness_check = checks.get("completeness")
    completeness_check = completeness_check if isinstance(completeness_check, Mapping) else {}
    completeness_reason = str(completeness_check.get("reason", ""))
    normalized_completeness_reason = completeness_reason.replace("未被明显裁切", "")
    completeness_uncertainty_markers = (
        "无法验证", "露出程度", "可见性", "遮挡", "occlusion", "visibility", "cannot verify",
    )
    completeness_error_markers = (
        "主体不完整", "实际缺失", "缺少视图", "重复视图", "明显裁切", "cropped", "missing view", "duplicate view",
    )
    completeness_ambiguity_only = (
        completeness_check.get("status") == "pass"
        or (
            "completeness" in warnings
            and any(marker in normalized_completeness_reason.lower() for marker in completeness_uncertainty_markers)
            and not any(marker in normalized_completeness_reason.lower() for marker in completeness_error_markers)
        )
    )
    ambiguity_only = (
        warnings.issubset({"view_order", "completeness"})
        and bool(warnings)
        and score >= 88
        and core_checks_pass
        and ("view_order" not in warnings or view_ambiguity_only)
        and completeness_ambiguity_only
    )
    return {
        "status": "pass" if ambiguity_only else "reject",
        "accepted_view_order_ambiguity": ambiguity_only,
        "reason": "occlusion_or_side_names_visually_ambiguous" if ambiguity_only else "review_failed",
    }


def build_multiview_sheet_prompt(description: str, palette: tuple[str, ...]) -> str:
    colors = normalize_palette(palette)
    if len(colors) != 4:
        raise MultiviewReferenceError("A multiview sheet requires exactly four printable colors.")
    return (
        "Create one precise 2-by-2 orthographic turntable reference sheet of the exact supplied finished collectible. "
        "First infer one single coherent solid 3D sculpture, then render that exact same unchanged sculpture four times; do not draw "
        "four independent interpretations. Do not redesign, simplify, add, remove, recolor, mirror, or change any part. Preserve "
        "identical identity, dimensions, proportions, pose, base, accessories, attachment points, material boundaries, and silhouette "
        "in every panel. Occluded parts must remain fixed in object space instead of moving to stay visible. The subject specification is: "
        + description.strip()
        + "\nPanel order is mandatory: top-left FRONT, top-right LEFT SIDE, bottom-left BACK, bottom-right RIGHT SIDE. "
        "Use exact camera yaw angles 0, +90, 180, and -90 degrees with zero pitch and zero roll. Every panel must show the complete "
        "subject at the same scale, centered, upright, level, and viewed with a true orthographic camera. Left and right describe the "
        "subject's own left and right sides. All protrusions must keep exactly the same depth, thickness, attachment and elevation in "
        "front, side and back views. Held tools must remain rigidly fused to the same hand with the same grip and orientation. Use the "
        "same transparent or plain neutral background "
        "and flat lighting in all four panels. Use only these four solid material colors: "
        + ", ".join(colors)
        + ". Do not add labels, text, arrows, borders, dividers, shadows, scenery, duplicate subjects, extra views, perspective drama, "
        "or cut-off geometry. Output exactly one 2-by-2 sheet."
    )


def split_multiview_sheet(sheet_path: Path | str, output_directory: Path | str) -> dict[str, Path]:
    source = Path(sheet_path)
    output = Path(output_directory)
    output.mkdir(parents=True, exist_ok=True)
    try:
        with Image.open(source) as opened:
            image = opened.convert("RGBA")
    except (OSError, ValueError):
        raise MultiviewReferenceError("The multiview sheet could not be read.") from None
    if image.width < 512 or image.height < 512:
        raise MultiviewReferenceError("The multiview sheet must be at least 512 by 512 pixels.")
    middle_x, middle_y = image.width // 2, image.height // 2
    boxes = {
        "front": (0, 0, middle_x, middle_y),
        "left": (middle_x, 0, image.width, middle_y),
        "back": (0, middle_y, middle_x, image.height),
        "right": (middle_x, middle_y, image.width, image.height),
    }
    result: dict[str, Path] = {}
    for view in VIEW_ORDER:
        destination = output / f"{view}-raw.png"
        image.crop(boxes[view]).save(destination)
        result[view] = destination
    return result


def process_multiview_crops(
    crops: Mapping[str, Path],
    output_directory: Path | str,
    palette: tuple[str, ...],
    settings: PrintSettings | None = None,
    *,
    palette_roles: Mapping[str, str] | None = None,
) -> tuple[dict[str, Path], dict[str, Path], dict[str, Any]]:
    if set(crops) != set(VIEW_ORDER):
        raise MultiviewReferenceError("All four named multiview crops are required.")
    output = Path(output_directory)
    references: dict[str, Path] = {}
    generation_references: dict[str, Path] = {}
    metrics: dict[str, Any] = {}
    for view in VIEW_ORDER:
        result = process_printable_image(
            crops[view],
            output / view,
            palette,
            settings or PrintSettings(),
            palette_roles=palette_roles,
        )
        # Material repair must sample the approved exact-palette image.  The
        # detail-rich model reference is appropriate for geometry generation,
        # but nearest-colour sampling its photographic shadows can reintroduce
        # black speckles into a green garment or skin into a light jacket.
        material_path = output / view / "material_reference.png"
        generation_path = output / view / "generation_reference.png"
        with (
            Image.open(crops[view]) as source,
            Image.open(result.subject_mask) as mask,
            Image.open(result.clean_preview) as material,
        ):
            source_rgba = source.convert("RGBA")
            subject_mask, chroma_cleanup = _remove_provider_chroma_key(
                source_rgba, mask, palette
            )
            # Transparent pixels must not retain the provider's chroma-key RGB.
            # Some image-to-3D services sample hidden RGB during edge filtering;
            # keeping cyan or magenta under alpha=0 can create a coloured plate
            # below the base even though a normal image viewer looks correct.
            generation_image = Image.new("RGBA", source_rgba.size, (0, 0, 0, 0))
            generation_image.paste(source_rgba, (0, 0), subject_mask)
            material_image = Image.new("RGBA", source_rgba.size, (0, 0, 0, 0))
            material_image.paste(material.convert("RGBA"), (0, 0), subject_mask)
            for destination, image in (
                (generation_path, generation_image),
                (material_path, material_image),
            ):
                temporary = destination.with_name(destination.name + ".part")
                image.save(temporary, format="PNG")
                temporary.replace(destination)
        references[view] = material_path
        generation_references[view] = generation_path
        metrics[view] = dict(result.metrics)
        metrics[view]["generation_chroma_cleanup"] = chroma_cleanup
        component_cleanup = chroma_cleanup.get("component_cleanup", {})
        if isinstance(component_cleanup, Mapping):
            metrics[view]["largest_subject_component_ratio"] = component_cleanup.get(
                "largest_component_ratio_after",
                metrics[view].get("largest_subject_component_ratio", 0.0),
            )
            metrics[view]["largest_detached_subject_diagonal_ratio"] = component_cleanup.get(
                "largest_detached_diagonal_ratio_after",
                metrics[view].get("largest_detached_subject_diagonal_ratio", 1.0),
            )
            metrics[view]["printable_subject_area_ratio"] = component_cleanup.get(
                "subject_area_ratio_after",
                metrics[view].get("printable_subject_area_ratio", 0.0),
            )
    return references, generation_references, metrics


def _fit_subject_to_canvas(
    source_path: Path,
    canvas_size: tuple[int, int],
    *,
    exact_palette: bool,
) -> tuple[Image.Image, dict[str, Any]]:
    try:
        with Image.open(source_path) as opened:
            source = opened.convert("RGBA")
    except (OSError, ValueError):
        raise MultiviewReferenceError("A multiview input could not be normalized.") from None
    alpha = source.getchannel("A")
    source_box = alpha.getbbox()
    if source_box is None:
        raise MultiviewReferenceError("A multiview input has no visible subject.")
    # Remove hidden provider RGB before resampling so no chroma key can bleed
    # into semi-transparent edge pixels.
    sanitized = Image.new("RGBA", source.size, (0, 0, 0, 0))
    sanitized.paste(source, (0, 0), alpha)
    subject = sanitized.crop(source_box)
    width, height = canvas_size
    maximum_width = max(1, round(width * 0.90))
    maximum_height = max(1, round(height * 0.90))
    scale = min(maximum_width / subject.width, maximum_height / subject.height)
    target_size = (
        max(1, round(subject.width * scale)),
        max(1, round(subject.height * scale)),
    )
    resample = Image.Resampling.NEAREST if exact_palette else Image.Resampling.LANCZOS
    resized = subject.resize(target_size, resample=resample)
    # Every view uses the same 5% bottom margin and vertical extent. This keeps
    # Tripo from interpreting inconsistent panel framing as extra base geometry.
    left = (width - target_size[0]) // 2
    top = height - round(height * 0.05) - target_size[1]
    top = max(1, top)
    canvas = Image.new("RGBA", canvas_size, (0, 0, 0, 0))
    canvas.alpha_composite(resized, (left, top))
    target_box = canvas.getchannel("A").getbbox()
    return canvas, {
        "source_size": list(source.size),
        "source_box": list(source_box),
        "target_size": list(canvas_size),
        "target_box": list(target_box) if target_box is not None else [],
    }


def _merge_locked_view_with_provider_silhouette(
    locked: Image.Image,
    provider: Image.Image,
    *,
    exact_palette: bool,
) -> tuple[Image.Image, dict[str, int]]:
    """Keep identity pixels while inheriting the provider's complete silhouette.

    White garments can be punched out of an otherwise strong identity reference
    when a foreground mask mistakes them for a light background. The reviewed
    turntable front already has a complete alpha silhouette. Use that alpha as
    authoritative, retain locked pixels safely inside its valid regions, and
    fill any mask holes from the provider front. This avoids choosing between a
    recognizable face and a watertight torso.
    """

    identity = locked.convert("RGBA")
    fallback = provider.convert("RGBA")
    if identity.size != fallback.size:
        raise MultiviewReferenceError("The locked and provider portrait fronts do not align.")
    identity_alpha = identity.getchannel("A").filter(ImageFilter.MinFilter(3))
    provider_alpha = fallback.getchannel("A")
    provider_box = provider_alpha.getbbox()
    if provider_box is None:
        raise MultiviewReferenceError("The reviewed provider front has no visible subject.")
    left, top, right, bottom = provider_box
    width = right - left
    height = bottom - top
    # Lock only the facial identity core. Hair silhouette, shoulders, garment
    # and base stay provider-consistent with the side/back views, and opaque
    # checkerboard pixels from an imperfect source mask cannot leak around the
    # ears or white jacket. A feathered edge avoids a visible facial patch in
    # the natural generation reference.
    face_zone = Image.new("L", identity.size, 0)
    ImageDraw.Draw(face_zone).ellipse(
        (
            round(left + width * 0.34),
            round(top + height * 0.08),
            round(left + width * 0.66),
            round(top + height * 0.29),
        ),
        fill=255,
    )
    if not exact_palette:
        face_zone = face_zone.filter(ImageFilter.GaussianBlur(max(1.0, width * 0.018)))
    identity_alpha = ImageChops.multiply(identity_alpha, face_zone)
    if exact_palette:
        identity_alpha = identity_alpha.point(lambda value: 255 if value >= 128 else 0)
    merged_rgb = Image.composite(
        identity.convert("RGB"), fallback.convert("RGB"), identity_alpha
    )
    merged = merged_rgb.convert("RGBA")
    merged.putalpha(provider_alpha)
    provider_bytes = provider_alpha.tobytes()
    identity_bytes = identity_alpha.tobytes()
    return merged, {
        "provider_subject_pixels": sum(value > 0 for value in provider_bytes),
        "identity_pixels_retained": sum(
            provider_value > 0 and identity_value >= 128
            for provider_value, identity_value in zip(provider_bytes, identity_bytes)
        ),
        "provider_hole_fill_pixels": sum(
            provider_value > 0 and identity_value < 128
            for provider_value, identity_value in zip(provider_bytes, identity_bytes)
        ),
    }


def normalize_multiview_inputs(
    references: Mapping[str, Path],
    generation_references: Mapping[str, Path],
    *,
    locked_front_material: Path | None = None,
    locked_front_generation: Path | None = None,
    target_canvas_size: tuple[int, int] | None = None,
) -> dict[str, Any]:
    """Normalize actual Tripo inputs and optionally source-lock the front view.

    Image generators commonly place one quadrant against its lower edge or
    change scale by a few percent. Normalizing after segmentation creates equal
    camera framing. For a real-person portrait the already-approved front image
    is a stronger identity reference than a newly hallucinated turntable front,
    while Image2 still supplies the unseen side and back geometry.
    """
    if set(references) != set(VIEW_ORDER) or set(generation_references) != set(VIEW_ORDER):
        raise MultiviewReferenceError("All four normalized multiview inputs are required.")
    if target_canvas_size is not None and (
        len(target_canvas_size) != 2 or any(int(edge) <= 0 for edge in target_canvas_size)
    ):
        raise MultiviewReferenceError("The multiview target canvas must have two positive edges.")
    normalized_canvas_size = (
        tuple(int(edge) for edge in target_canvas_size)
        if target_canvas_size is not None
        else None
    )
    report: dict[str, Any] = {
        "version": MULTIVIEW_NORMALIZATION_VERSION,
        "canvas_size": list(normalized_canvas_size) if normalized_canvas_size is not None else None,
        "views": {},
    }
    for view in VIEW_ORDER:
        generation_destination = Path(generation_references[view])
        material_destination = Path(references[view])
        if normalized_canvas_size is not None:
            canvas_size = normalized_canvas_size
        else:
            try:
                with Image.open(generation_destination) as opened:
                    canvas_size = opened.size
            except (OSError, ValueError):
                raise MultiviewReferenceError("A multiview generation input is unavailable.") from None
        generation_source = (
            Path(locked_front_generation)
            if view == "front" and locked_front_generation is not None
            else generation_destination
        )
        material_source = (
            Path(locked_front_material)
            if view == "front" and locked_front_material is not None
            else material_destination
        )
        provider_generation, _ = _fit_subject_to_canvas(
            generation_destination, canvas_size, exact_palette=False
        )
        provider_material, _ = _fit_subject_to_canvas(
            material_destination, canvas_size, exact_palette=True
        )
        generation_image, generation_report = _fit_subject_to_canvas(
            generation_source, canvas_size, exact_palette=False
        )
        material_image, material_report = _fit_subject_to_canvas(
            material_source, canvas_size, exact_palette=True
        )
        locked_composition: dict[str, Any] | None = None
        if view == "front" and locked_front_generation is not None:
            generation_image, generation_composition = _merge_locked_view_with_provider_silhouette(
                generation_image, provider_generation, exact_palette=False
            )
            locked_composition = {"generation": generation_composition}
            if locked_front_material is not None:
                material_image, material_composition = _merge_locked_view_with_provider_silhouette(
                    material_image, provider_material, exact_palette=True
                )
                locked_composition["material"] = material_composition
        for destination, image in (
            (generation_destination, generation_image),
            (material_destination, material_image),
        ):
            temporary = destination.with_name(destination.name + ".normalized.part")
            try:
                image.save(temporary, format="PNG")
                temporary.replace(destination)
            except OSError:
                try:
                    temporary.unlink(missing_ok=True)
                except OSError:
                    pass
                raise MultiviewReferenceError("A normalized multiview input could not be saved.") from None
        report["views"][view] = {
            "source_locked": view == "front" and locked_front_generation is not None,
            "generation": generation_report,
            "material": material_report,
            "locked_composition": locked_composition,
        }
    return report


def build_multiview_input_sheet(
    generation_references: Mapping[str, Path], destination: Path | str,
) -> Path:
    """Build the review sheet from the exact four files submitted to Tripo."""
    if set(generation_references) != set(VIEW_ORDER):
        raise MultiviewReferenceError("All four multiview inputs are required for review.")
    images: dict[str, Image.Image] = {}
    try:
        for view in VIEW_ORDER:
            with Image.open(generation_references[view]) as opened:
                images[view] = opened.convert("RGBA")
        width = max(image.width for image in images.values())
        height = max(image.height for image in images.values())
        sheet = Image.new("RGB", (width * 2, height * 2), (238, 240, 243))
        positions = {
            "front": (0, 0), "left": (width, 0),
            "back": (0, height), "right": (width, height),
        }
        for view in VIEW_ORDER:
            panel = Image.new("RGBA", (width, height), (238, 240, 243, 255))
            panel.alpha_composite(images[view], (0, 0))
            sheet.paste(panel.convert("RGB"), positions[view])
        output = Path(destination)
        temporary = output.with_name(output.name + ".part")
        sheet.save(temporary, format="PNG")
        temporary.replace(output)
        return output
    except (OSError, ValueError):
        raise MultiviewReferenceError("The normalized multiview review sheet could not be saved.") from None
    finally:
        for image in images.values():
            image.close()


def _review_system_prompt() -> str:
    return (
        "You review a source reference plus a 2-by-2 multiview reference sheet before paid 3D generation. If two images are "
        "provided, the first is the original source and the second is the sheet. The required sheet order is top-left front, "
        "top-right subject-left, bottom-left back, bottom-right subject-right. Judge whether all panels depict the same object with "
        "the same identity, proportions, pose, base, accessories, geometry and four material colors. A back view may reveal unseen "
        "surfaces but must not invent large structures. Orthographic FRONT in the sheet intentionally supersedes any three-quarter "
        "view wording in the source description; never flag that intended change. If the two side panels are clearly opposite views "
        "and geometrically consistent, do not flag them merely because subject-left versus camera-left naming is visually ambiguous. "
        "For a portrait, side and back panels must show genuine rounded skull, hair, shoulders and torso volume. A flat backing board, "
        "person-shaped wall, silhouette plate, photo cutout, rear sheet or straight vertical panel is a geometry failure, even if it "
        "touches the body or pedestal; mark geometry and completeness as review. Different surfaces may legitimately use different "
        "proportions of the same four colors; flag palette only for actual color "
        "substitution or changed material boundaries, not coverage ratios. Return exactly one JSON object without markdown: "
        '{"summary":"Chinese sentence","score":0,"confidence":0.0,"checks":{'
        '"identity":{"status":"pass|review","reason":"Chinese sentence"},'
        '"view_order":{"status":"pass|review","reason":"Chinese sentence"},'
        '"geometry":{"status":"pass|review","reason":"Chinese sentence"},'
        '"palette":{"status":"pass|review","reason":"Chinese sentence"},'
        '"completeness":{"status":"pass|review","reason":"Chinese sentence"}}}. '
        "Inspect every face, neck, hand, crossed arm and sleeve boundary closely. Flag palette when skin colour leaks onto a sleeve or "
        "jacket, garment colour leaks onto face/neck/hands, a coloured fringe remains around the silhouette, or a detached coloured "
        "patch/tab appears near or below the base. Natural occlusion is not incompleteness: a wristwatch, hand, lapel or front garment "
        "detail need not be visible from the back, "
        "and its visibility can legitimately differ between opposite side views. Flag an accessory only when two views that both expose "
        "it show conflicting geometry, not because a hidden view cannot verify it. Do not request labels for otherwise consistent opposite "
        "side views. Use review for any duplicated/missing view, actual orientation error, identity drift, changed accessory, changed base, "
        "large invented back geometry, crop, or material-color mismatch. Scores are integers 0 to 100."
    )


def _parse_review(response: str) -> dict[str, Any]:
    stripped = response.strip()
    if stripped.startswith("```"):
        lines = stripped.splitlines()
        if len(lines) >= 3 and lines[-1].strip() == "```":
            stripped = "\n".join(lines[1:-1]).strip()
    try:
        raw = json.loads(stripped)
    except json.JSONDecodeError:
        raise MultiviewReferenceError("The multiview review returned invalid JSON.") from None
    if not isinstance(raw, dict):
        raise MultiviewReferenceError("The multiview review did not return an object.")
    checks: dict[str, Any] = {}
    warnings: list[str] = []
    source_checks = raw.get("checks") if isinstance(raw.get("checks"), dict) else {}
    for check_id in CHECK_IDS:
        candidate = source_checks.get(check_id) if isinstance(source_checks.get(check_id), dict) else {}
        status = "review" if candidate.get("status") != "pass" else "pass"
        if status == "review":
            warnings.append(check_id)
        checks[check_id] = {"status": status, "reason": str(candidate.get("reason", ""))[:300]}
    score_value = raw.get("score", 0)
    score = int(max(0, min(100, score_value))) if isinstance(score_value, (int, float)) and not isinstance(score_value, bool) else 0
    confidence_value = raw.get("confidence", 0.0)
    confidence = float(max(0.0, min(1.0, confidence_value))) if isinstance(confidence_value, (int, float)) else 0.0
    return {
        "status": "review" if warnings or score < 80 else "pass",
        "score": score,
        "confidence": round(confidence, 3),
        "summary": str(raw.get("summary", ""))[:500],
        "warnings": warnings,
        "checks": checks,
    }


def review_multiview_sheet(
    sheet_path: Path | str,
    description: str,
    *,
    source_path: Path | str | None = None,
    completion: Callable[[str, str, tuple[Path, ...]], str],
) -> dict[str, Any]:
    images = (Path(sheet_path),) if source_path is None else (Path(source_path), Path(sheet_path))
    response = completion(
        _review_system_prompt(),
        "对象描述：" + description.strip() + "\n请核对四个面板是否可安全用于同一模型的多视图建模。",
        images,
    )
    return _parse_review(response)


def write_multiview_manifest(
    output_directory: Path | str,
    *,
    sheet: Path,
    references: Mapping[str, Path],
    generation_references: Mapping[str, Path] | None = None,
    metrics: Mapping[str, Any],
    review: Mapping[str, Any],
    palette: tuple[str, ...],
    settings: PrintSettings,
) -> Path:
    destination = Path(output_directory) / "multiview-reference.json"
    value = {
        "schema_version": SCHEMA_VERSION,
        "sheet": str(sheet.resolve()),
        "layout": VIEW_POSITIONS,
        "views": {view: str(references[view].resolve()) for view in VIEW_ORDER},
        "generation_views": {
            view: str((generation_references or references)[view].resolve())
            for view in VIEW_ORDER
        },
        "palette": list(normalize_palette(palette)),
        "print_settings": asdict(settings),
        "metrics": metrics,
        "review": dict(review),
    }
    temporary = destination.with_name(destination.name + ".part")
    temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2), encoding="utf-8")
    temporary.replace(destination)
    return destination


def create_multiview_sheet(
    source_path: Path | str,
    output_path: Path | str,
    description: str,
    palette: tuple[str, ...],
    *,
    editor: Callable[[Path | str, str, Path | str], Path],
) -> Path:
    return editor(source_path, build_multiview_sheet_prompt(description, palette), output_path)
