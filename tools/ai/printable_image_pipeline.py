#!/usr/bin/env python3
"""Deterministic fixed-palette image processing for printable color regions."""

from __future__ import annotations

from collections import Counter, deque
from dataclasses import asdict, dataclass
import json
import math
import os
from pathlib import Path
from typing import Any, Iterable, Mapping

try:
    from .printable_palette import (
        MAX_PRINTABLE_COLORS,
        PrintablePaletteError,
        assign_palette_roles,
        normalize_palette as normalize_printable_palette,
    )
except ImportError:
    from printable_palette import (
        MAX_PRINTABLE_COLORS,
        PrintablePaletteError,
        assign_palette_roles,
        normalize_palette as normalize_printable_palette,
    )


MAX_IMAGE_PIXELS = 64 * 1024 * 1024
MAX_PALETTE_COLORS = MAX_PRINTABLE_COLORS


class PrintableImageError(RuntimeError):
    pass


@dataclass(frozen=True)
class PrintSettings:
    width_mm: float = 160.0
    nozzle_mm: float = 0.4
    line_width_mm: float = 0.4
    minimum_feature_mm: float = 0.8
    color_distance: str = "ciede2000"
    print_mode: str = "solid_regions"
    shadow_color: str = "blue"

    @classmethod
    def from_mapping(cls, value: Any) -> "PrintSettings":
        if value is None:
            return cls()
        if not isinstance(value, dict):
            raise PrintableImageError("print settings must be an object")
        allowed = {
            "width_mm", "nozzle_mm", "line_width_mm", "minimum_feature_mm",
            "color_distance", "print_mode", "shadow_color",
        }
        if set(value) - allowed:
            raise PrintableImageError("print settings contain unsupported fields")
        try:
            settings = cls(
                width_mm=float(value.get("width_mm", cls.width_mm)),
                nozzle_mm=float(value.get("nozzle_mm", cls.nozzle_mm)),
                line_width_mm=float(value.get("line_width_mm", cls.line_width_mm)),
                minimum_feature_mm=float(value.get("minimum_feature_mm", cls.minimum_feature_mm)),
                color_distance=str(value.get("color_distance", cls.color_distance)),
                print_mode=str(value.get("print_mode", cls.print_mode)),
                shadow_color=str(value.get("shadow_color", cls.shadow_color)),
            )
        except (TypeError, ValueError):
            raise PrintableImageError("print settings contain invalid numeric values") from None
        if not 20.0 <= settings.width_mm <= 2000.0:
            raise PrintableImageError("print width must be between 20 and 2000 mm")
        if not 0.1 <= settings.nozzle_mm <= 2.0:
            raise PrintableImageError("nozzle diameter must be between 0.1 and 2.0 mm")
        if not 0.1 <= settings.line_width_mm <= 3.0:
            raise PrintableImageError("line width must be between 0.1 and 3.0 mm")
        if not settings.line_width_mm <= settings.minimum_feature_mm <= 20.0:
            raise PrintableImageError("minimum feature must be at least one line width and no more than 20 mm")
        if settings.color_distance not in {"ciede2000", "delta_e76"}:
            raise PrintableImageError("unsupported color distance")
        if settings.print_mode != "solid_regions":
            raise PrintableImageError("only solid_regions print mode is currently supported")
        if settings.shadow_color not in {"red", "green", "blue", "white"}:
            raise PrintableImageError("unsupported shadow color")
        return settings


@dataclass(frozen=True)
class PipelineResult:
    strict_preview: Path
    clean_preview: Path
    model_reference: Path
    heatmap: Path
    background_mask: Path
    subject_mask: Path
    masks: dict[str, Path]
    metadata: Path
    metrics: dict[str, Any]
    palette_usage: dict[str, int]


def normalize_palette(colors: Iterable[str]) -> tuple[str, ...]:
    try:
        return normalize_printable_palette(colors)
    except PrintablePaletteError as exc:
        raise PrintableImageError(str(exc)) from None


def _hex_rgb(color: str) -> tuple[int, int, int]:
    return tuple(int(color[index:index + 2], 16) for index in (1, 3, 5))  # type: ignore[return-value]


