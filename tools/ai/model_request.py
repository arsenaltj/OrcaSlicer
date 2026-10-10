"""Validate model requests without HTTP, job execution or provider calls."""
from __future__ import annotations
import json
import re
from dataclasses import asdict, dataclass
from hunyuan_provider_gateway import validate_options as validate_hunyuan_options
from io import BytesIO
from model_contracts import (
    DEFAULT_IMAGE_INSTRUCTION,
    DEFAULT_PALETTE_COLORS,
    GENERATION_PROFILES,
    LEGACY_STYLE_ALIASES,
    MAX_CUSTOM_STYLE_BYTES,
    MAX_IMAGE_BYTES,
    MAX_PALETTE_COLORS,
    MAX_PROMPT_BYTES,
    MAX_TEXTURE_PIXELS,
    MODEL_FACE_LIMITS,
    RequestError,
    STYLE_IDS,
)
from model_job_support import image_type as _image_type
from model_provider_gateway import ProviderGatewayError
from pathlib import Path
from printable_image_pipeline import PrintSettings, PrintableImageError
from printable_palette import (
    PrintablePaletteError,
    active_palette_roles,
    assign_palette_roles,
    normalize_palette_color_count,
)
from tripo_client import (
    TripoError,
    validate_generation_option_values,
    validate_generation_options,
)
from typing import Any


def _text_field(value: Any, name: str, *, allow_empty: bool = False) -> str:
    if not isinstance(value, str):
        raise RequestError("invalid_request", f"{name} must be a string.", 400)
    value = value.strip()
    if not allow_empty and not value:
        raise RequestError("invalid_request", f"{name} is required.", 400)
    if len(value.encode("utf-8")) > MAX_PROMPT_BYTES:
        raise RequestError("invalid_request", f"{name} exceeds the 2000-byte limit.", 400)
    return value

def _boolean_field(value: Any, name: str, *, default: bool = False) -> bool:
    if value is None:
        return default
    if isinstance(value, bool):
        return value
    if isinstance(value, str) and value.strip().lower() in {"true", "false"}:
        return value.strip().lower() == "true"
    raise RequestError("invalid_request", f"{name} must be a boolean.", 400)

def _normalize_palette(value: Any) -> tuple[str, ...]:
    if not isinstance(value, list) or len(value) > MAX_PALETTE_COLORS:
        raise RequestError(
            "invalid_palette",
            f"palette must contain between 0 and {MAX_PALETTE_COLORS} colors.",
            400,
        )
    normalized: list[str] = []
    seen: set[str] = set()
    for color in value:
        if not isinstance(color, str) or re.fullmatch(r"#[0-9A-Fa-f]{6}", color) is None:
            raise RequestError("invalid_palette", "palette colors must use #RRGGBB format.", 400)
        canonical = color.upper()
        if canonical not in seen:
            seen.add(canonical)
            normalized.append(canonical)
    return tuple(normalized)

def _normalize_palette_color_count(value: Any) -> int:
    try:
        return normalize_palette_color_count(value)
    except PrintablePaletteError as exc:
        raise RequestError("invalid_palette_color_count", str(exc), 400) from None

def _multipart_palette(value: Any) -> tuple[str, ...]:
    if not isinstance(value, str):
        raise RequestError("invalid_palette", "palette is required.", 400)
    try:
        parsed = json.loads(value)
    except json.JSONDecodeError:
        raise RequestError("invalid_palette", "palette must be a JSON color array.", 400) from None
    return _normalize_palette(parsed)

def _normalize_palette_roles(value: Any, palette: tuple[str, ...]) -> dict[str, str]:
    if not palette:
        if value in (None, {}):
            return {}
        raise RequestError("invalid_palette_roles", "palette roles require printable colors.", 400)
    if value is None:
        value = {}
    if not isinstance(value, dict) or any(not isinstance(key, str) or not isinstance(color, str) for key, color in value.items()):
        raise RequestError("invalid_palette_roles", "palette_roles must be a color-role object.", 400)
    try:
        return assign_palette_roles(palette, value).color_by_role
    except PrintablePaletteError as exc:
        raise RequestError("invalid_palette_roles", str(exc), 400) from None

