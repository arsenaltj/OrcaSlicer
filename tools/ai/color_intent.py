#!/usr/bin/env python3
"""Versioned color intent bound to an exact printable OBJ artifact."""

from __future__ import annotations

from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path
import re
from typing import Any, Iterable, Mapping

from PIL import Image, UnidentifiedImageError

from printable_palette import (
    MAX_PRINTABLE_COLORS,
    MIN_PRINTABLE_COLORS,
    PALETTE_ROLES,
    active_palette_roles,
)


SCHEMA_ID = "orcaslicer.color-intent.v1"
COLOR_INTENT_FILENAME = "color-intent.v1.json"
MAX_MANIFEST_BYTES = 64 * 1024
MAX_REFERENCE_PIXELS = 64 * 1024 * 1024
_HEX_COLOR = re.compile(r"^#[0-9A-F]{6}$")
_SHA256 = re.compile(r"^[0-9a-f]{64}$")


class ColorIntentError(ValueError):
    pass


@dataclass(frozen=True)
class ColorIntentManifestFile:
    path: Path
    schema: str
    sha256: str


def sha256_file(path: str | os.PathLike[str]) -> str:
    digest = hashlib.sha256()
    try:
        with Path(path).open("rb") as stream:
            for block in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(block)
    except OSError as exc:
        raise ColorIntentError("A referenced color-intent file is unavailable.") from exc
    return digest.hexdigest()


def _safe_file_reference(path: Path) -> dict[str, str]:
    try:
        if not path.is_file() or path.stat().st_size <= 0:
            raise OSError
    except OSError as exc:
        raise ColorIntentError("A referenced color-intent file is unavailable.") from exc
    return {"filename": path.name, "sha256": sha256_file(path)}


def _normalize_palette_roles(
    palette: Iterable[str], palette_roles: Mapping[str, str]
) -> tuple[tuple[str, ...], dict[str, str]]:
    colors = tuple(str(color).strip().upper() for color in palette)
    if not MIN_PRINTABLE_COLORS <= len(colors) <= MAX_PRINTABLE_COLORS:
        raise ColorIntentError("Color intent requires between one and six fallback colors.")
    if any(not _HEX_COLOR.fullmatch(color) for color in colors):
        raise ColorIntentError("Color-intent colors must use uppercase #RRGGBB format.")
    if len(set(colors)) != len(colors):
        raise ColorIntentError("Color-intent fallback colors must be unique.")
    roles = active_palette_roles(len(colors))
    if set(palette_roles) != set(roles):
        raise ColorIntentError("Color intent must define each active semantic role exactly once.")
    normalized = {role: str(palette_roles[role]).strip().upper() for role in roles}
    if set(normalized.values()) != set(colors) or len(set(normalized.values())) != len(colors):
        raise ColorIntentError("Each color-intent role must reference one unique fallback color.")
    return colors, normalized


def _load_reference(path: Path) -> Image.Image:
    try:
        with Image.open(path) as source:
            width, height = source.size
            if width <= 0 or height <= 0 or width * height > MAX_REFERENCE_PIXELS:
                raise ColorIntentError("A color-intent reference image has invalid dimensions.")
            source.load()
            return source.convert("RGBA")
    except ColorIntentError:
        raise
    except (OSError, UnidentifiedImageError, Image.DecompressionBombError) as exc:
        raise ColorIntentError("A color-intent reference image is unavailable or invalid.") from exc


def _weighted_median(histogram: list[int], sample_count: int) -> int:
    midpoint = (sample_count - 1) // 2
    cumulative = 0
    for value, count in enumerate(histogram):
        cumulative += count
        if cumulative > midpoint:
            return value
    return 0


