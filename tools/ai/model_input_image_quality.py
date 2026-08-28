"""Conservative local quality checks for image-to-3D reference images.

The evaluator intentionally uses only Pillow.  It is suitable for offline
preflight and benchmark reporting; callers should treat warnings as guidance
and only use ``model_input_eligible`` as a gate for severe, measurable faults.
"""

from __future__ import annotations

from collections import deque
from pathlib import Path
from typing import Any, Iterable

from PIL import Image


QUALITY_SCHEMA_VERSION = 1
ANALYSIS_EDGE = 192


class ModelInputImageQualityError(RuntimeError):
    pass


def _median(values: Iterable[int]) -> int:
    ordered = sorted(values)
    if not ordered:
        return 0
    return ordered[len(ordered) // 2]


def _corner_colors(pixels: list[tuple[int, int, int, int]], width: int, height: int) -> list[tuple[int, int, int]]:
    block = max(2, min(width, height) // 16)
    colors: list[tuple[int, int, int]] = []
    for left, top in ((0, 0), (width - block, 0), (0, height - block), (width - block, height - block)):
        sample = [
            pixels[y * width + x]
            for y in range(top, top + block)
            for x in range(left, left + block)
            if pixels[y * width + x][3] >= 240
        ]
        if sample:
            colors.append(tuple(_median(pixel[channel] for pixel in sample) for channel in range(3)))
    return colors or [(255, 255, 255)]


def _color_distance_squared(pixel: tuple[int, int, int, int], color: tuple[int, int, int]) -> int:
    return sum((pixel[channel] - color[channel]) ** 2 for channel in range(3))


def _component_areas(mask: list[bool], width: int, height: int) -> list[int]:
    visited = bytearray(width * height)
    areas: list[int] = []
    for start, foreground in enumerate(mask):
        if not foreground or visited[start]:
            continue
        visited[start] = 1
        queue = deque([start])
        area = 0
        while queue:
            offset = queue.popleft()
            area += 1
            x = offset % width
            y = offset // width
            if x and mask[offset - 1] and not visited[offset - 1]:
                visited[offset - 1] = 1
                queue.append(offset - 1)
            if x + 1 < width and mask[offset + 1] and not visited[offset + 1]:
                visited[offset + 1] = 1
                queue.append(offset + 1)
            if y and mask[offset - width] and not visited[offset - width]:
                visited[offset - width] = 1
                queue.append(offset - width)
            if y + 1 < height and mask[offset + width] and not visited[offset + width]:
                visited[offset + width] = 1
                queue.append(offset + width)
        areas.append(area)
    return sorted(areas, reverse=True)


def assess_model_input_image(path: str | Path) -> dict[str, Any]:
    """Return stable metrics, warnings and severe blockers for a reference image."""

    try:
        with Image.open(path) as opened:
            opened.load()
            original_width, original_height = opened.size
            image = opened.convert("RGBA")
    except (OSError, ValueError) as exc:
        raise ModelInputImageQualityError("The model reference image cannot be decoded.") from exc

    if original_width < 1 or original_height < 1:
        raise ModelInputImageQualityError("The model reference image has no pixels.")
    image.thumbnail((ANALYSIS_EDGE, ANALYSIS_EDGE), Image.Resampling.LANCZOS)
    width, height = image.size
    raw = image.tobytes()
    pixels = list(zip(raw[0::4], raw[1::4], raw[2::4], raw[3::4]))
    total = len(pixels)

    transparent_pixel_ratio = sum(pixel[3] < 240 for pixel in pixels) / total
    uses_transparency = transparent_pixel_ratio >= 0.01
    corner_colors = _corner_colors(pixels, width, height)
    background_threshold_squared = 42 ** 2
    if uses_transparency:
        mask = [pixel[3] >= 32 for pixel in pixels]
    else:
        mask = [
            min(_color_distance_squared(pixel, color) for color in corner_colors) > background_threshold_squared
            for pixel in pixels
        ]

    foreground_offsets = [offset for offset, foreground in enumerate(mask) if foreground]
    foreground_count = len(foreground_offsets)
    foreground_ratio = foreground_count / total
    soft_alpha_count = sum(16 < pixel[3] < 240 for pixel in pixels)
    soft_alpha_ratio = soft_alpha_count / max(1, foreground_count)

    border_offsets = set(range(width))
    border_offsets.update(range((height - 1) * width, height * width))
    border_offsets.update(y * width for y in range(height))
    border_offsets.update(y * width + width - 1 for y in range(height))
    border_foreground_ratio = sum(mask[offset] for offset in border_offsets) / max(1, len(border_offsets))
    border_background_ratio = sum(
        min(_color_distance_squared(pixels[offset], color) for color in corner_colors) <= background_threshold_squared
        for offset in border_offsets
    ) / max(1, len(border_offsets))

    if foreground_offsets:
        xs = [offset % width for offset in foreground_offsets]
        ys = [offset // width for offset in foreground_offsets]
        left, right = min(xs), max(xs)
        top, bottom = min(ys), max(ys)
        bbox_width = right - left + 1
        bbox_height = bottom - top + 1
        bbox_area_ratio = bbox_width * bbox_height / total
        margins = {
            "left": left / width,
            "right": (width - 1 - right) / width,
            "top": top / height,
            "bottom": (height - 1 - bottom) / height,
        }
    else:
        bbox_area_ratio = 0.0
        margins = {"left": 0.0, "right": 0.0, "top": 0.0, "bottom": 0.0}

    areas = _component_areas(mask, width, height)
    meaningful_threshold = max(4, int(foreground_count * 0.005))
    meaningful_areas = [area for area in areas if area >= meaningful_threshold]
    largest_component_ratio = areas[0] / max(1, foreground_count) if areas else 0.0

    warnings: list[str] = []
    blockers: list[str] = []
    if foreground_ratio < 0.03:
        blockers.append("subject_not_detected")
    elif foreground_ratio < 0.08:
        blockers.append("subject_too_small")
    elif foreground_ratio < 0.16:
        warnings.append("subject_small")
    if foreground_ratio > 0.94:
        blockers.append("subject_or_background_fills_frame")
    elif foreground_ratio > 0.88:
        warnings.append("subject_nearly_fills_frame")
    if border_foreground_ratio > 0.18:
        blockers.append("subject_cropped")
    elif border_foreground_ratio > 0.04:
        warnings.append("subject_close_to_frame")
    if largest_component_ratio < 0.60 and len(meaningful_areas) > 1:
        blockers.append("fragmented_subject")
    elif largest_component_ratio < 0.88 or len(meaningful_areas) > 4:
        warnings.append("multiple_or_fragmented_components")
    if uses_transparency and soft_alpha_ratio > 0.30:
        blockers.append("excessive_semitransparency")
    elif uses_transparency and soft_alpha_ratio > 0.08:
        warnings.append("soft_transparent_edges")
    if not uses_transparency and border_background_ratio < 0.45:
        blockers.append("background_not_isolated")
    elif not uses_transparency and border_background_ratio < 0.75:
        warnings.append("background_may_be_complex")

    score = 100.0
    penalty_by_flag = {
        "subject_not_detected": 70,
        "subject_too_small": 45,
        "subject_small": 15,
        "subject_or_background_fills_frame": 40,
        "subject_nearly_fills_frame": 15,
        "subject_cropped": 35,
        "subject_close_to_frame": 12,
        "fragmented_subject": 35,
        "multiple_or_fragmented_components": 12,
        "excessive_semitransparency": 30,
        "soft_transparent_edges": 8,
        "background_not_isolated": 30,
        "background_may_be_complex": 10,
    }
    for flag in blockers + warnings:
        score -= penalty_by_flag[flag]

    return {
        "schema_version": QUALITY_SCHEMA_VERSION,
        "score": round(max(0.0, score), 1),
        "model_input_eligible": not blockers,
        "blockers": blockers,
        "warnings": warnings,
        "metrics": {
            "analysis_width": width,
            "analysis_height": height,
            "foreground_ratio": round(foreground_ratio, 6),
            "foreground_bbox_area_ratio": round(bbox_area_ratio, 6),
            "border_foreground_ratio": round(border_foreground_ratio, 6),
            "border_background_ratio": round(border_background_ratio, 6),
            "largest_component_ratio": round(largest_component_ratio, 6),
            "meaningful_component_count": len(meaningful_areas),
            "transparent_pixel_ratio": round(transparent_pixel_ratio, 6),
            "soft_alpha_ratio": round(soft_alpha_ratio, 6),
            "margins": {name: round(value, 6) for name, value in margins.items()},
        },
    }