def _is_bright_neutral(color: tuple[int, int, int]) -> bool:
    # Light gray studio backgrounds and white toy materials should use the brightest filament. Without this guard,
    # perceptual distance can send medium gray to a blue shadow filament and create large false background islands.
    return max(color) - min(color) <= 24 and sum(color) / 3 >= 120


def _srgb_to_lab(color: tuple[int, int, int]) -> tuple[float, float, float]:
    values = []
    for channel in color:
        value = channel / 255.0
        values.append(value / 12.92 if value <= 0.04045 else ((value + 0.055) / 1.055) ** 2.4)
    red, green, blue = values
    x = (red * 0.4124564 + green * 0.3575761 + blue * 0.1804375) / 0.95047
    y = red * 0.2126729 + green * 0.7151522 + blue * 0.0721750
    z = (red * 0.0193339 + green * 0.1191920 + blue * 0.9503041) / 1.08883

    def pivot(value: float) -> float:
        return value ** (1.0 / 3.0) if value > 216.0 / 24389.0 else (24389.0 / 27.0 * value + 16.0) / 116.0

    fx, fy, fz = pivot(x), pivot(y), pivot(z)
    return 116.0 * fy - 16.0, 500.0 * (fx - fy), 200.0 * (fy - fz)


def _delta_e76(left: tuple[float, float, float], right: tuple[float, float, float]) -> float:
    return math.sqrt(sum((a - b) ** 2 for a, b in zip(left, right)))


def _ciede2000(left: tuple[float, float, float], right: tuple[float, float, float]) -> float:
    l1, a1, b1 = left
    l2, a2, b2 = right
    c1, c2 = math.hypot(a1, b1), math.hypot(a2, b2)
    mean_c = (c1 + c2) / 2.0
    g = 0.5 * (1.0 - math.sqrt(mean_c ** 7 / (mean_c ** 7 + 25.0 ** 7)))
    ap1, ap2 = (1.0 + g) * a1, (1.0 + g) * a2
    cp1, cp2 = math.hypot(ap1, b1), math.hypot(ap2, b2)

    def hue(ap: float, b: float) -> float:
        angle = math.degrees(math.atan2(b, ap))
        return angle + 360.0 if angle < 0.0 else angle

    hp1, hp2 = hue(ap1, b1), hue(ap2, b2)
    dl, dc = l2 - l1, cp2 - cp1
    dh = hp2 - hp1
    if cp1 * cp2 == 0.0:
        dh = 0.0
    elif dh > 180.0:
        dh -= 360.0
    elif dh < -180.0:
        dh += 360.0
    d_h = 2.0 * math.sqrt(cp1 * cp2) * math.sin(math.radians(dh / 2.0))
    mean_l, mean_cp = (l1 + l2) / 2.0, (cp1 + cp2) / 2.0
    if cp1 * cp2 == 0.0:
        mean_hp = hp1 + hp2
    elif abs(hp1 - hp2) <= 180.0:
        mean_hp = (hp1 + hp2) / 2.0
    elif hp1 + hp2 < 360.0:
        mean_hp = (hp1 + hp2 + 360.0) / 2.0
    else:
        mean_hp = (hp1 + hp2 - 360.0) / 2.0
    t = (
        1.0 - 0.17 * math.cos(math.radians(mean_hp - 30.0))
        + 0.24 * math.cos(math.radians(2.0 * mean_hp))
        + 0.32 * math.cos(math.radians(3.0 * mean_hp + 6.0))
        - 0.20 * math.cos(math.radians(4.0 * mean_hp - 63.0))
    )
    sl = 1.0 + 0.015 * (mean_l - 50.0) ** 2 / math.sqrt(20.0 + (mean_l - 50.0) ** 2)
    sc = 1.0 + 0.045 * mean_cp
    sh = 1.0 + 0.015 * mean_cp * t
    rotation = 30.0 * math.exp(-((mean_hp - 275.0) / 25.0) ** 2)
    rc = 2.0 * math.sqrt(mean_cp ** 7 / (mean_cp ** 7 + 25.0 ** 7))
    rt = -math.sin(math.radians(2.0 * rotation)) * rc
    return math.sqrt((dl / sl) ** 2 + (dc / sc) ** 2 + (d_h / sh) ** 2 + rt * (dc / sc) * (d_h / sh))