def _normalize_palette_recommendation(value: Any, expected_color_count: int = DEFAULT_PALETTE_COLORS) -> dict[str, Any]:
    if value in (None, {}):
        return {}
    expected_color_count = _normalize_palette_color_count(expected_color_count)
    if not isinstance(value, dict):
        raise RequestError("invalid_palette_recommendation", "palette recommendation must be an object.", 400)
    summary = value.get("summary")
    colors = value.get("colors")
    if not isinstance(summary, str) or not summary.strip() or len(summary.strip().encode("utf-8")) > 400:
        raise RequestError("invalid_palette_recommendation", "palette recommendation summary is invalid.", 400)
    if not isinstance(colors, list) or len(colors) != expected_color_count:
        raise RequestError(
            "invalid_palette_recommendation",
            f"palette recommendation must contain {expected_color_count} colors.",
            400,
        )
    roles = active_palette_roles(expected_color_count)
    by_role: dict[str, dict[str, str]] = {}
    palette_values: list[str] = []
    limits = {"name": 80, "usage": 160, "reason": 400}
    for item in colors:
        if not isinstance(item, dict):
            raise RequestError("invalid_palette_recommendation", "palette recommendation color is invalid.", 400)
        role = item.get("role")
        if role not in roles or role in by_role:
            raise RequestError("invalid_palette_recommendation", "palette recommendation roles are invalid.", 400)
        color = item.get("hex")
        palette = _normalize_palette([color])
        fields: dict[str, str] = {"hex": palette[0], "role": role}
        for name, maximum in limits.items():
            text = item.get(name)
            if not isinstance(text, str) or not text.strip() or len(text.strip().encode("utf-8")) > maximum:
                raise RequestError("invalid_palette_recommendation", f"palette recommendation {name} is invalid.", 400)
            fields[name] = text.strip()
        by_role[role] = fields
        palette_values.append(palette[0])
    palette = _normalize_palette(palette_values)
    if len(palette) != expected_color_count or set(by_role) != set(roles):
        raise RequestError("invalid_palette_recommendation", "palette recommendation colors and roles must be unique.", 400)
    try:
        assignment = assign_palette_roles(palette, {role: by_role[role]["hex"] for role in roles})
    except PrintablePaletteError as exc:
        raise RequestError("invalid_palette_recommendation", str(exc), 400) from None
    if assignment.low_contrast:
        raise RequestError("invalid_palette_recommendation", "palette recommendation colors have insufficient contrast.", 400)
    return {"summary": summary.strip(), "colors": [by_role[role] for role in roles]}

def _multipart_palette_roles(value: Any, palette: tuple[str, ...]) -> dict[str, str]:
    if value in (None, ""):
        return _normalize_palette_roles(None, palette)
    if not isinstance(value, str):
        raise RequestError("invalid_palette_roles", "palette_roles must be valid JSON.", 400)
    try:
        parsed = json.loads(value)
    except json.JSONDecodeError:
        raise RequestError("invalid_palette_roles", "palette_roles must be valid JSON.", 400) from None
    return _normalize_palette_roles(parsed, palette)

@dataclass(frozen=True)
class ValidatedImage:
    content_type: str
    width: int
    height: int

def _validate_image_data(
    data: bytes,
    *,
    minimum_edge: int,
    require_visual_detail: bool = False,
) -> ValidatedImage:
    """Check transport integrity; image quality belongs to advisory reports.

    Keep legacy quality arguments compatible with existing callers, but small,
    blank and transparent decodable images are allowed to continue.
    """
    if not data or len(data) > MAX_IMAGE_BYTES:
        raise ValueError("The image is empty or exceeds the 20 MB limit.")
    content_type = _image_type(data[:16])
    if content_type is None:
        raise ValueError("The image must be PNG or JPEG.")
    try:
        from PIL import Image, UnidentifiedImageError
    except ImportError:
        raise ValueError("Pillow is required to validate the image.") from None
    try:
        with Image.open(BytesIO(data)) as opened:
            expected_format = "PNG" if content_type == "image/png" else "JPEG"
            if opened.format != expected_format:
                raise ValueError("The image format does not match its file signature.")
            width, height = opened.size
            if width * height > MAX_TEXTURE_PIXELS:
                raise ValueError("The image must contain no more than 64 megapixels.")
            opened.load()
    except ValueError:
        raise
    except (OSError, UnidentifiedImageError, Image.DecompressionBombError):
        raise ValueError("The image is damaged or could not be decoded completely.") from None
    return ValidatedImage(content_type, width, height)