def _desired_colors_by_fallback(
    appearance_source_path: Path,
    material_preview_path: Path,
    fallback_colors: Iterable[str],
) -> dict[str, tuple[str, int]]:
    appearance = _load_reference(appearance_source_path)
    material = _load_reference(material_preview_path)
    if appearance.size != material.size:
        appearance = appearance.resize(material.size, Image.Resampling.BILINEAR)
    fallback_by_rgb = {
        bytes(int(color[index:index + 2], 16) for index in (1, 3, 5)): color
        for color in fallback_colors
    }
    histograms = {color: [[0] * 256 for _ in range(3)] for color in fallback_by_rgb.values()}
    sample_counts = {color: 0 for color in fallback_by_rgb.values()}
    appearance_bytes = appearance.tobytes()
    material_bytes = material.tobytes()
    for offset in range(0, len(material_bytes), 4):
        if material_bytes[offset + 3] == 0 or appearance_bytes[offset + 3] == 0:
            continue
        fallback = fallback_by_rgb.get(material_bytes[offset:offset + 3])
        if fallback is None:
            continue
        sample_counts[fallback] += 1
        for channel in range(3):
            histograms[fallback][channel][appearance_bytes[offset + channel]] += 1
    result: dict[str, tuple[str, int]] = {}
    for fallback in fallback_by_rgb.values():
        count = sample_counts[fallback]
        desired = fallback if count == 0 else "#" + "".join(
            f"{_weighted_median(histograms[fallback][channel], count):02X}" for channel in range(3)
        )
        result[fallback] = desired, count
    return result


def build_color_intent_manifest(
    artifact_path: str | os.PathLike[str],
    appearance_source_path: str | os.PathLike[str],
    material_preview_path: str | os.PathLike[str],
    palette: Iterable[str],
    palette_roles: Mapping[str, str],
    *,
    geometry_reference_path: str | os.PathLike[str] | None = None,
) -> dict[str, Any]:
    artifact = Path(artifact_path)
    appearance = Path(appearance_source_path)
    material = Path(material_preview_path)
    colors, roles = _normalize_palette_roles(palette, palette_roles)
    desired = _desired_colors_by_fallback(appearance, material, colors)
    references = {
        "appearance_source": _safe_file_reference(appearance),
        "material_preview": _safe_file_reference(material),
    }
    if geometry_reference_path is not None:
        references["geometry"] = _safe_file_reference(Path(geometry_reference_path))
    manifest = {
        "schema": SCHEMA_ID,
        "mode": "discrete_filament",
        "artifact": {
            **_safe_file_reference(artifact),
            "color_encoding": "vertex_colors",
        },
        "references": references,
        "targets": [
            {
                "role": role,
                "fallback_color": roles[role],
                "desired_color": desired[roles[role]][0],
                "sample_count": desired[roles[role]][1],
            }
            for role in active_palette_roles(len(colors))
        ],
    }
    validate_color_intent_manifest(manifest, artifact_path=artifact)
    return manifest


def _validate_file_reference(value: Any, label: str) -> None:
    if not isinstance(value, dict) or set(value) != {"filename", "sha256"}:
        raise ColorIntentError(f"The {label} reference is invalid.")
    filename = value.get("filename")
    if not isinstance(filename, str) or not filename or Path(filename).name != filename:
        raise ColorIntentError(f"The {label} filename is invalid.")
    if not isinstance(value.get("sha256"), str) or not _SHA256.fullmatch(value["sha256"]):
        raise ColorIntentError(f"The {label} SHA-256 is invalid.")


