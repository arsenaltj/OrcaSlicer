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


def _fill_binary_subject_holes(foreground: bytes, width: int, height: int) -> bytes:
    """Fill subject holes that cannot reach the image border."""

    if width <= 0 or height <= 0 or len(foreground) != width * height:
        raise PrintableImageError("subject mask has inconsistent dimensions")
    reachable_background = bytearray(len(foreground))
    pending: deque[int] = deque()

    def enqueue(offset: int) -> None:
        if not foreground[offset] and not reachable_background[offset]:
            reachable_background[offset] = 1
            pending.append(offset)

    for x in range(width):
        enqueue(x)
        enqueue((height - 1) * width + x)
    for y in range(1, height - 1):
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
        if offset + width < len(foreground):
            enqueue(offset + width)
    return bytes(
        255 if value or not reachable_background[offset] else 0
        for offset, value in enumerate(foreground)
    )


def _repair_portrait_background_mask(
    background: bytes,
    width: int,
    height: int,
) -> tuple[bytes, int]:
    """Recover light clothing swallowed by an opaque checkerboard backdrop.

    Some image providers paint a white checkerboard instead of returning real
    alpha. A color flood-fill can then enter a white jacket through a narrow
    low-contrast opening. Close only small openings at a bounded analysis
    resolution, fill the newly enclosed holes, and union them with the original
    subject. This runs only after the portrait material detector has found a
    credible face/garment palette, so non-portrait handles remain governed by
    the stricter detached-structure gate.
    """

    if width <= 0 or height <= 0 or len(background) != width * height:
        raise PrintableImageError("portrait background mask has inconsistent dimensions")
    try:
        from PIL import Image, ImageFilter
    except ImportError:
        raise PrintableImageError("Pillow is required to repair portrait masks") from None

    maximum_analysis_dimension = 384
    scale = min(1.0, maximum_analysis_dimension / max(width, height))
    analysis_width = max(1, round(width * scale))
    analysis_height = max(1, round(height * scale))
    foreground = Image.frombytes(
        "L", (width, height), bytes(0 if value else 255 for value in background)
    )
    analysis = foreground.resize(
        (analysis_width, analysis_height), Image.Resampling.NEAREST
    )
    radius = max(2, round(min(analysis_width, analysis_height) * 0.015))
    kernel = radius * 2 + 1
    closed = analysis.filter(ImageFilter.MaxFilter(kernel)).filter(
        ImageFilter.MinFilter(kernel)
    )
    filled = _fill_binary_subject_holes(
        closed.tobytes(), analysis_width, analysis_height
    )
    recovered = Image.frombytes(
        "L", (analysis_width, analysis_height), filled
    ).resize((width, height), Image.Resampling.NEAREST).tobytes()
    recovered_count = sum(
        bool(is_background and recovered_value)
        for is_background, recovered_value in zip(background, recovered)
    )
    # A portrait leak is local. Refuse a repair that would absorb a large part
    # of the frame, preserving the original mask for downstream quality gates.
    if recovered_count > width * height * 0.12:
        return background, 0
    repaired = bytes(
        0 if not is_background or recovered_value else 255
        for is_background, recovered_value in zip(background, recovered)
    )
    return repaired, recovered_count


def _largest_foreground_mask(background: bytes, width: int, height: int) -> bytes:
    """Return only the largest 4-connected foreground component."""

    visited = bytearray(width * height)
    largest: list[int] = []
    for seed, is_background in enumerate(background):
        if is_background or visited[seed]:
            continue
        visited[seed] = 1
        pending: deque[int] = deque([seed])
        component: list[int] = []
        while pending:
            offset = pending.popleft()
            component.append(offset)
            x = offset % width
            neighbors = (
                offset - 1 if x else -1,
                offset + 1 if x + 1 < width else -1,
                offset - width if offset >= width else -1,
                offset + width if offset + width < len(background) else -1,
            )
            for neighbor in neighbors:
                if neighbor >= 0 and not background[neighbor] and not visited[neighbor]:
                    visited[neighbor] = 1
                    pending.append(neighbor)
        if len(component) > len(largest):
            largest = component
    result = bytearray(width * height)
    for offset in largest:
        result[offset] = 255
    return bytes(result)


def _portrait_source_subject_mask(
    width: int,
    height: int,
    reference_path: str | os.PathLike[str] | None,
) -> bytes:
    """Build a conservative, aligned upper-body mask from the user's photo."""

    if reference_path is None or width <= 0 or height <= 0:
        return b""
    try:
        from PIL import Image, ImageFilter, UnidentifiedImageError
        with Image.open(reference_path) as opened:
            reference = opened.convert("RGB").resize((width, height), Image.Resampling.LANCZOS)
    except (OSError, ValueError, UnidentifiedImageError):
        return b""

    reference_background = _source_boundary_background(reference)
    source_foreground = _largest_foreground_mask(reference_background, width, height)
    source_image = Image.frombytes("L", (width, height), source_foreground)
    expansion = max(3, min(9, (round(min(width, height) * 0.006) | 1)))
    source_image = source_image.filter(ImageFilter.MaxFilter(expansion))
    # The source may be a waist-up photograph cropped at the bottom. Preserve
    # the generated bust finish and newly added base below this safe band.
    source_image.paste(0, (0, round(height * 0.76), width, height))
    source_foreground = source_image.tobytes()
    source_count = sum(value > 0 for value in source_foreground)
    if not width * height * 0.08 <= source_count <= width * height * 0.65:
        return b""
    return source_foreground


def _repair_portrait_mask_from_source(
    background: bytes,
    width: int,
    height: int,
    reference_path: str | os.PathLike[str] | None,
) -> tuple[bytes, int]:
    """Recover a white portrait garment using the user's original silhouette.

    Opaque checkerboards can be nearly identical to a white blazer, so closing
    the generated mask alone may still leave broad transparent panels. A
    realistic portrait edit is contractually composition-locked to the source.
    Reuse only the largest, strongly overlapping upper-body component from that
    source, stop before the generated bust/base region, and cap the added area.
    """

    if reference_path is None or width <= 0 or height <= 0 or len(background) != width * height:
        return background, 0
    source_foreground = _portrait_source_subject_mask(width, height, reference_path)
    if not source_foreground:
        return background, 0
    current_foreground = bytes(0 if value else 255 for value in background)
    source_count = sum(value > 0 for value in source_foreground)
    overlap = sum(
        source_value > 0 and current_value > 0
        for source_value, current_value in zip(source_foreground, current_foreground)
    )
    if overlap < source_count * 0.45:
        return background, 0
    recovered_count = sum(
        bool(source_value and is_background)
        for source_value, is_background in zip(source_foreground, background)
    )
    if recovered_count <= 0 or recovered_count > width * height * 0.18:
        return background, 0
    repaired = bytes(
        0 if not is_background or source_value else 255
        for is_background, source_value in zip(background, source_foreground)
    )
    return repaired, recovered_count


@dataclass(frozen=True)
class _ForegroundComponent:
    area: int
    left: int
    top: int
    right: int
    bottom: int

    @property
    def diagonal(self) -> float:
        return math.hypot(self.right - self.left + 1, self.bottom - self.top + 1)


def _foreground_components(background: bytes, width: int, height: int) -> list[_ForegroundComponent]:
    visited = bytearray(width * height)
    components: list[_ForegroundComponent] = []
    for seed, is_background in enumerate(background):
        if is_background or visited[seed]:
            continue
        pending: deque[int] = deque([seed])
        visited[seed] = 1
        area = 0
        left = right = seed % width
        top = bottom = seed // width
        while pending:
            offset = pending.popleft()
            area += 1
            x, y = offset % width, offset // width
            left, right = min(left, x), max(right, x)
            top, bottom = min(top, y), max(bottom, y)
            for neighbor in (
                offset - 1 if x else -1,
                offset + 1 if x + 1 < width else -1,
                offset - width if y else -1,
                offset + width if y + 1 < height else -1,
            ):
                if neighbor >= 0 and not background[neighbor] and not visited[neighbor]:
                    visited[neighbor] = 1
                    pending.append(neighbor)
        components.append(_ForegroundComponent(area, left, top, right, bottom))
    return components


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


def _color_component_stats(
    indices: bytes | bytearray,
    background: bytes | bytearray,
    width: int,
    height: int,
    color_count: int,
) -> tuple[list[int], list[float]]:
    """Measure color islands without treating the transparent background as a material region."""

    visited = bytearray(width * height)
    areas: list[list[int]] = [[] for _ in range(color_count)]
    subject_total = sum(not value for value in background)
    for seed in range(width * height):
        if visited[seed] or background[seed]:
            continue
        color = indices[seed]
        visited[seed] = 1
        pending: deque[int] = deque([seed])
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
                if (
                    neighbor >= 0
                    and not visited[neighbor]
                    and not background[neighbor]
                    and indices[neighbor] == color
                ):
                    visited[neighbor] = 1
                    pending.append(neighbor)
        areas[color].append(area)
    counts = [len(values) for values in areas]
    secondary_ratios = [
        (sum(values) - max(values, default=0)) / max(1, subject_total)
        for values in areas
    ]
    return counts, secondary_ratios


