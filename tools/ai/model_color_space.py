"""Color distance and palette selection; no file, job or UI dependencies."""
from __future__ import annotations

import math
from typing import Mapping

def _srgb_to_lab(color: tuple[int, int, int]) -> tuple[float, float, float]:
    linear = []
    for channel in color:
        value = channel / 255.0
        linear.append(value / 12.92 if value <= 0.04045 else ((value + 0.055) / 1.055) ** 2.4)
    red, green, blue = linear
    x = (0.4124564 * red + 0.3575761 * green + 0.1804375 * blue) / 0.95047
    y = 0.2126729 * red + 0.7151522 * green + 0.0721750 * blue
    z = (0.0193339 * red + 0.1191920 * green + 0.9503041 * blue) / 1.08883

    def transform(value: float) -> float:
        return value ** (1.0 / 3.0) if value > 0.008856 else 7.787 * value + 16.0 / 116.0

    fx, fy, fz = transform(x), transform(y), transform(z)
    return 116.0 * fy - 16.0, 500.0 * (fx - fy), 200.0 * (fy - fz)

def _palette_data(palette: tuple[str, ...]) -> tuple[list[tuple[int, int, int]], list[tuple[float, float, float]]]:
    colors = [tuple(int(color[index : index + 2], 16) for index in (1, 3, 5)) for color in palette]
    return colors, [_srgb_to_lab(color) for color in colors]

def _vertex_color_data(
    palette: tuple[str, ...],
) -> tuple[list[tuple[int, int, int]], list[tuple[float, float, float]]]:
    return _palette_data(palette) if palette else ([], [])

def _nearest_palette_index(
    color: tuple[int, int, int],
    palette_lab: list[tuple[float, float, float]],
    cache: dict[tuple[int, int, int], int],
) -> int:
    cached = cache.get(color)
    if cached is not None:
        return cached
    lab = _srgb_to_lab(color)
    index = min(
        range(len(palette_lab)),
        key=lambda item: sum((lab[channel] - palette_lab[item][channel]) ** 2 for channel in range(3)),
    )
    cache[color] = index
    return index

def _portrait_material_palette_indices(
    palette: tuple[str, ...],
    palette_roles: Mapping[str, str] | None,
    portrait_materials: bool,
) -> tuple[int, int] | None:
    """Return the neutral garment and skin indices for an explicitly detected portrait.

    This deliberately requires both upstream portrait evidence and a very specific
    material palette. It must never change generic nearest-colour behaviour for
    non-portrait jobs or palettes where white and warm skin are not unambiguous.
    """
    if not portrait_materials or not palette or not palette_roles:
        return None
    try:
        primary = str(palette_roles["primary"]).strip().upper()
        skin = str(palette_roles["light"]).strip().upper()
        primary_index = palette.index(primary)
        skin_index = palette.index(skin)
    except (KeyError, ValueError):
        return None
    if primary_index == skin_index:
        return None
    _, labs = _palette_data(palette)
    primary_lab = labs[primary_index]
    skin_lab = labs[skin_index]
    primary_chroma = math.hypot(primary_lab[1], primary_lab[2])
    skin_chroma = math.hypot(skin_lab[1], skin_lab[2])
    if primary_lab[0] < 82.0 or primary_chroma > 12.0:
        return None
    if skin_lab[1] < 5.0 or skin_lab[2] < 8.0 or skin_chroma < 16.0:
        return None
    return primary_index, skin_index

def _portrait_material_role_indices(
    palette: tuple[str, ...],
    palette_roles: Mapping[str, str] | None,
    portrait_materials: bool,
) -> dict[str, int] | None:
    if _portrait_material_palette_indices(palette, palette_roles, portrait_materials) is None:
        return None
    try:
        result = {
            role: palette.index(str((palette_roles or {})[role]).strip().upper())
            for role in ("primary", "structure", "light", "accent")
        }
    except (KeyError, ValueError):
        return None
    return result if len(set(result.values())) == 4 else None

def _semantic_palette_index(
    color: tuple[int, int, int],
    palette_lab: list[tuple[float, float, float]],
    cache: dict[tuple[int, int, int], int],
    portrait_indices: tuple[int, int] | None,
    portrait_role_indices: Mapping[str, int] | None = None,
) -> int:
    nearest = _nearest_palette_index(color, palette_lab, cache)
    if portrait_indices is None:
        return nearest
    if portrait_role_indices is not None and nearest == portrait_role_indices.get("structure"):
        accent_index = portrait_role_indices.get("accent")
        if accent_index is not None:
            lab = _srgb_to_lab(color)
            accent_lab = palette_lab[accent_index]
            source_chroma = math.hypot(lab[1], lab[2])
            accent_chroma = math.hypot(accent_lab[1], accent_lab[2])
            hue_similarity = (
                (lab[1] * accent_lab[1] + lab[2] * accent_lab[2])
                / (source_chroma * accent_chroma)
                if source_chroma > 1e-9 and accent_chroma > 1e-9
                else -1.0
            )
            # Deep folds of a green blouse can be perceptually nearer to black
            # than to the available green filament. Keep genuinely neutral hair,
            # base and watch pixels black, but preserve a chromatic sample whose
            # hue and channel dominance clearly match the accent material.
            if (
                lab[0] >= 8.0
                and source_chroma >= 4.0
                and accent_chroma >= 8.0
                and hue_similarity >= 0.90
                and color[1] >= color[0] + 3
                and color[1] >= color[2] + 3
            ):
                return accent_index
    primary_index, skin_index = portrait_indices
    if nearest != skin_index:
        return nearest
    lab = _srgb_to_lab(color)
    primary_distance = math.sqrt(
        sum((lab[channel] - palette_lab[primary_index][channel]) ** 2 for channel in range(3))
    )
    skin_distance = math.sqrt(
        sum((lab[channel] - palette_lab[skin_index][channel]) ** 2 for channel in range(3))
    )
    # Warm-white cloth shadows sit very close to the neutral/skin Voronoi
    # boundary. Bias only those low-chroma ambiguous samples back to the garment;
    # clearly saturated skin remains mapped to skin.
    if (
        lab[0] >= 35.0
        and max(color) - min(color) <= 36
        and math.hypot(lab[1], lab[2]) <= 18.0
        and primary_distance <= max(1e-9, skin_distance) * 1.35
    ):
        return primary_index
    return nearest