def _validate_image_file(
    path: Path | None,
    *,
    minimum_edge: int,
    require_visual_detail: bool = False,
) -> ValidatedImage:
    if path is None:
        raise ValueError("The image file is unavailable.")
    try:
        size = path.stat().st_size
        if size <= 0 or size > MAX_IMAGE_BYTES:
            raise ValueError("The image is empty or exceeds the 20 MB limit.")
        data = path.read_bytes()
    except ValueError:
        raise
    except OSError:
        raise ValueError("The image file could not be read.") from None
    return _validate_image_data(
        data,
        minimum_edge=minimum_edge,
        require_visual_detail=require_visual_detail,
    )

def _normalize_style(value: Any) -> str:
    if value is None or value == "":
        return "sculpture"
    if isinstance(value, str):
        value = LEGACY_STYLE_ALIASES.get(value, value)
    if not isinstance(value, str) or value not in STYLE_IDS:
        raise RequestError(
            "invalid_style",
            "style must be sculpture, realistic, portrait_sketch, cartoon, low_poly, relief, ink_relief, diorama, or custom.",
            400,
        )
    return value

def _normalize_custom_style(value: Any, style: str) -> str:
    if value is None:
        value = ""
    if not isinstance(value, str):
        raise RequestError("invalid_custom_style", "custom_style must be a string.", 400)
    description = value.strip()
    if style == "custom":
        if not description:
            raise RequestError("invalid_custom_style", "custom_style is required when style is custom.", 400)
        if len(description.encode("utf-8")) > MAX_CUSTOM_STYLE_BYTES:
            raise RequestError("invalid_custom_style", "custom_style exceeds the 1000-byte limit.", 400)
        return description
    if description:
        raise RequestError("invalid_custom_style", "custom_style is only allowed when style is custom.", 400)
    return ""

def _normalize_face_limit(value: Any) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value not in MODEL_FACE_LIMITS:
        raise RequestError(
            "invalid_face_limit",
            "face_limit must be 100000, 300000, 500000, 1000000, or 2000000 triangles.",
            400,
        )
    return value

def _normalize_generation_profile(value: Any) -> str:
    if not isinstance(value, str) or value not in GENERATION_PROFILES:
        raise RequestError(
            "invalid_generation_profile",
            "generation_profile must be quality or performance.",
            400,
        )
    return value

def _generation_options(payload: dict[str, Any], face_limit: int, *,
                        validate_provider_constraints: bool = True) -> tuple[str | None, str, str]:
    provider = _generation_provider(payload)
    geometry = payload.get("geometry_quality")
    texture = payload.get("texture_quality", "standard")
    output = payload.get("output_format", "glb")
    try:
        validate_generation_option_values(geometry, texture)
        if validate_provider_constraints:
            if provider == "hunyuan":
                validate_hunyuan_options(face_limit, geometry, texture)
            else:
                validate_generation_options(face_limit, geometry, texture)
    except (TripoError, ProviderGatewayError) as exc:
        raise RequestError("invalid_generation_options", str(exc), 400) from None
    if output not in ("glb", "obj"):
        raise RequestError("invalid_generation_options", "Output format must be glb or obj.", 400)
    return geometry, texture, output

def _generation_provider(payload: dict[str, Any]) -> str:
    provider = payload.get("provider", "tripo")
    if not isinstance(provider, str) or provider not in ("tripo", "hunyuan"):
        raise RequestError("invalid_provider", "Provider must be tripo or hunyuan.", 400)
    return provider

def _normalize_image_instruction(value: Any) -> str:
    if value is None:
        return DEFAULT_IMAGE_INSTRUCTION
    if not isinstance(value, str):
        raise RequestError("invalid_request", "instruction must be UTF-8 text.", 400)
    return value.strip() or DEFAULT_IMAGE_INSTRUCTION

def _user_image_instruction(value: Any) -> str:
    """Return only text the user actually entered; keep internal defaults private."""
    if value is None:
        return ""
    if not isinstance(value, str):
        raise RequestError("invalid_request", "instruction must be UTF-8 text.", 400)
    return value.strip()

def _normalize_print_settings(value: Any) -> dict[str, Any]:
    try:
        return asdict(PrintSettings.from_mapping(value))
    except PrintableImageError as exc:
        raise RequestError("invalid_print_settings", str(exc), 400) from None