def _stabilize_portrait_skin_components(
    indices: bytes,
    background: bytes,
    source_pixels: list[tuple[int, int, int]],
    width: int,
    height: int,
    palette: tuple[str, ...],
    role_by_color: Mapping[str, str],
) -> tuple[bytes, dict[str, Any]]:
    """Remove low-chroma garment shadows that were quantized as skin.

    This activates only when a bright neutral primary, a warm skin-like light
    filament, and a large upper-center warm component provide portrait evidence.
    Saturated source-visible hands remain valid; low-saturation cream jacket
    shadows are merged into their strongest non-skin neighbor.
    """

    report: dict[str, Any] = {
        "activated": 0,
        "removed_components": 0,
        "trimmed_components": 0,
        "recolored_pixels": 0,
        "garment_color": "",
        "skin_color": "",
        "role_fallback_used": 0,
        "strong_skin_fallback_used": 0,
        "strong_skin_seed_components": 0,
        "preserved_hand_seed_components": 0,
        "face_bounds": {},
    }
    if len(indices) != width * height or len(background) != len(indices) or len(source_pixels) != len(indices):
        raise PrintableImageError("portrait material stabilization received inconsistent image data")
    def bright_neutral(color: str) -> bool:
        red, green, blue = _hex_rgb(color)
        return max(red, green, blue) - min(red, green, blue) <= 32 and (red + green + blue) / 3 >= 180

    def warm_skin(color: str) -> bool:
        red, green, blue = _hex_rgb(color)
        return (
            red > green >= blue
            and 12 <= red - green <= 82
            and 28 <= red - blue <= 118
            and green >= 70
            and blue >= 45
        )

    role_primary = next((color for color in palette if role_by_color.get(color) == "primary"), "")
    role_skin = next((color for color in palette if role_by_color.get(color) == "light"), "")
    role_pair_valid = bool(role_primary and role_skin and bright_neutral(role_primary) and warm_skin(role_skin))
    if role_pair_valid:
        primary_color, skin_color = role_primary, role_skin
    else:
        neutral_candidates = [color for color in palette if bright_neutral(color)]
        skin_candidates = [color for color in palette if warm_skin(color)]
        if not neutral_candidates or not skin_candidates:
            return indices, report
        primary_color = max(neutral_candidates, key=lambda color: sum(_hex_rgb(color)))
        skin_color = max(skin_candidates, key=lambda color: sum(_hex_rgb(color)))
        if primary_color == skin_color:
            return indices, report
        report["role_fallback_used"] = 1
    primary_index = palette.index(primary_color)
    skin_index = palette.index(skin_color)
    report["garment_color"] = primary_color
    report["skin_color"] = skin_color
    foreground = [offset for offset, is_background in enumerate(background) if not is_background]
    if not foreground:
        return indices, report
    left = min(offset % width for offset in foreground)
    right = max(offset % width for offset in foreground) + 1
    top = min(offset // width for offset in foreground)
    bottom = max(offset // width for offset in foreground) + 1
    subject_width = max(1, right - left)
    subject_height = max(1, bottom - top)
    subject_area = len(foreground)

    visited = bytearray(len(indices))
    components: list[dict[str, Any]] = []
    for seed, color in enumerate(indices):
        if color != skin_index or background[seed] or visited[seed]:
            continue
        visited[seed] = 1
        pending: deque[int] = deque([seed])
        pixels: list[int] = []
        neighbors: Counter[int] = Counter()
        sum_x = sum_y = 0
        sum_red = sum_green = sum_blue = 0.0
        sum_saturation = 0.0
        component_left = width
        component_right = -1
        component_top = height
        component_bottom = -1
        while pending:
            offset = pending.popleft()
            pixels.append(offset)
            x, y = offset % width, offset // width
            sum_x += x
            sum_y += y
            component_left = min(component_left, x)
            component_right = max(component_right, x)
            component_top = min(component_top, y)
            component_bottom = max(component_bottom, y)
            red, green, blue = source_pixels[offset]
            sum_red += red
            sum_green += green
            sum_blue += blue
            maximum, minimum = max(red, green, blue), min(red, green, blue)
            sum_saturation += (maximum - minimum) / maximum if maximum else 0.0
            for neighbor in (
                offset - 1 if x else -1,
                offset + 1 if x + 1 < width else -1,
                offset - width if y else -1,
                offset + width if y + 1 < height else -1,
            ):
                if neighbor < 0 or background[neighbor]:
                    continue
                if indices[neighbor] == skin_index:
                    if not visited[neighbor]:
                        visited[neighbor] = 1
                        pending.append(neighbor)
                else:
                    neighbors[indices[neighbor]] += 1
        area = len(pixels)
        components.append({
            "pixels": pixels,
            "area": area,
            "center_x": sum_x / area,
            "center_y": sum_y / area,
            "left": component_left,
            "right": component_right,
            "top": component_top,
            "bottom": component_bottom,
            "mean_rgb": (sum_red / area, sum_green / area, sum_blue / area),
            "mean_saturation": sum_saturation / area,
            "neighbors": neighbors,
        })

    def warm(component: dict[str, Any]) -> bool:
        red, green, blue = component["mean_rgb"]
        return red > green >= blue and red - green >= 18 and red - blue >= 35

    face_candidates = [
        component for component in components
        if component["area"] >= max(64, subject_area * 0.02)
        and left + subject_width * 0.25 <= component["center_x"] <= left + subject_width * 0.75
        and component["top"] <= top + subject_height * 0.38
        and component["mean_saturation"] >= 0.25
        and warm(component)
    ]
    diffuse_face_candidate = bool(face_candidates) and (
        max(face_candidates, key=lambda component: component["area"])["bottom"]
        - max(face_candidates, key=lambda component: component["area"])["top"]
        + 1
    ) > subject_height * 0.48
    if not face_candidates or diffuse_face_candidate:
        # A profile or rear portrait often has no large, front-facing skin
        # component: pale jacket shadows can connect the cheek, neck, hands and
        # torso into one diffuse skin-labelled island. Recover semantic skin
        # from saturated warm source pixels instead. The upper seed owns the
        # visible face/ears/neck; lower compact seeds own hands. Everything
        # else remains garment, even when palette distance alone calls it skin.
        strong_skin = bytearray(len(indices))
        for offset, color in enumerate(indices):
            if color != skin_index or background[offset]:
                continue
            red, green, blue = source_pixels[offset]
            maximum, minimum = max(red, green, blue), min(red, green, blue)
            saturation = (maximum - minimum) / maximum if maximum else 0.0
            if (
                saturation >= 0.20
                and red > green >= blue
                and red - green >= 15
                and red - blue >= 28
            ):
                strong_skin[offset] = 1

        strong_visited = bytearray(len(indices))
        strong_components: list[dict[str, Any]] = []
        for seed, is_strong in enumerate(strong_skin):
            if not is_strong or strong_visited[seed]:
                continue
            strong_visited[seed] = 1
            pending: deque[int] = deque([seed])
            pixels: list[int] = []
            component_left = component_right = seed % width
            component_top = component_bottom = seed // width
            while pending:
                offset = pending.popleft()
                pixels.append(offset)
                x, y = offset % width, offset // width
                component_left, component_right = min(component_left, x), max(component_right, x)
                component_top, component_bottom = min(component_top, y), max(component_bottom, y)
                for neighbor in (
                    offset - 1 if x else -1,
                    offset + 1 if x + 1 < width else -1,
                    offset - width if y else -1,
                    offset + width if y + 1 < height else -1,
                ):
                    if neighbor >= 0 and strong_skin[neighbor] and not strong_visited[neighbor]:
                        strong_visited[neighbor] = 1
                        pending.append(neighbor)
            if len(pixels) < max(10, subject_area * 0.00005):
                continue
            strong_components.append({
                "pixels": pixels,
                "area": len(pixels),
                "left": component_left,
                "right": component_right,
                "top": component_top,
                "bottom": component_bottom,
                "center_x": (component_left + component_right) / 2,
                "center_y": (component_top + component_bottom) / 2,
            })
        report["strong_skin_seed_components"] = len(strong_components)
        upper_seed_candidates = [
            component for component in strong_components
            if component["area"] >= max(24, subject_area * 0.001)
            and component["top"] <= top + subject_height * 0.45
            and component["center_y"] <= top + subject_height * 0.42
        ]
        if not upper_seed_candidates:
            return indices, report
        face_seed = max(upper_seed_candidates, key=lambda component: component["area"])
        face_seed_components = [
            component for component in strong_components
            if component["area"] >= max(10, subject_area * 0.00005)
            and component["top"] <= top + subject_height * 0.45
            and component["center_y"] <= top + subject_height * 0.42
            if abs(component["center_x"] - face_seed["center_x"]) <= subject_width * 0.35
            and component["top"] <= face_seed["bottom"] + subject_height * 0.08
            and component["bottom"] >= face_seed["top"] - subject_height * 0.04
        ]
        report["face_seed_components"] = len(face_seed_components)
        padding_x = max(3, round(subject_width * 0.03))
        padding_top = max(2, round(subject_height * 0.02))
        padding_bottom = max(4, round(subject_height * 0.06))
        face_left = max(left, min(component["left"] for component in face_seed_components) - padding_x)
        face_right = min(right - 1, max(component["right"] for component in face_seed_components) + padding_x)
        face_top = max(top, min(component["top"] for component in face_seed_components) - padding_top)
        face_bottom = min(bottom - 1, max(component["bottom"] for component in face_seed_components) + padding_bottom)
        report["face_bounds"] = {
            "left": face_left,
            "right": face_right,
            "top": face_top,
            "bottom": face_bottom,
        }

        face_seed_pixels = [
            offset for component in face_seed_components for offset in component["pixels"]
        ]
        retained: set[int] = set(face_seed_pixels)
        pending = deque(face_seed_pixels)
        while pending:
            offset = pending.popleft()
            x, y = offset % width, offset // width
            for neighbor in (
                offset - 1 if x else -1,
                offset + 1 if x + 1 < width else -1,
                offset - width if y else -1,
                offset + width if y + 1 < height else -1,
            ):
                if neighbor < 0 or neighbor in retained or background[neighbor] or indices[neighbor] != skin_index:
                    continue
                neighbor_x, neighbor_y = neighbor % width, neighbor // width
                if face_left <= neighbor_x <= face_right and face_top <= neighbor_y <= face_bottom:
                    retained.add(neighbor)
                    pending.append(neighbor)

        hand_seed_candidates = [
            component for component in strong_components
            if all(component is not face_component for face_component in face_seed_components)
            and component["area"] >= max(24, subject_area * 0.0002)
            and component["center_y"] <= top + subject_height * 0.84
            and max(
                component["right"] - component["left"] + 1,
                component["bottom"] - component["top"] + 1,
            ) / max(
                1,
                min(
                    component["right"] - component["left"] + 1,
                    component["bottom"] - component["top"] + 1,
                ),
            ) <= 4.0
        ]
        hand_seed_components = sorted(
            hand_seed_candidates,
            key=lambda component: component["area"],
            reverse=True,
        )[:2]
        report["preserved_hand_seed_components"] = len(hand_seed_components)
        maximum_distance = max(3, round(min(subject_width, subject_height) * 0.015))
        hand_pending: deque[tuple[int, int]] = deque()
        for component in hand_seed_components:
            for offset in component["pixels"]:
                if offset not in retained:
                    retained.add(offset)
                    hand_pending.append((offset, 0))
        while hand_pending:
            offset, distance = hand_pending.popleft()
            if distance >= maximum_distance:
                continue
            x, y = offset % width, offset // width
            for neighbor in (
                offset - 1 if x else -1,
                offset + 1 if x + 1 < width else -1,
                offset - width if y else -1,
                offset + width if y + 1 < height else -1,
            ):
                if (
                    neighbor >= 0
                    and neighbor not in retained
                    and not background[neighbor]
                    and indices[neighbor] == skin_index
                ):
                    retained.add(neighbor)
                    hand_pending.append((neighbor, distance + 1))

        output = bytearray(indices)
        for component in components:
            removed = [offset for offset in component["pixels"] if offset not in retained]
            if not removed:
                continue
            neighbors = component["neighbors"]
            replacement = min(
                (index for index in neighbors if index != skin_index),
                key=lambda index: (-neighbors[index], index),
                default=primary_index,
            )
            for offset in removed:
                output[offset] = replacement
            if len(removed) == component["area"]:
                report["removed_components"] += 1
            else:
                report["trimmed_components"] += 1
            report["recolored_pixels"] += len(removed)
        report["activated"] = 1
        report["strong_skin_fallback_used"] = 1
        return bytes(output), report
    face = max(face_candidates, key=lambda component: component["area"])
    report["face_bounds"] = {
        "left": face["left"],
        "right": face["right"],
        "top": face["top"],
        "bottom": face["bottom"],
    }
    output = bytearray(indices)
    for component in components:
        if component is face:
            continue
        strong_skin_pixels = []
        for offset in component["pixels"]:
            red, green, blue = source_pixels[offset]
            maximum, minimum = max(red, green, blue), min(red, green, blue)
            saturation = (maximum - minimum) / maximum if maximum else 0.0
            if saturation >= 0.30 and red > green >= blue and red - green >= 18 and red - blue >= 35:
                strong_skin_pixels.append(offset)
        meaningful_warm_skin = (
            component["area"] >= max(24, subject_area * 0.00025)
            and component["center_y"] <= top + subject_height * 0.82
            and len(strong_skin_pixels) >= max(24, component["area"] * 0.08)
        )
        if meaningful_warm_skin:
            component_membership = set(component["pixels"])
            maximum_distance = max(2, round(min(width, height) * 0.003))
            retained = set(strong_skin_pixels)
            pending: deque[tuple[int, int]] = deque((offset, 0) for offset in strong_skin_pixels)
            while pending:
                offset, distance = pending.popleft()
                if distance >= maximum_distance:
                    continue
                x, y = offset % width, offset // width
                for neighbor in (
                    offset - 1 if x else -1,
                    offset + 1 if x + 1 < width else -1,
                    offset - width if y else -1,
                    offset + width if y + 1 < height else -1,
                ):
                    if neighbor in component_membership and neighbor not in retained:
                        retained.add(neighbor)
                        pending.append((neighbor, distance + 1))
            if retained:
                removed = [offset for offset in component["pixels"] if offset not in retained]
                if removed:
                    neighbors = component["neighbors"]
                    replacement = min(
                        (index for index in neighbors if index != skin_index),
                        key=lambda index: (-neighbors[index], index),
                        default=primary_index,
                    )
                    for offset in removed:
                        output[offset] = replacement
                    report["trimmed_components"] += 1
                    report["recolored_pixels"] += len(removed)
                continue
        neighbors = component["neighbors"]
        replacement = min(
            (index for index in neighbors if index != skin_index),
            key=lambda index: (-neighbors[index], index),
            default=primary_index,
        )
        for offset in component["pixels"]:
            output[offset] = replacement
        report["removed_components"] += 1
        report["recolored_pixels"] += component["area"]
    report["activated"] = 1
    return bytes(output), report


def _stabilize_portrait_accent_components(
    indices: bytearray,
    background: bytes | bytearray,
    source_pixels: list[tuple[int, int, int]],
    width: int,
    height: int,
    palette: tuple[str, ...],
    portrait_report: Mapping[str, Any],
) -> dict[str, Any]:
    """Remove portrait accent islands outside the main secondary garment.

    This is gated by the skin/garment portrait detector above. The remaining
    non-neutral, non-skin, non-structure material is expected to describe one
    secondary garment. Its largest component is the anchor; small disconnected
    highlights in hair, jacket edges, or the base are reassigned to the material
    that actually surrounds them.
    """

    report: dict[str, Any] = {
        "accent_color": "",
        "accent_removed_components": 0,
        "accent_recolored_pixels": 0,
        "accent_skin_recolored_pixels": 0,
        "accent_shadow_recolored_pixels": 0,
        "accent_neutral_shadow_recolored_pixels": 0,
        "accent_seeded_pixels": 0,
        "accent_anchor_source_hue_pixels": 0,
        "accent_anchor_in_torso": 0,
    }
    if (
        portrait_report.get("activated") != 1
        or len(indices) != width * height
        or len(source_pixels) != len(indices)
    ):
        return report
    garment_color = str(portrait_report.get("garment_color", "")).upper()
    skin_color = str(portrait_report.get("skin_color", "")).upper()
    if garment_color not in palette or skin_color not in palette:
        return report
    structure_index = min(range(len(palette)), key=lambda index: sum(_hex_rgb(palette[index])))
    excluded = {palette.index(garment_color), palette.index(skin_color), structure_index}
    candidates = [index for index in range(len(palette)) if index not in excluded]
    if len(candidates) != 1:
        return report
    accent_index = candidates[0]
    report["accent_color"] = palette[accent_index]
    accent_rgb = _hex_rgb(palette[accent_index])
    _, accent_a, accent_b = _srgb_to_lab(accent_rgb)
    accent_chroma = math.hypot(accent_a, accent_b)

    def shares_accent_hue(color: tuple[int, int, int]) -> bool:
        _, source_a, source_b = _srgb_to_lab(color)
        source_chroma = math.hypot(source_a, source_b)
        if accent_chroma < 8.0 or source_chroma < 6.0:
            return False
        cosine = (source_a * accent_a + source_b * accent_b) / (source_chroma * accent_chroma)
        cosine = max(-1.0, min(1.0, cosine))
        return math.degrees(math.acos(cosine)) <= 32.0

    # A dark or shaded neck can be closer to a green/brown garment filament
    # than to the available skin filament. Component cleanup alone cannot fix
    # this when the false patch touches the real blouse. Within the detected
    # face/neck envelope, restore warm source pixels to skin before finding the
    # garment anchor. Genuine garment pixels remain unchanged because their
    # source hue is not warm-skin-like.
    face_bounds = portrait_report.get("face_bounds")
    if isinstance(face_bounds, Mapping) and face_bounds:
        try:
            face_left = int(face_bounds["left"])
            face_right = int(face_bounds["right"])
            face_top = int(face_bounds["top"])
            face_bottom = int(face_bounds["bottom"])
        except (KeyError, TypeError, ValueError):
            face_left = face_right = face_top = face_bottom = -1
        if face_left >= 0 and face_right >= face_left and face_bottom >= face_top:
            face_width = max(1, face_right - face_left + 1)
            face_height = max(1, face_bottom - face_top + 1)
            protected_left = max(0, round(face_left - face_width * 0.06))
            protected_right = min(width - 1, round(face_right + face_width * 0.06))
            protected_bottom = min(height - 1, round(face_bottom + face_height * 0.12))
            for offset, color in enumerate(indices):
                if color != accent_index or background[offset]:
                    continue
                x, y = offset % width, offset // width
                if not (
                    protected_left <= x <= protected_right
                    and face_top <= y <= protected_bottom
                ):
                    continue
                red, green, blue = source_pixels[offset]
                maximum, minimum = max(red, green, blue), min(red, green, blue)
                saturation = (maximum - minimum) / maximum if maximum else 0.0
                if (
                    saturation >= 0.08
                    and red > green >= blue
                    and red - green >= 8
                    and red - blue >= 18
                ):
                    indices[offset] = palette.index(skin_color)
                    report["accent_skin_recolored_pixels"] += 1
                    report["accent_recolored_pixels"] += 1
    foreground = [offset for offset, is_background in enumerate(background) if not is_background]
    if not foreground:
        return report
    subject_area = len(foreground)
    subject_left = min(offset % width for offset in foreground)
    subject_right = max(offset % width for offset in foreground) + 1
    subject_top = min(offset // width for offset in foreground)
    subject_bottom = max(offset // width for offset in foreground) + 1
    subject_width = max(1, subject_right - subject_left)
    subject_height = max(1, subject_bottom - subject_top)

    if portrait_report.get("strong_skin_fallback_used") == 1:
        # Side and rear views cannot use a front-centre blouse anchor. Recover
        # the accent garment from its source hue, then keep only connected
        # torso components that contain real hue evidence. This preserves a
        # side-visible blouse while removing green reflections from hair/base.
        skin_index = palette.index(skin_color)
        for offset, color in enumerate(indices):
            if color != accent_index or background[offset]:
                continue
            red, green, blue = source_pixels[offset]
            maximum, minimum = max(red, green, blue), min(red, green, blue)
            saturation = (maximum - minimum) / maximum if maximum else 0.0
            if (
                saturation >= 0.12
                and red > green >= blue
                and red - green >= 10
                and red - blue >= 20
            ):
                indices[offset] = skin_index
                report["accent_skin_recolored_pixels"] += 1
                report["accent_recolored_pixels"] += 1
        source_accent_seed: set[int] = set()
        torso_top = subject_top + subject_height * 0.22
        torso_bottom = subject_top + subject_height * 0.84
        torso_left = subject_left + subject_width * 0.08
        torso_right = subject_left + subject_width * 0.92
        for offset, color in enumerate(indices):
            if background[offset] or color == skin_index:
                continue
            x, y = offset % width, offset // width
            if not (torso_left <= x <= torso_right and torso_top <= y <= torso_bottom):
                continue
            if not shares_accent_hue(source_pixels[offset]):
                continue
            source_accent_seed.add(offset)
            if color != accent_index:
                indices[offset] = accent_index
                report["accent_seeded_pixels"] += 1
                report["accent_recolored_pixels"] += 1

        visited = bytearray(len(indices))
        for seed, color in enumerate(indices):
            if color != accent_index or background[seed] or visited[seed]:
                continue
            visited[seed] = 1
            pending: deque[int] = deque([seed])
            pixels: list[int] = []
            neighbors: Counter[int] = Counter()
            seed_evidence = 0
            left = right = seed % width
            top = bottom = seed // width
            while pending:
                offset = pending.popleft()
                pixels.append(offset)
                seed_evidence += offset in source_accent_seed
                x, y = offset % width, offset // width
                left, right = min(left, x), max(right, x)
                top, bottom = min(top, y), max(bottom, y)
                for neighbor in (
                    offset - 1 if x else -1,
                    offset + 1 if x + 1 < width else -1,
                    offset - width if y else -1,
                    offset + width if y + 1 < height else -1,
                ):
                    if neighbor < 0 or background[neighbor]:
                        continue
                    if indices[neighbor] == accent_index:
                        if not visited[neighbor]:
                            visited[neighbor] = 1
                            pending.append(neighbor)
                    else:
                        neighbors[indices[neighbor]] += 1
            center_x = (left + right) * 0.5
            center_y = (top + bottom) * 0.5
            valid_torso_component = (
                len(pixels) >= max(12, subject_area * 0.00008)
                and seed_evidence >= max(6, len(pixels) * 0.015)
                and torso_left <= center_x <= torso_right
                and torso_top <= center_y <= torso_bottom
            )
            if valid_torso_component:
                continue
            replacement = min(
                (index for index in neighbors if index != accent_index),
                key=lambda index: (-neighbors[index], index),
                default=structure_index,
            )
            for offset in pixels:
                indices[offset] = replacement
            report["accent_removed_components"] += 1
            report["accent_recolored_pixels"] += len(pixels)
        return report

    # A very dark green or brown blouse can initially collapse entirely into
    # the black filament, leaving no accent component for the usual anchor
    # logic below. Seed only chromatic structure-colour pixels in the central
    # torso. Neutral hair, pupils, watches and bases fail the hue test, while
    # the strict torso window keeps them out even when dark.
    if not any(
        color == accent_index and not background[offset]
        for offset, color in enumerate(indices)
    ):
        torso_left = subject_left + subject_width * 0.24
        torso_right = subject_left + subject_width * 0.76
        torso_top = subject_top + subject_height * 0.28
        torso_bottom = subject_top + subject_height * 0.78
        for offset, color in enumerate(indices):
            if color != structure_index or background[offset]:
                continue
            x, y = offset % width, offset // width
            if not (torso_left <= x <= torso_right and torso_top <= y <= torso_bottom):
                continue
            if not shares_accent_hue(source_pixels[offset]):
                continue
            indices[offset] = accent_index
            report["accent_seeded_pixels"] += 1
            report["accent_recolored_pixels"] += 1

    visited = bytearray(len(indices))
    components: list[dict[str, Any]] = []
    for seed, color in enumerate(indices):
        if color != accent_index or background[seed] or visited[seed]:
            continue
        visited[seed] = 1
        pending: deque[int] = deque([seed])
        pixels: list[int] = []
        neighbors: Counter[int] = Counter()
        source_hue_pixels = 0
        left = right = seed % width
        top = bottom = seed // width
        while pending:
            offset = pending.popleft()
            pixels.append(offset)
            source_hue_pixels += shares_accent_hue(source_pixels[offset])
            x, y = offset % width, offset // width
            left, right = min(left, x), max(right, x)
            top, bottom = min(top, y), max(bottom, y)
            for neighbor in (
                offset - 1 if x else -1,
                offset + 1 if x + 1 < width else -1,
                offset - width if y else -1,
                offset + width if y + 1 < height else -1,
            ):
                if neighbor < 0 or background[neighbor]:
                    continue
                if indices[neighbor] == accent_index:
                    if not visited[neighbor]:
                        visited[neighbor] = 1
                        pending.append(neighbor)
                else:
                    neighbors[indices[neighbor]] += 1
        components.append({
            "pixels": pixels,
            "area": len(pixels),
            "left": left,
            "right": right,
            "top": top,
            "bottom": bottom,
            "center_x": (left + right) / 2,
            "center_y": (top + bottom) / 2,
            "neighbors": neighbors,
            "source_hue_pixels": source_hue_pixels,
        })
    if not components:
        return report
    # The largest accent-colour island is not necessarily the secondary
    # garment. A broad charcoal pedestal reflection can quantize to green and
    # become larger than a narrow blouse. Prefer a central torso component
    # that is supported by the source garment hue; only fall back to the old
    # largest-component rule when no such evidence exists.
    torso_anchor_candidates = [
        component
        for component in components
        if (
            subject_left + subject_width * 0.18
            <= component["center_x"]
            <= subject_left + subject_width * 0.82
            and subject_top + subject_height * 0.22
            <= component["center_y"]
            <= subject_top + subject_height * 0.82
            and component["source_hue_pixels"]
            >= max(8, round(component["area"] * 0.01))
        )
    ]
    anchor = max(
        torso_anchor_candidates or components,
        key=lambda component: (component["source_hue_pixels"], component["area"]),
    )
    report["accent_anchor_source_hue_pixels"] = anchor["source_hue_pixels"]
    report["accent_anchor_in_torso"] = int(anchor in torso_anchor_candidates)

    # Very dark folds in a saturated blouse are often closer to the black
    # filament than to the garment filament in lightness-aware colour space.
    # Keeping those pixels produces large camouflage-like patches after the
    # texture is baked onto a model.  Within the already-detected main accent
    # garment, use hue (not lightness) to fold those shaded pixels back into
    # the same printable material.  Neutral black accessories stay black
    # because they have too little chroma or a different hue.
    shadow_margin_x = max(2, round((anchor["right"] - anchor["left"] + 1) * 0.03))
    shadow_margin_y = max(2, round((anchor["bottom"] - anchor["top"] + 1) * 0.03))
    for offset, color in enumerate(indices):
        if color != structure_index or background[offset]:
            continue
        x, y = offset % width, offset // width
        if not (
            anchor["left"] - shadow_margin_x <= x <= anchor["right"] + shadow_margin_x
            and anchor["top"] - shadow_margin_y <= y <= anchor["bottom"] + shadow_margin_y
        ):
            continue
        if shares_accent_hue(source_pixels[offset]):
            indices[offset] = accent_index
            report["accent_shadow_recolored_pixels"] += 1
            report["accent_recolored_pixels"] += 1

    # Some provider folds are nearly neutral after relighting, so hue alone
    # cannot distinguish them from the black filament.  Merge only sizeable
    # dark islands that sit inside the main blouse and have a strong green
    # boundary.  The minimum area deliberately preserves small black details
    # such as a watch, button, eye, or microphone.
    anchor_width = max(1, anchor["right"] - anchor["left"] + 1)
    anchor_height = max(1, anchor["bottom"] - anchor["top"] + 1)
    minimum_neutral_shadow_area = max(64, round(anchor["area"] * 0.002))
    maximum_neutral_shadow_area = max(
        minimum_neutral_shadow_area,
        round(anchor["area"] * 0.35),
    )
    structure_visited = bytearray(len(indices))
    for seed, color in enumerate(indices):
        if color != structure_index or background[seed] or structure_visited[seed]:
            continue
        structure_visited[seed] = 1
        pending = deque([seed])
        pixels: list[int] = []
        accent_boundary = 0
        left = right = seed % width
        top = bottom = seed // width
        while pending:
            offset = pending.popleft()
            pixels.append(offset)
            x, y = offset % width, offset // width
            left, right = min(left, x), max(right, x)
            top, bottom = min(top, y), max(bottom, y)
            for neighbor in (
                offset - 1 if x else -1,
                offset + 1 if x + 1 < width else -1,
                offset - width if y else -1,
                offset + width if y + 1 < height else -1,
            ):
                if neighbor < 0 or background[neighbor]:
                    continue
                if indices[neighbor] == structure_index:
                    if not structure_visited[neighbor]:
                        structure_visited[neighbor] = 1
                        pending.append(neighbor)
                elif indices[neighbor] == accent_index:
                    accent_boundary += 1
        area = len(pixels)
        center_x = (left + right) * 0.5
        center_y = (top + bottom) * 0.5
        required_boundary = max(8, round(math.sqrt(area) * 0.75))
        if not (
            minimum_neutral_shadow_area <= area <= maximum_neutral_shadow_area
            and accent_boundary >= required_boundary
            and anchor["left"] <= center_x <= anchor["right"]
            and anchor["top"] <= center_y <= anchor["bottom"]
            and right - left + 1 <= anchor_width * 0.75
            and bottom - top + 1 <= anchor_height * 0.75
        ):
            continue
        for offset in pixels:
            indices[offset] = accent_index
        report["accent_neutral_shadow_recolored_pixels"] += area
        report["accent_shadow_recolored_pixels"] += area
        report["accent_recolored_pixels"] += area

    margin_x = subject_width * 0.08
    margin_y = subject_height * 0.08
    minimum_area = max(24, round(subject_area * 0.0001))
    for component in components:
        if component is anchor:
            continue
        near_anchor = (
            anchor["left"] - margin_x <= component["center_x"] <= anchor["right"] + margin_x
            and anchor["top"] - margin_y <= component["center_y"] <= anchor["bottom"] + margin_y
        )
        in_torso = (
            subject_left + subject_width * 0.18 <= component["center_x"] <= subject_left + subject_width * 0.82
            and subject_top + subject_height * 0.28 <= component["center_y"] <= subject_top + subject_height * 0.82
        )
        if near_anchor and in_torso and component["area"] >= minimum_area:
            continue
        neighbors = component["neighbors"]
        replacement = min(
            (index for index in neighbors if index != accent_index),
            key=lambda index: (-neighbors[index], index),
            default=structure_index,
        )
        for offset in component["pixels"]:
            indices[offset] = replacement
        report["accent_removed_components"] += 1
        report["accent_recolored_pixels"] += component["area"]
    return report


def _stabilize_clean_portrait_skin_components(
    indices: bytearray,
    background: bytes | bytearray,
    source_pixels: list[tuple[int, int, int]],
    width: int,
    height: int,
    palette: tuple[str, ...],
    portrait_report: Mapping[str, Any],
) -> dict[str, int]:
    """Keep a portrait face and at most compact hand-shaped skin regions.

    The first skin pass runs before the printable mode/component filters, where
    a real hand can be split into many small pieces. At this later stage those
    pieces are coherent again, so shape and area can reliably distinguish a
    hand from long warm folds in a cream jacket.
    """

    result = {
        "postclean_skin_removed_components": 0,
        "postclean_skin_recolored_pixels": 0,
        "postclean_face_skin_recolored_pixels": 0,
        "postclean_face_structure_recolored_pixels": 0,
    }
    if (
        portrait_report.get("activated") != 1
        or len(indices) != width * height
        or len(background) != len(indices)
        or len(source_pixels) != len(indices)
    ):
        return result
    garment_color = str(portrait_report.get("garment_color", "")).upper()
    skin_color = str(portrait_report.get("skin_color", "")).upper()
    face_bounds = portrait_report.get("face_bounds")
    if garment_color not in palette or skin_color not in palette or not isinstance(face_bounds, Mapping):
        return result
    try:
        face_left = int(face_bounds["left"])
        face_right = int(face_bounds["right"])
        face_top = int(face_bounds["top"])
        face_bottom = int(face_bounds["bottom"])
    except (KeyError, TypeError, ValueError):
        return result
    garment_index = palette.index(garment_color)
    skin_index = palette.index(skin_color)
    structure_index = min(range(len(palette)), key=lambda index: sum(_hex_rgb(palette[index])))
    subject_area = sum(not value for value in background)
    if not subject_area:
        return result

    visited = bytearray(len(indices))
    components: list[dict[str, Any]] = []
    for seed, color in enumerate(indices):
        if color != skin_index or background[seed] or visited[seed]:
            continue
        visited[seed] = 1
        pending: deque[int] = deque([seed])
        pixels: list[int] = []
        left = right = seed % width
        top = bottom = seed // width
        while pending:
            offset = pending.popleft()
            pixels.append(offset)
            x, y = offset % width, offset // width
            left, right = min(left, x), max(right, x)
            top, bottom = min(top, y), max(bottom, y)
            for neighbor in (
                offset - 1 if x else -1,
                offset + 1 if x + 1 < width else -1,
                offset - width if y else -1,
                offset + width if y + 1 < height else -1,
            ):
                if (
                    neighbor >= 0
                    and not background[neighbor]
                    and not visited[neighbor]
                    and indices[neighbor] == skin_index
                ):
                    visited[neighbor] = 1
                    pending.append(neighbor)
        components.append({
            "pixels": pixels,
            "area": len(pixels),
            "left": left,
            "right": right,
            "top": top,
            "bottom": bottom,
            "center_x": (left + right) / 2,
            "center_y": (top + bottom) / 2,
        })
    if not components:
        return result

    face_candidates = [
        component for component in components
        if component["right"] >= face_left
        and component["left"] <= face_right
        and component["bottom"] >= face_top
        and component["top"] <= face_bottom
    ]
    face = max(face_candidates or components, key=lambda component: component["area"])
    face_height = max(1, face_bottom - face_top + 1)
    if portrait_report.get("strong_skin_fallback_used") == 1:
        foreground = [offset for offset, is_background in enumerate(background) if not is_background]
        subject_top = min(offset // width for offset in foreground)
        subject_bottom = max(offset // width for offset in foreground) + 1
        maximum_hand_center_y = subject_top + (subject_bottom - subject_top) * 0.84
    else:
        maximum_hand_center_y = face_bottom + face_height * 0.8
    skin_red, skin_green, skin_blue = _hex_rgb(skin_color)
    minimum_skin_red_green = max(18, round((skin_red - skin_green) * 0.65))
    minimum_skin_red_blue = max(35, round((skin_red - skin_blue) * 0.55))

    # Bright skin highlights can be quantized to the neutral garment filament,
    # leaving white islands on a forehead or cheek. Fill only warm, enclosed
    # garment islands in the upper face envelope. Eye/teeth highlights stay
    # white even under warm lighting; jacket regions touch the envelope
    # boundary or continue into its lower part and are not recolored.
    face_width = max(1, face_right - face_left + 1)
    face_cleanup_bottom = min(face_bottom, round(face_top + face_height * 0.68))
    visited_face_garment: set[int] = set()
    for seed, color in enumerate(indices):
        if color != garment_index or background[seed] or seed in visited_face_garment:
            continue
        seed_x, seed_y = seed % width, seed // width
        if not (
            face_left <= seed_x <= face_right
            and face_top <= seed_y <= face_cleanup_bottom
        ):
            continue
        visited_face_garment.add(seed)
        pending: deque[int] = deque([seed])
        pixels: list[int] = []
        warm_pixels = 0
        touches_boundary = False
        skin_neighbors = 0
        structure_neighbors = 0
        while pending:
            offset = pending.popleft()
            pixels.append(offset)
            x, y = offset % width, offset // width
            touches_boundary = touches_boundary or (
                x in (face_left, face_right) or y in (face_top, face_cleanup_bottom)
            )
            red, green, blue = source_pixels[offset]
            if red > green >= blue and red - green >= 5 and red - blue >= 12:
                warm_pixels += 1
            for neighbor in (
                offset - 1 if x else -1,
                offset + 1 if x + 1 < width else -1,
                offset - width if y else -1,
                offset + width if y + 1 < height else -1,
            ):
                if neighbor < 0 or background[neighbor]:
                    continue
                neighbor_x, neighbor_y = neighbor % width, neighbor // width
                if indices[neighbor] == skin_index:
                    skin_neighbors += 1
                elif indices[neighbor] == structure_index:
                    structure_neighbors += 1
                if not (
                    face_left <= neighbor_x <= face_right
                    and face_top <= neighbor_y <= face_cleanup_bottom
                    and indices[neighbor] == garment_index
                    and neighbor not in visited_face_garment
                ):
                    continue
                visited_face_garment.add(neighbor)
                pending.append(neighbor)
        feature_boundary = structure_neighbors >= max(2, round(math.sqrt(len(pixels)) * 0.20))
        if (
            not touches_boundary
            and not feature_boundary
            and skin_neighbors
            and len(pixels) >= 4
            and warm_pixels >= max(4, math.ceil(len(pixels) * 0.70))
        ):
            for offset in pixels:
                indices[offset] = skin_index
            result["postclean_face_skin_recolored_pixels"] += len(pixels)

    # Remove isolated dark material dots from the central forehead. Eyes,
    # brows, nostrils and the mouth sit lower; hair strands remain connected to
    # the large hair component. A tiny disconnected structure component in
    # this otherwise continuous skin area is a quantization artifact that can
    # become a conspicuous black plug in the printed face.
    forehead_left = round(face_left + face_width * 0.18)
    forehead_right = round(face_right - face_width * 0.18)
    forehead_top = round(face_top + face_height * 0.12)
    forehead_bottom = round(face_top + face_height * 0.26)
    maximum_forehead_speckle = max(16, round(face_width * face_height * 0.001))
    visited_face_structure = bytearray(len(indices))
    for seed, color in enumerate(indices):
        if color != structure_index or background[seed] or visited_face_structure[seed]:
            continue
        seed_x, seed_y = seed % width, seed // width
        if not (
            forehead_left <= seed_x <= forehead_right
            and forehead_top <= seed_y <= forehead_bottom
        ):
            continue
        visited_face_structure[seed] = 1
        pending = deque([seed])
        pixels: list[int] = []
        component_left = component_right = seed_x
        component_top = component_bottom = seed_y
        skin_neighbors = 0
        while pending:
            offset = pending.popleft()
            pixels.append(offset)
            x, y = offset % width, offset // width
            component_left, component_right = min(component_left, x), max(component_right, x)
            component_top, component_bottom = min(component_top, y), max(component_bottom, y)
            for neighbor in (
                offset - 1 if x else -1,
                offset + 1 if x + 1 < width else -1,
                offset - width if y else -1,
                offset + width if y + 1 < height else -1,
            ):
                if neighbor < 0 or background[neighbor]:
                    continue
                if indices[neighbor] == skin_index:
                    skin_neighbors += 1
                elif indices[neighbor] == structure_index and not visited_face_structure[neighbor]:
                    visited_face_structure[neighbor] = 1
                    pending.append(neighbor)
        if (
            len(pixels) <= maximum_forehead_speckle
            and component_left >= forehead_left
            and component_right <= forehead_right
            and component_top >= forehead_top
            and component_bottom <= forehead_bottom
            and skin_neighbors
        ):
            for offset in pixels:
                indices[offset] = skin_index
            result["postclean_face_structure_recolored_pixels"] += len(pixels)

    hand_candidates: list[dict[str, Any]] = []
    evaluated_components: list[tuple[dict[str, Any], bool, bool, float, float, int]] = []
    for component in components:
        if component is face:
            continue
        component_width = component["right"] - component["left"] + 1
        component_height = component["bottom"] - component["top"] + 1
        aspect = max(component_width, component_height) / max(1, min(component_width, component_height))
        fill_ratio = component["area"] / max(1, component_width * component_height)
        strong_source_pixels = 0
        for offset in component["pixels"]:
            red, green, blue = source_pixels[offset]
            maximum, minimum = max(red, green, blue), min(red, green, blue)
            saturation = (maximum - minimum) / maximum if maximum else 0.0
            if (
                saturation >= 0.26
                and red > green >= blue
                and red - green >= minimum_skin_red_green
                and red - blue >= minimum_skin_red_blue
            ):
                strong_source_pixels += 1
        compact_hand = (
            component["center_y"] <= maximum_hand_center_y
            and component["area"] >= max(128, subject_area * 0.002)
            and aspect <= 3.5
            and fill_ratio >= 0.35
        )
        source_visible_hand = (
            component["center_y"] <= maximum_hand_center_y
            and component["area"] >= max(64, subject_area * 0.0005)
            and aspect <= 5.0
            and fill_ratio >= 0.15
            and strong_source_pixels >= max(24, component["area"] * 0.25)
        )
        evaluated_components.append(
            (component, compact_hand, source_visible_hand, aspect, fill_ratio, strong_source_pixels)
        )
        if compact_hand or source_visible_hand:
            hand_candidates.append(component)

    largest_hand_area = max((component["area"] for component in hand_candidates), default=0)
    for component, compact_hand, source_visible_hand, aspect, fill_ratio, strong_source_pixels in evaluated_components:
        strong_small_hand = (
            aspect <= 2.5
            and fill_ratio >= 0.35
            and strong_source_pixels >= max(24, component["area"] * 0.50)
        )
        meaningful_relative_size = (
            largest_hand_area > 0 and component["area"] >= largest_hand_area * 0.45
        )
        if (compact_hand or source_visible_hand) and (meaningful_relative_size or strong_small_hand):
            continue
        for offset in component["pixels"]:
            indices[offset] = garment_index
        result["postclean_skin_removed_components"] += 1
        result["postclean_skin_recolored_pixels"] += component["area"]
    return result


def _stabilize_portrait_base_components(
    indices: bytearray,
    background: bytes | bytearray,
    source_pixels: list[tuple[int, int, int]],
    width: int,
    height: int,
    palette: tuple[str, ...],
    portrait_report: Mapping[str, Any],
) -> dict[str, Any]:
    """Keep a detected low portrait pedestal in one structure material.

    Dark studio highlights on a black base can be perceptually closer to a
    green or brown garment filament.  Because the pedestal is wide, low and
    connected, its material ownership is less ambiguous than its pixel colour.
    Detect that geometry from the darkest material, then fold only dark pixels
    inside its row spans back into the same structure material.  Bright jacket
    pixels that overlap the top surface remain untouched.
    """

    result: dict[str, Any] = {
        "base_color": "",
        "base_recolored_pixels": 0,
        "base_bounds": {},
    }
    if (
        portrait_report.get("activated") != 1
        or len(indices) != width * height
        or len(background) != len(indices)
        or len(source_pixels) != len(indices)
        or not palette
    ):
        return result
    structure_index = min(range(len(palette)), key=lambda index: sum(_hex_rgb(palette[index])))
    result["base_color"] = palette[structure_index]
    foreground = [offset for offset, value in enumerate(background) if not value]
    if not foreground:
        return result
    subject_left = min(offset % width for offset in foreground)
    subject_right = max(offset % width for offset in foreground)
    subject_top = min(offset // width for offset in foreground)
    subject_bottom = max(offset // width for offset in foreground)
    subject_width = max(1, subject_right - subject_left + 1)
    subject_height = max(1, subject_bottom - subject_top + 1)
    subject_area = len(foreground)

    visited = bytearray(len(indices))
    candidates: list[dict[str, Any]] = []
    for seed, color in enumerate(indices):
        if color != structure_index or background[seed] or visited[seed]:
            continue
        visited[seed] = 1
        pending: deque[int] = deque([seed])
        pixels: list[int] = []
        left = right = seed % width
        top = bottom = seed // width
        row_spans: dict[int, list[int]] = {}
        while pending:
            offset = pending.popleft()
            pixels.append(offset)
            x, y = offset % width, offset // width
            left, right = min(left, x), max(right, x)
            top, bottom = min(top, y), max(bottom, y)
            span = row_spans.setdefault(y, [x, x])
            span[0], span[1] = min(span[0], x), max(span[1], x)
            for neighbor in (
                offset - 1 if x else -1,
                offset + 1 if x + 1 < width else -1,
                offset - width if y else -1,
                offset + width if y + 1 < height else -1,
            ):
                if (
                    neighbor >= 0
                    and not background[neighbor]
                    and not visited[neighbor]
                    and indices[neighbor] == structure_index
                ):
                    visited[neighbor] = 1
                    pending.append(neighbor)
        component_width = right - left + 1
        component_height = bottom - top + 1
        center_y = (top + bottom) * 0.5
        if (
            center_y >= subject_top + subject_height * 0.72
            and component_width >= subject_width * 0.45
            and component_width >= component_height * 2.0
            and len(pixels) >= max(64, round(subject_area * 0.015))
        ):
            candidates.append({
                "area": len(pixels),
                "left": left,
                "right": right,
                "top": top,
                "bottom": bottom,
                "row_spans": row_spans,
            })
    if not candidates:
        return result
    base = max(candidates, key=lambda component: (component["area"], component["bottom"]))
    result["base_bounds"] = {
        "left": base["left"],
        "right": base["right"],
        "top": base["top"],
        "bottom": base["bottom"],
    }
    # Use the complete low pedestal envelope, not only each row's surviving
    # structure-colour span.  A wrong reflection can occupy the whole left or
    # right end of the top ellipse, leaving no structure pixel on that side in
    # the same row.  Source luminance remains the safety gate for overlapping
    # bright jacket pixels.
    lateral_margin = max(1, round(subject_width * 0.01))
    base_left = max(subject_left, base["left"] - lateral_margin)
    base_right = min(subject_right, base["right"] + lateral_margin)
    for y in range(base["top"], base["bottom"] + 1):
        for x in range(base_left, base_right + 1):
            offset = y * width + x
            if background[offset] or indices[offset] == structure_index:
                continue
            red, green, blue = source_pixels[offset]
            # Preserve the bright white jacket where the torso intersects the
            # base top, but absorb dark green/skin/neutral reflection pixels.
            lower_pedestal = y >= base["top"] + (base["bottom"] - base["top"] + 1) * 0.45
            if (
                not lower_pedestal
                and (max(red, green, blue) >= 190 or (red + green + blue) / 3 >= 165)
            ):
                continue
            indices[offset] = structure_index
            result["base_recolored_pixels"] += 1
    return result


def _restore_clean_portrait_smile(
    indices: bytearray,
    strict_indices: bytes,
    background: bytes,
    width: int,
    height: int,
    palette: tuple[str, ...],
    portrait_report: Mapping[str, Any],
) -> dict[str, int]:
    """Restore one printable tooth band removed by generic small-region cleanup."""

    result = {"smile_restored_pixels": 0}
    if (
        portrait_report.get("activated") != 1
        or len(indices) != width * height
        or len(strict_indices) != len(indices)
        or len(background) != len(indices)
    ):
        return result
    garment_color = str(portrait_report.get("garment_color", "")).upper()
    if garment_color not in palette:
        return result
    face_bounds = portrait_report.get("face_bounds", {})
    if not isinstance(face_bounds, Mapping):
        return result
    try:
        left = max(0, int(face_bounds["left"]))
        right = min(width - 1, int(face_bounds["right"]))
        top = max(0, int(face_bounds["top"]))
        bottom = min(height - 1, int(face_bounds["bottom"]))
    except (KeyError, TypeError, ValueError):
        return result
    face_width = right - left + 1
    face_height = bottom - top + 1
    if face_width < 8 or face_height < 12:
        return result
    primary_index = palette.index(garment_color)
    center_x = (left + right) * 0.5
    smile_left = max(left, math.floor(center_x - face_width * 0.22))
    smile_right = min(right, math.ceil(center_x + face_width * 0.22))
    smile_top = max(top, math.floor(top + face_height * 0.38))
    smile_bottom = min(bottom, math.ceil(top + face_height * 0.62))
    candidates = bytearray(width * height)
    for y in range(smile_top, smile_bottom + 1):
        row = y * width
        for x in range(smile_left, smile_right + 1):
            offset = row + x
            if not background[offset] and strict_indices[offset] == primary_index:
                candidates[offset] = 1

    visited = bytearray(width * height)
    components: list[tuple[float, list[int]]] = []
    maximum_area = max(12, math.ceil(face_width * face_height * 0.04))
    for start, enabled in enumerate(candidates):
        if not enabled or visited[start]:
            continue
        stack = [start]
        visited[start] = 1
        pixels: list[int] = []
        component_left = width
        component_right = -1
        component_top = height
        component_bottom = -1
        while stack:
            offset = stack.pop()
            pixels.append(offset)
            x = offset % width
            y = offset // width
            component_left = min(component_left, x)
            component_right = max(component_right, x)
            component_top = min(component_top, y)
            component_bottom = max(component_bottom, y)
            for neighbor in (
                offset - 1 if x > 0 else -1,
                offset + 1 if x + 1 < width else -1,
                offset - width if y > 0 else -1,
                offset + width if y + 1 < height else -1,
            ):
                if neighbor >= 0 and candidates[neighbor] and not visited[neighbor]:
                    visited[neighbor] = 1
                    stack.append(neighbor)
        component_width = component_right - component_left + 1
        component_height = component_bottom - component_top + 1
        if (
            4 <= len(pixels) <= maximum_area
            and component_width >= max(3, math.ceil(component_height * 1.35))
        ):
            component_center_x = (component_left + component_right) * 0.5
            component_center_y = (component_top + component_bottom) * 0.5
            expected_y = top + face_height * 0.50
            score = (
                len(pixels)
                - abs(component_center_x - center_x) * 0.5
                - abs(component_center_y - expected_y) * 0.25
            )
            components.append((score, pixels))
    if not components:
        return result
    _, smile = max(components, key=lambda item: item[0])
    for offset in smile:
        indices[offset] = primary_index
    result["smile_restored_pixels"] = len(smile)
    return result


def process_printable_image(
    source_path: str | os.PathLike[str],
    output_directory: str | os.PathLike[str],
    palette: Iterable[str],
    settings: PrintSettings | dict[str, Any] | None = None,
    palette_roles: Mapping[str, str] | None = None,
    subject_reference_path: str | os.PathLike[str] | None = None,
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
            # Keep the provider image's facial and surface detail for image-to-3D.
            # The printable preview below is intentionally reduced to exact
            # filament colors, which is useful for material validation but can
            # erase the landmarks needed to reconstruct a recognizable face.
            detailed_reference = opened.convert("RGBA")
            palette_rgb = [_hex_rgb(color) for color in selected]
            palette_lab = [_srgb_to_lab(color) for color in palette_rgb]
            brightest_index = max(range(len(palette_lab)), key=lambda index: palette_lab[index][0])
            alpha_background: bytes | None = None
            if "A" in opened.getbands():
                rgba = opened.convert("RGBA")
                alpha = rgba.getchannel("A").tobytes()
                candidate = bytes(255 if value <= 8 else 0 for value in alpha)
                alpha_subject_ratio = 1.0 - sum(bool(value) for value in candidate) / len(candidate)
                if 0.05 <= alpha_subject_ratio <= 0.95:
                    alpha_background = candidate
                base = Image.new("RGBA", opened.size, palette_rgb[brightest_index] + (255,))
                base.alpha_composite(rgba)
                rgb = base.convert("RGB")
            else:
                rgb = opened.convert("RGB")
            smoothed = rgb.filter(ImageFilter.MedianFilter(size=3))
            smoothed_pixels = list(smoothed.getdata())
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
            mapped_indices = bytes(index_map.get(index, brightest_index) for index in adaptive_data)
            source_background = alpha_background or _source_boundary_background(rgb)
            strict_indices, portrait_skin_cleanup = _stabilize_portrait_skin_components(
                mapped_indices,
                source_background,
                smoothed_pixels,
                opened.width,
                opened.height,
                selected,
                role_assignment.role_by_color,
            )
            portrait_skin_cleanup["subject_mask_recovered_pixels"] = 0
            portrait_skin_cleanup["source_mask_recovered_pixels"] = 0
            if alpha_background is None and portrait_skin_cleanup.get("activated") == 1:
                repaired_background, recovered_pixels = _repair_portrait_background_mask(
                    source_background, opened.width, opened.height
                )
                if recovered_pixels:
                    source_background = repaired_background
                    strict_indices, portrait_skin_cleanup = _stabilize_portrait_skin_components(
                        mapped_indices,
                        source_background,
                        smoothed_pixels,
                        opened.width,
                        opened.height,
                        selected,
                        role_assignment.role_by_color,
                    )
                    portrait_skin_cleanup["subject_mask_recovered_pixels"] = recovered_pixels
                source_repaired_background, source_recovered_pixels = _repair_portrait_mask_from_source(
                    source_background,
                    opened.width,
                    opened.height,
                    subject_reference_path,
                )
                if source_recovered_pixels:
                    source_background = source_repaired_background
                    strict_indices, portrait_skin_cleanup = _stabilize_portrait_skin_components(
                        mapped_indices,
                        source_background,
                        smoothed_pixels,
                        opened.width,
                        opened.height,
                        selected,
                        role_assignment.role_by_color,
                    )
                    portrait_skin_cleanup["subject_mask_recovered_pixels"] = recovered_pixels
                    portrait_skin_cleanup["source_mask_recovered_pixels"] = source_recovered_pixels
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
            portrait_skin_cleanup.update(
                _stabilize_portrait_accent_components(
                    cleaned,
                    background,
                    smoothed_pixels,
                    opened.width,
                    opened.height,
                    selected,
                    portrait_skin_cleanup,
                )
            )
            portrait_skin_cleanup.update(
                _stabilize_clean_portrait_skin_components(
                    cleaned,
                    background,
                    smoothed_pixels,
                    opened.width,
                    opened.height,
                    selected,
                    portrait_skin_cleanup,
                )
            )
            portrait_skin_cleanup.update(
                _stabilize_portrait_base_components(
                    cleaned,
                    background,
                    smoothed_pixels,
                    opened.width,
                    opened.height,
                    selected,
                    portrait_skin_cleanup,
                )
            )
            portrait_skin_cleanup.update(
                _restore_clean_portrait_smile(
                    cleaned,
                    strict_indices,
                    background,
                    opened.width,
                    opened.height,
                    selected,
                    portrait_skin_cleanup,
                )
            )
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
            model_reference = detailed_reference.copy()
            model_reference.putalpha(subject_mask)
            clean_reference = clean_image.convert("RGBA")
            clean_reference.putalpha(subject_mask)
            # The clean preview is also subject-only. Keeping its background
            # transparent lets the native GUI show light filament regions on a
            # checkerboard without introducing a fifth, non-printable color.
            _atomic_png(clean_reference, clean_path)
            _atomic_png(model_reference, model_reference_path)

            effective_color_by_role = dict(role_assignment.color_by_role)
            if portrait_skin_cleanup.get("activated") == 1:
                garment_color = str(portrait_skin_cleanup.get("garment_color", "")).upper()
                skin_color = str(portrait_skin_cleanup.get("skin_color", "")).upper()
                if garment_color in selected and skin_color in selected and garment_color != skin_color:
                    # The portrait detector is deliberately more semantic than
                    # the generic luminance/chroma role inference. Keep masks,
                    # fragmentation gates, metadata, and later OBJ cleanup on
                    # the recovered material ownership as one consistent truth.
                    # Rebuild the complete assignment instead of overwriting two
                    # dictionary entries. A user can legitimately swap any two
                    # advanced role controls; direct overwrites could then leave
                    # duplicate colors and omit another palette color entirely.
                    effective_color_by_role = assign_palette_roles(
                        selected,
                        {"primary": garment_color, "light": skin_color},
                    ).color_by_role
            effective_role_by_color = {
                color: role for role, color in effective_color_by_role.items()
            }
            roles = [effective_role_by_color[color] for color in selected]
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
            color_component_counts, color_secondary_component_ratios = _color_component_stats(
                cleaned, background, opened.width, opened.height, len(selected)
            )
            subject_area_ratio = subject_total / total
            subject_components = _foreground_components(background, opened.width, opened.height)
            subject_component_areas = [component.area for component in subject_components]
            largest_subject_component_ratio = (
                max(subject_component_areas, default=0) / subject_total if subject_total else 0.0
            )
            largest_component = max(subject_components, key=lambda component: component.area, default=None)
            if subject_components:
                subject_left = min(component.left for component in subject_components)
                subject_top = min(component.top for component in subject_components)
                subject_right = max(component.right for component in subject_components)
                subject_bottom = max(component.bottom for component in subject_components)
                subject_diagonal = math.hypot(
                    subject_right - subject_left + 1,
                    subject_bottom - subject_top + 1,
                )
            else:
                subject_diagonal = 0.0
            detached_diagonals = [
                component.diagonal
                for component in subject_components
                if component is not largest_component
            ]
            largest_detached_subject_diagonal_ratio = (
                max(detached_diagonals, default=0.0) / subject_diagonal
                if subject_diagonal > 0.0 else 0.0
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
            # Area alone misses long, thin detached handles, spokes, and
            # supports. Their pixel area may be tiny while their lost span is
            # structurally important to the later 3D model.
            detached_structure_ok = largest_detached_subject_diagonal_ratio < 0.08
            subject_continuity_ok = (
                largest_subject_component_ratio >= 0.90
                and detached_structure_ok
            )
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
            fragmented_roles = [
                roles[index]
                for index in range(len(selected))
                if roles[index] != "structure"
                and color_component_counts[index] >= 4
                and color_secondary_component_ratios[index] >= 0.01
            ]
            severe_fragmented_roles = [
                roles[index]
                for index in range(len(selected))
                if roles[index] != "structure"
                and color_component_counts[index] >= 12
                and color_secondary_component_ratios[index] >= 0.025
            ]
            portrait_materials_detected = portrait_skin_cleanup.get("activated") == 1
            material_fragmentation_ok = not (portrait_materials_detected and severe_fragmented_roles)
            palette_quality_ok = (
                subject_area_ratio >= 0.18
                and subject_continuity_ok
                and material_fragmentation_ok
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
            if not detached_structure_ok:
                warnings.append("printable_subject_has_large_detached_structure")
            if minimum_feature_px == 1:
                warnings.append("minimum_feature_rounds_to_one_pixel")
            if role_assignment.low_contrast:
                warnings.append("filament_colors_have_low_contrast")
            if fragmented_roles:
                warnings.append("palette_material_is_fragmented")
            if not material_fragmentation_ok:
                warnings.append("portrait_material_fragmentation_blocks_3d")
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
                "subject_color_component_count": {
                    color: color_component_counts[index] for index, color in enumerate(selected)
                },
                "secondary_subject_color_component_ratio": {
                    color: round(color_secondary_component_ratios[index], 6)
                    for index, color in enumerate(selected)
                },
                "fragmented_palette_roles": fragmented_roles,
                "severe_fragmented_palette_roles": severe_fragmented_roles,
                "material_fragmentation_ok": material_fragmentation_ok,
                "portrait_skin_cleanup": portrait_skin_cleanup,
                "meaningful_palette_count": meaningful_palette_count,
                "meaningful_subject_color_count": meaningful_subject_count,
                "palette_diversity_ok": palette_diversity_ok,
                "printable_subject_area_ratio": round(subject_area_ratio, 6),
                "subject_component_count": len(subject_component_areas),
                "meaningful_subject_component_count": meaningful_subject_component_count,
                "largest_subject_component_ratio": round(largest_subject_component_ratio, 6),
                "largest_detached_subject_diagonal_ratio": round(
                    largest_detached_subject_diagonal_ratio, 6
                ),
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
                "palette_roles": {
                    "role_by_color": effective_role_by_color,
                    "color_by_role": effective_color_by_role,
                    "minimum_distance": round(role_assignment.minimum_distance, 4),
                    "low_contrast": role_assignment.low_contrast,
                },
                "background": {"palette_index": background_index, "palette_hex": selected[background_index]},
                "background_detection": "source_alpha" if alpha_background is not None else "boundary_color",
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