def validate_color_intent_manifest(
    payload: Any, *, artifact_path: str | os.PathLike[str] | None = None
) -> None:
    if not isinstance(payload, dict) or set(payload) != {"schema", "mode", "artifact", "references", "targets"}:
        raise ColorIntentError("The color-intent manifest shape is invalid.")
    if payload.get("schema") != SCHEMA_ID or payload.get("mode") != "discrete_filament":
        raise ColorIntentError("The color-intent schema or mode is unsupported.")
    artifact = payload.get("artifact")
    if not isinstance(artifact, dict) or set(artifact) != {"filename", "sha256", "color_encoding"}:
        raise ColorIntentError("The color-intent artifact reference is invalid.")
    _validate_file_reference({key: artifact[key] for key in ("filename", "sha256")}, "artifact")
    if artifact.get("color_encoding") != "vertex_colors":
        raise ColorIntentError("The color-intent artifact encoding is unsupported.")
    references = payload.get("references")
    if not isinstance(references, dict) or not {"appearance_source", "material_preview"}.issubset(references):
        raise ColorIntentError("The color-intent image references are incomplete.")
    if not set(references).issubset({"appearance_source", "material_preview", "geometry"}):
        raise ColorIntentError("The color-intent image references contain an unknown entry.")
    for name, reference in references.items():
        _validate_file_reference(reference, name)
    targets = payload.get("targets")
    if not isinstance(targets, list) or not MIN_PRINTABLE_COLORS <= len(targets) <= MAX_PRINTABLE_COLORS:
        raise ColorIntentError("Color intent must contain between one and six targets.")
    expected_roles = set(active_palette_roles(len(targets)))
    roles: set[str] = set()
    fallback_colors: set[str] = set()
    for target in targets:
        if not isinstance(target, dict) or set(target) != {"role", "fallback_color", "desired_color", "sample_count"}:
            raise ColorIntentError("A color-intent target is invalid.")
        role = target.get("role")
        fallback = target.get("fallback_color")
        desired = target.get("desired_color")
        samples = target.get("sample_count")
        if role not in PALETTE_ROLES or role in roles:
            raise ColorIntentError("Color-intent roles must be active and unique.")
        if not isinstance(fallback, str) or not _HEX_COLOR.fullmatch(fallback) or fallback in fallback_colors:
            raise ColorIntentError("Color-intent fallback colors must be uppercase and unique.")
        if not isinstance(desired, str) or not _HEX_COLOR.fullmatch(desired):
            raise ColorIntentError("A desired color is invalid.")
        if isinstance(samples, bool) or not isinstance(samples, int) or samples < 0:
            raise ColorIntentError("A color-intent sample count is invalid.")
        roles.add(role)
        fallback_colors.add(fallback)
    if roles != expected_roles:
        raise ColorIntentError("Color-intent targets do not match the active semantic roles.")
    if artifact_path is not None:
        path = Path(artifact_path)
        if path.name != artifact["filename"] or sha256_file(path) != artifact["sha256"]:
            raise ColorIntentError("The color-intent manifest does not match its OBJ artifact.")


def write_color_intent_manifest(
    destination: str | os.PathLike[str],
    artifact_path: str | os.PathLike[str],
    appearance_source_path: str | os.PathLike[str],
    material_preview_path: str | os.PathLike[str],
    palette: Iterable[str],
    palette_roles: Mapping[str, str],
    *,
    geometry_reference_path: str | os.PathLike[str] | None = None,
) -> ColorIntentManifestFile:
    path = Path(destination)
    manifest = build_color_intent_manifest(
        artifact_path,
        appearance_source_path,
        material_preview_path,
        palette,
        palette_roles,
        geometry_reference_path=geometry_reference_path,
    )
    encoded = (json.dumps(manifest, ensure_ascii=False, indent=2, sort_keys=True) + "\n").encode("utf-8")
    if len(encoded) > MAX_MANIFEST_BYTES:
        raise ColorIntentError("The color-intent manifest exceeds its size limit.")
    temporary = path.with_name(path.name + ".part")
    try:
        temporary.write_bytes(encoded)
        os.replace(temporary, path)
    except OSError as exc:
        try:
            temporary.unlink(missing_ok=True)
        except OSError:
            pass
        raise ColorIntentError("The color-intent manifest could not be written.") from exc
    return ColorIntentManifestFile(path=path, schema=SCHEMA_ID, sha256=sha256_file(path))


def verify_color_intent_manifest_file(
    path: str | os.PathLike[str],
    artifact_path: str | os.PathLike[str] | None,
    *,
    expected_schema: str = SCHEMA_ID,
    expected_sha256: str = "",
) -> ColorIntentManifestFile:
    if artifact_path is None:
        raise ColorIntentError("The color-intent OBJ artifact is unavailable.")
    manifest_path = Path(path)
    try:
        if not manifest_path.is_file() or not 0 < manifest_path.stat().st_size <= MAX_MANIFEST_BYTES:
            raise OSError
        payload = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise ColorIntentError("The color-intent manifest is unavailable or invalid.") from exc
    validate_color_intent_manifest(payload, artifact_path=artifact_path)
    digest = sha256_file(manifest_path)
    if expected_schema != SCHEMA_ID or (expected_sha256 and expected_sha256 != digest):
        raise ColorIntentError("The persisted color-intent identity does not match the manifest.")
    return ColorIntentManifestFile(path=manifest_path, schema=SCHEMA_ID, sha256=digest)
