#!/usr/bin/env python3
"""Provider-agnostic printable palette roles for four-filament workflows."""

from __future__ import annotations

from dataclasses import dataclass
import math
import re
from typing import Iterable, Mapping


MAX_PRINTABLE_COLORS = 4
LEGACY_MAX_PRINTABLE_COLORS = 16
PALETTE_ROLES = ("primary", "structure", "light", "accent")
_HEX_COLOR = re.compile(r"^#[0-9A-F]{6}$")


class PrintablePaletteError(ValueError):
    pass


@dataclass(frozen=True)
class PaletteRoleAssignment:
    palette: tuple[str, ...]
    role_by_color: dict[str, str]
    color_by_role: dict[str, str]
    minimum_distance: float
    low_contrast: bool

    def ordered_colors(self) -> tuple[str, ...]:
        return tuple(self.color_by_role[role] for role in PALETTE_ROLES if role in self.color_by_role)

    def as_metadata(self) -> dict[str, object]:
        return {
            "role_by_color": dict(self.role_by_color),
            "color_by_role": dict(self.color_by_role),
            "minimum_distance": round(self.minimum_distance, 4),
            "low_contrast": self.low_contrast,
        }


def normalize_palette(colors: Iterable[str], *, max_colors: int = MAX_PRINTABLE_COLORS) -> tuple[str, ...]:
    normalized: list[str] = []
    for value in colors:
        color = str(value).strip().upper()
        if not _HEX_COLOR.fullmatch(color):
            raise PrintablePaletteError("palette colors must use #RRGGBB format")
        if color not in normalized:
            normalized.append(color)
    if not 1 <= len(normalized) <= max_colors:
        raise PrintablePaletteError(f"palette must contain between 1 and {max_colors} unique colors")
    return tuple(normalized)


def _rgb(color: str) -> tuple[int, int, int]:
    return tuple(int(color[index:index + 2], 16) for index in (1, 3, 5))  # type: ignore[return-value]


def _lab(color: str) -> tuple[float, float, float]:
    linear: list[float] = []
    for channel in _rgb(color):
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


def _distance(left: tuple[float, float, float], right: tuple[float, float, float]) -> float:
    return math.sqrt(sum((left[index] - right[index]) ** 2 for index in range(3)))


def _active_roles(color_count: int) -> tuple[str, ...]:
    if color_count == 1:
        return ("primary",)
    if color_count == 2:
        return ("primary", "structure")
    if color_count == 3:
        return ("primary", "structure", "light")
    return PALETTE_ROLES


def assign_palette_roles(
    colors: Iterable[str], overrides: Mapping[str, str] | None = None
) -> PaletteRoleAssignment:
    palette = normalize_palette(colors)
    labs = {color: _lab(color) for color in palette}
    roles = _active_roles(len(palette))
    assigned: dict[str, str] = {}
    used: set[str] = set()

    for role, raw_color in (overrides or {}).items():
        if role not in roles:
            raise PrintablePaletteError(f"palette role {role!r} is not active")
        color = str(raw_color).strip().upper()
        if color not in palette:
            raise PrintablePaletteError("palette role colors must belong to the printable palette")
        if color in used:
            raise PrintablePaletteError("each active palette role must use a different color")
        assigned[role] = color
        used.add(color)

    def choose(role: str, key) -> None:
        if role in roles and role not in assigned:
            candidates = [color for color in palette if color not in used]
            chosen = max(candidates, key=key)
            assigned[role] = chosen
            used.add(chosen)

    # Structural regions need the darkest available material; highlights need the lightest.
    choose("structure", lambda color: (-labs[color][0], -palette.index(color)))
    choose("light", lambda color: (labs[color][0], -palette.index(color)))
    # The most chromatic remaining color becomes the main material. The final color is the accent.
    choose(
        "primary",
        lambda color: (math.hypot(labs[color][1], labs[color][2]), -abs(labs[color][0] - 58.0), -palette.index(color)),
    )
    choose("accent", lambda color: (math.hypot(labs[color][1], labs[color][2]), -palette.index(color)))

    role_by_color = {color: role for role, color in assigned.items()}
    distances = [
        _distance(labs[palette[left]], labs[palette[right]])
        for left in range(len(palette))
        for right in range(left + 1, len(palette))
    ]
    minimum_distance = min(distances) if distances else math.inf
    return PaletteRoleAssignment(
        palette=palette,
        role_by_color=role_by_color,
        color_by_role=assigned,
        minimum_distance=minimum_distance,
        low_contrast=minimum_distance < 12.0,
    )