def _distance(left: tuple[float, float, float], right: tuple[float, float, float], method: str) -> float:
    return _ciede2000(left, right) if method == "ciede2000" else _delta_e76(left, right)


def _atomic_png(image: Any, path: Path) -> None:
    temporary = path.with_name(path.name + ".part")
    image.save(temporary, format="PNG", optimize=True)
    os.replace(temporary, path)


def _atomic_json(payload: dict[str, Any], path: Path) -> None:
    temporary = path.with_name(path.name + ".part")
    temporary.write_text(json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8")
    os.replace(temporary, path)


def _palette_roles(palette: tuple[str, ...], overrides: Mapping[str, str] | None = None) -> list[str]:
    try:
        assignment = assign_palette_roles(palette, overrides)
    except PrintablePaletteError as exc:
        raise PrintableImageError(str(exc)) from None
    return [assignment.role_by_color[color] for color in palette]


def _boundary_background(indices: bytes, width: int, height: int, background_index: int) -> bytes:
    mask = bytearray(width * height)
    pending: deque[int] = deque()

    def enqueue(offset: int) -> None:
        if indices[offset] == background_index and not mask[offset]:
            mask[offset] = 255
            pending.append(offset)

    for x in range(width):
        enqueue(x)
        enqueue((height - 1) * width + x)
    for y in range(height):
        enqueue(y * width)
        enqueue(y * width + width - 1)
    while pending:
        offset = pending.popleft()
        x = offset % width
        if x:
            enqueue(offset - 1)
        if x + 1 < width:
            enqueue(offset + 1)
        if offset >= width:
            enqueue(offset - width)
        if offset + width < len(mask):
            enqueue(offset + width)
    return bytes(mask)


def _source_boundary_background(image: Any) -> bytes:
    """Find a painted checker/solid backdrop before palette quantization.

    Image generators sometimes render a transparency checker into an opaque
    RGB image. Detecting the background after fixed-palette mapping then
    confuses light filament regions with that backdrop. Here we learn the
    dominant colors only from the outer border and flood-fill those source
    colors before any filament color is assigned.
    """
    width, height = image.size
    pixels = list(image.getdata())
    border_offsets = list(range(width)) + list(range((height - 1) * width, height * width))
    border_offsets.extend(y * width for y in range(1, height - 1))
    border_offsets.extend(y * width + width - 1 for y in range(1, height - 1))
    border_bins = Counter(tuple(channel // 8 for channel in pixels[offset]) for offset in border_offsets)
    minimum_bin_count = max(2, math.ceil(len(border_offsets) * 0.01))
    dominant_bins = {
        value for value, count in border_bins.most_common(8) if count >= minimum_bin_count
    }
    if not dominant_bins:
        dominant_bins = {border_bins.most_common(1)[0][0]}

    def is_background_candidate(offset: int) -> bool:
        pixel_bin = tuple(channel // 8 for channel in pixels[offset])
        return any(max(abs(left - right) for left, right in zip(pixel_bin, candidate)) <= 1
                   for candidate in dominant_bins)

    mask = bytearray(width * height)
    pending: deque[int] = deque()

    def enqueue(offset: int) -> None:
        if not mask[offset] and is_background_candidate(offset):
            mask[offset] = 255
            pending.append(offset)

    for offset in border_offsets:
        enqueue(offset)
    while pending:
        offset = pending.popleft()
        x = offset % width
        if x:
            enqueue(offset - 1)
        if x + 1 < width:
            enqueue(offset + 1)
        if offset >= width:
            enqueue(offset - width)
        if offset + width < len(mask):
            enqueue(offset + width)
    return bytes(mask)


def _foreground_component_areas(background: bytes, width: int, height: int) -> list[int]:
    visited = bytearray(width * height)
    areas: list[int] = []
    for seed, is_background in enumerate(background):
        if is_background or visited[seed]:
            continue
        pending: deque[int] = deque([seed])
        visited[seed] = 1
        area = 0
        while pending:
            offset = pending.popleft()
            area += 1
            x, y = offset % width, offset // width
            for neighbor in (
                offset - 1 if x else -1,
                offset + 1 if x + 1 < width else -1,
                offset - width if y else -1,
                offset + width if y + 1 < height else -1,
            ):
                if neighbor >= 0 and not background[neighbor] and not visited[neighbor]:
                    visited[neighbor] = 1
                    pending.append(neighbor)
        areas.append(area)
    return areas


def _component_pass(
    indices: bytes | bytearray,
    width: int,
    height: int,
    minimum_width: int,
    palette_lab: list[tuple[float, float, float]],
    color_distance: str,
    *,
    merge: bool,
) -> tuple[bytearray, int, int, int]:
    output = bytearray(indices)
    visited = bytearray(width * height)
    small_pixels = 0
    small_regions = 0
    region_count = 0
    minimum_area = max(1, minimum_width * minimum_width)
    for seed in range(width * height):
        if visited[seed]:
            continue
        color = indices[seed]
        visited[seed] = 1
        pending: deque[int] = deque([seed])
        component: list[int] = []
        neighbors: Counter[int] = Counter()
        min_x = max_x = seed % width
        min_y = max_y = seed // width
        while pending:
            offset = pending.popleft()
            component.append(offset)
            x, y = offset % width, offset // width
            min_x, max_x = min(min_x, x), max(max_x, x)
            min_y, max_y = min(min_y, y), max(max_y, y)
            for neighbor in (
                offset - 1 if x else -1,
                offset + 1 if x + 1 < width else -1,
                offset - width if y else -1,
                offset + width if y + 1 < height else -1,
            ):
                if neighbor < 0:
                    continue
                if indices[neighbor] == color:
                    if not visited[neighbor]:
                        visited[neighbor] = 1
                        pending.append(neighbor)
                else:
                    neighbors[indices[neighbor]] += 1
        region_count += 1
        too_small = (
            len(component) < minimum_area
            or max_x - min_x + 1 < minimum_width
            or max_y - min_y + 1 < minimum_width
        )
        if too_small:
            small_regions += 1
            small_pixels += len(component)
            if merge and neighbors:
                replacement = min(
                    neighbors,
                    key=lambda candidate: (
                        -neighbors[candidate],
                        _distance(palette_lab[color], palette_lab[candidate], color_distance),
                        candidate,
                    ),
                )
                for offset in component:
                    output[offset] = replacement
    return output, region_count, small_regions, small_pixels


def _boundary_complexity(indices: bytes | bytearray, width: int, height: int) -> float:
    boundary = 0
    possible = max(1, (width - 1) * height + (height - 1) * width)
    for y in range(height):
        row = y * width
        for x in range(width):
            offset = row + x
            if x + 1 < width and indices[offset] != indices[offset + 1]:
                boundary += 1
            if y + 1 < height and indices[offset] != indices[offset + width]:
                boundary += 1
    return boundary / possible


def process_printable_image(
    source_path: str | os.PathLike[str],
    output_directory: str | os.PathLike[str],
    palette: Iterable[str],
    settings: PrintSettings | dict[str, Any] | None = None,
    palette_roles: Mapping[str, str] | None = None,
) -> PipelineResult:
    try:
        from PIL import Image, ImageFilter, ImageOps, UnidentifiedImageError
    except ImportError:
        raise PrintableImageError("Pillow is required for printable image processing") from None

    selected = normalize_palette(palette)
    try:
        role_assignment = assign_palette_roles(selected, palette_roles)
    except PrintablePaletteError as exc:
        raise PrintableImageError(str(exc)) from None
    options = settings if isinstance(settings, PrintSettings) else PrintSettings.from_mapping(settings)
    source = Path(source_path)
    output = Path(output_directory)
    output.mkdir(parents=True, exist_ok=True)
    try:
        with Image.open(source) as opened:
            if opened.width <= 0 or opened.height <= 0 or opened.width * opened.height > MAX_IMAGE_PIXELS:
                raise PrintableImageError("source image has an invalid size")
            palette_rgb = [_hex_rgb(color) for color in selected]
            palette_lab = [_srgb_to_lab(color) for color in palette_rgb]
            brightest_index = max(range(len(palette_lab)), key=lambda index: palette_lab[index][0])
            if "A" in opened.getbands():
                rgba = opened.convert("RGBA")
                base = Image.new("RGBA", opened.size, palette_rgb[brightest_index] + (255,))
                base.alpha_composite(rgba)
                rgb = base.convert("RGB")
            else:
                rgb = opened.convert("RGB")
            smoothed = rgb.filter(ImageFilter.MedianFilter(size=3))
            adaptive = smoothed.quantize(
                # Designer-toy renders often devote most pixels to a white background and one dominant material.
                # Keep enough adaptive clusters for smaller semantic materials (for example green clothing beside blue hair)
                # so median-cut does not merge their hues before the fixed filament mapping runs.
                colors=min(128, max(64, len(selected) * 16)),
                method=Image.Quantize.MEDIANCUT,
                dither=Image.Dither.NONE,
            )
            histogram = adaptive.getcolors(maxcolors=opened.width * opened.height) or []
            used = sorted(index for count, index in histogram if count)
            adaptive_palette = adaptive.getpalette() or []
            source_rgb = [tuple(adaptive_palette[index * 3:index * 3 + 3]) for index in used]
            source_lab = [_srgb_to_lab(color) for color in source_rgb]  # type: ignore[arg-type]
            assignments = [
                brightest_index
                if _is_bright_neutral(source_rgb[index])
                else min(
                    range(len(selected)),
                    key=lambda target: _distance(color, palette_lab[target], options.color_distance),
                )
                for index, color in enumerate(source_lab)
            ]
            index_map = {source_index: target for source_index, target in zip(used, assignments)}
            adaptive_data = bytes(adaptive.getdata())
            strict_indices = bytes(index_map.get(index, brightest_index) for index in adaptive_data)
            color_bytes = bytes(channel for index in strict_indices for channel in palette_rgb[index])
            strict_image = Image.frombytes("RGB", opened.size, color_bytes)

            border = []
            step = max(1, min(opened.size) // 256)
            for x in range(0, opened.width, step):
                border.extend((smoothed.getpixel((x, 0)), smoothed.getpixel((x, opened.height - 1))))
            for y in range(0, opened.height, step):
                border.extend((smoothed.getpixel((0, y)), smoothed.getpixel((opened.width - 1, y))))
            border_rgb = tuple(sorted(pixel[channel] for pixel in border)[len(border) // 2] for channel in range(3))
            border_lab = _srgb_to_lab(border_rgb)  # type: ignore[arg-type]
            background_index = min(
                range(len(selected)), key=lambda index: _distance(border_lab, palette_lab[index], options.color_distance)
            )
            source_background = _source_boundary_background(rgb)

            minimum_feature_px = max(1, math.ceil(opened.width * options.minimum_feature_mm / options.width_mm))
            cleanup_kernel = min(7, minimum_feature_px if minimum_feature_px % 2 else minimum_feature_px - 1)
            cleanup_seed = strict_indices
            if cleanup_kernel >= 3:
                cleanup_seed = bytes(
                    Image.frombytes("L", opened.size, strict_indices)
                    .filter(ImageFilter.ModeFilter(size=cleanup_kernel))
                    .getdata()
                )
            cleaned, region_count, small_count, small_pixels = _component_pass(
                cleanup_seed, opened.width, opened.height, minimum_feature_px,
                palette_lab, options.color_distance, merge=True
            )
            cleaned, _, _, _ = _component_pass(
                cleaned, opened.width, opened.height, minimum_feature_px,
                palette_lab, options.color_distance, merge=True
            )
            _, clean_region_count, clean_small_count, clean_small_pixels = _component_pass(
                cleaned, opened.width, opened.height, minimum_feature_px,
                palette_lab, options.color_distance, merge=False
            )
            quantized_background = _boundary_background(bytes(cleaned), opened.width, opened.height, background_index)
            source_subject_ratio = 1.0 - sum(bool(value) for value in source_background) / len(source_background)
            background = source_background if 0.05 <= source_subject_ratio <= 0.95 else quantized_background
            for offset, is_background in enumerate(background):
                if is_background:
                    cleaned[offset] = background_index
            clean_bytes = bytes(channel for index in cleaned for channel in palette_rgb[index])
            clean_image = Image.frombytes("RGB", opened.size, clean_bytes)

            strict_path = output / "four_color_preview.png"
            clean_path = output / "clean_preview.png"
            model_reference_path = output / "model_reference.png"
            heatmap_path = output / "unprintable_heatmap.png"
            background_path = output / "mask_background.png"
            subject_path = output / "mask_subject.png"
            _atomic_png(strict_image, strict_path)

            changed = bytes(255 if left != right else 0 for left, right in zip(strict_indices, cleaned))
            gray = ImageOps.grayscale(rgb).convert("RGB")
            red = Image.new("RGB", opened.size, (255, 40, 40))
            heatmap = Image.blend(gray, red, 0.72)
            heatmap.paste(gray, mask=ImageOps.invert(Image.frombytes("L", opened.size, changed)))
            _atomic_png(heatmap, heatmap_path)

            _atomic_png(Image.frombytes("L", opened.size, background), background_path)
            subject_mask = ImageOps.invert(Image.frombytes("L", opened.size, background))
            _atomic_png(subject_mask, subject_path)
            model_reference = clean_image.convert("RGBA")
            model_reference.putalpha(subject_mask)
            # The clean preview is also subject-only. Keeping its background
            # transparent lets the native GUI show light filament regions on a
            # checkerboard without introducing a fifth, non-printable color.
            _atomic_png(model_reference, clean_path)
            _atomic_png(model_reference, model_reference_path)

            roles = [role_assignment.role_by_color[color] for color in selected]
            mask_paths: dict[str, Path] = {}
            for index, role in enumerate(roles):
                path = output / f"mask_{role}.png"
                mask = bytes(255 if value == index else 0 for value in cleaned)
                _atomic_png(Image.frombytes("L", opened.size, mask), path)
                mask_paths[role] = path

            usage_counts = Counter(cleaned)
            usage = {color: usage_counts[index] for index, color in enumerate(selected)}
            total = opened.width * opened.height
            subject_usage_counts = Counter(
                color for color, is_background in zip(cleaned, background) if not is_background
            )
            subject_total = sum(subject_usage_counts.values())
            subject_area_ratio = subject_total / total
            subject_component_areas = _foreground_component_areas(background, opened.width, opened.height)
            largest_subject_component_ratio = (
                max(subject_component_areas, default=0) / subject_total if subject_total else 0.0
            )
            meaningful_subject_component_threshold = max(
                minimum_feature_px * minimum_feature_px,
                math.ceil(subject_total * 0.02),
            )
            meaningful_subject_component_count = sum(
                area >= meaningful_subject_component_threshold for area in subject_component_areas
            )
            # Portraits and crossed-arm toy poses legitimately contain small
            # detached 2D islands (hands, facial details, base highlights).
            # Reject genuinely split subjects while allowing those printable
            # details to occupy up to ten percent of the silhouette.
            subject_continuity_ok = largest_subject_component_ratio >= 0.90
            weighted_error = sum(
                count * _distance(source_lab[used.index(source_index)], palette_lab[index_map[source_index]], options.color_distance)
                for count, source_index in histogram
                if source_index in index_map
            ) / max(1, total)
            strict_small_ratio = small_pixels / total
            clean_small_ratio = clean_small_pixels / total
            boundary = _boundary_complexity(cleaned, opened.width, opened.height)
            changed_ratio = sum(bool(value) for value in changed) / total
            active_ratios = [usage[color] / total for color in selected]
            subject_ratios = [subject_usage_counts[index] / max(1, subject_total) for index in range(len(selected))]
            meaningful_palette_count = sum(ratio >= 0.02 for ratio in subject_ratios)
            meaningful_subject_count = meaningful_palette_count
            required_palette_count = min(len(selected), 3)
            required_subject_count = required_palette_count
            palette_diversity_ok = (
                meaningful_palette_count >= required_palette_count
                and meaningful_subject_count >= required_subject_count
            )
            palette_quality_ok = (
                subject_area_ratio >= 0.18
                and subject_continuity_ok
            )
            imbalance = max(active_ratios) - min(active_ratios)
            score = min(1.0, weighted_error / 100.0) * 0.45 + clean_small_ratio * 0.30 + boundary * 0.15 + imbalance * 0.10
            warnings = []
            if clean_small_ratio > 0.02:
                warnings.append("small_region_ratio_above_2_percent")
            if max(active_ratios) > 0.90:
                warnings.append("palette_area_is_highly_imbalanced")
            if meaningful_palette_count < required_palette_count:
                warnings.append("too_few_meaningful_palette_colors")
            if meaningful_subject_count < required_subject_count:
                warnings.append("too_few_meaningful_subject_colors")
            if subject_area_ratio < 0.18:
                warnings.append("printable_subject_area_below_18_percent")
            if not subject_continuity_ok:
                warnings.append("printable_subject_is_disconnected")
            if minimum_feature_px == 1:
                warnings.append("minimum_feature_rounds_to_one_pixel")
            if role_assignment.low_contrast:
                warnings.append("filament_colors_have_low_contrast")
            metrics: dict[str, Any] = {
                "score": round(score, 6),
                "mean_color_error": round(weighted_error, 4),
                "small_region_ratio_before": round(strict_small_ratio, 6),
                "small_region_ratio_after": round(clean_small_ratio, 6),
                "boundary_complexity": round(boundary, 6),
                "changed_pixel_ratio": round(changed_ratio, 6),
                "region_count_before": region_count,
                "region_count_after": clean_region_count,
                "small_region_count_before": small_count,
                "small_region_count_after": clean_small_count,
                "minimum_feature_px": minimum_feature_px,
                "palette_area_ratio": {color: round(usage[color] / total, 6) for color in selected},
                "subject_palette_area_ratio": {
                    color: round(subject_usage_counts[index] / max(1, subject_total), 6)
                    for index, color in enumerate(selected)
                },
                "meaningful_palette_count": meaningful_palette_count,
                "meaningful_subject_color_count": meaningful_subject_count,
                "palette_diversity_ok": palette_diversity_ok,
                "printable_subject_area_ratio": round(subject_area_ratio, 6),
                "subject_component_count": len(subject_component_areas),
                "meaningful_subject_component_count": meaningful_subject_component_count,
                "largest_subject_component_ratio": round(largest_subject_component_ratio, 6),
                "palette_quality_ok": palette_quality_ok,
                "quality_warnings": warnings,
            }
            metadata_path = output / "metadata.json"
            metadata = {
                "schema_version": 2,
                "mode": "fixed_printable_palette",
                "print_mode": options.print_mode,
                "source": source.name,
                "size_px": {"width": opened.width, "height": opened.height},
                "palette": [
                    {"index": index, "id": roles[index], "hex": color, "lab": [round(value, 4) for value in palette_lab[index]]}
                    for index, color in enumerate(selected)
                ],
                "palette_roles": role_assignment.as_metadata(),
                "background": {"palette_index": background_index, "palette_hex": selected[background_index]},
                "print": asdict(options),
                "metrics": metrics,
                "warnings": warnings,
                "outputs": {
                    "strict_preview": strict_path.name,
                    "clean_preview": clean_path.name,
                    "model_reference": model_reference_path.name,
                    "heatmap": heatmap_path.name,
                    "background_mask": background_path.name,
                    "subject_mask": subject_path.name,
                    "masks": {role: path.name for role, path in mask_paths.items()},
                },
            }
            _atomic_json(metadata, metadata_path)
            return PipelineResult(
                strict_preview=strict_path,
                clean_preview=clean_path,
                model_reference=model_reference_path,
                heatmap=heatmap_path,
                background_mask=background_path,
                subject_mask=subject_path,
                masks=mask_paths,
                metadata=metadata_path,
                metrics=metrics,
                palette_usage=usage,
            )
    except PrintableImageError:
        raise
    except (OSError, UnidentifiedImageError, Image.DecompressionBombError):
        raise PrintableImageError("source image could not be processed") from None
