from __future__ import annotations

import base64
import binascii
import ipaddress
import json
import os
import socket
import ssl
import urllib.error
import urllib.parse
import urllib.request
import uuid
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Mapping

try:
    from .printable_palette import PALETTE_ROLES, PrintablePaletteError, assign_palette_roles, normalize_palette
except ImportError:
    from printable_palette import PALETTE_ROLES, PrintablePaletteError, assign_palette_roles, normalize_palette

_DEFAULT_BASE_URL = "https://api.openai.com/v1"
_MAX_JSON_BYTES = 32 * 1024 * 1024
_MAX_IMAGE_BYTES = 20 * 1024 * 1024
_TIMEOUT_SECONDS = 120

STYLE_PROFILES = {
    "q_cartoon": (
        "Restyle the selected primary subject as a premium designer-toy collectible. Preserve recognizable identity, facial "
        "relationships, hairstyle, signature clothing silhouette, pose, and cultural attributes before applying moderate chibi "
        "exaggeration. Use a roughly 92-percent identity-preserving and 8-percent playful treatment for a real person: keep their "
        "adult age, craniofacial proportions, natural eye size, eyelid shape, nose, mouth, smile, jaw and face width. Do not make "
        "an adult childlike, baby-faced, anime-like, or into a generic big-eyed doll. Keep any head enlargement subtle. Apply the "
        "stronger toy simplification to the body, hair masses, clothing folds and materials instead of replacing the face. Use "
        "rounded toy-like forms, grouped solid hair masses, and smooth matte vinyl surfaces."
    ),
    "low_poly": (
        "Restyle only already-visible surfaces and silhouettes as clean low-poly forms with large intentional polygon facets, "
        "broad readable planes, restrained geometric detail, and large contiguous material regions. Preserve the visible facial, "
        "clothing, and object structure. Do not invent missing anatomy or unrelated geometry."
    ),
    "cel_shaded": (
        "Restyle the selected subject as a printable cel-shaded collectible with smooth sturdy geometry and two or three broad "
        "tone bands per semantic part. Use solid material blocks instead of lighting gradients. Do not add black ink outlines, "
        "rim-light strokes, tiny highlights, or painted line art; silhouettes and facial features must be carried by geometry and "
        "large enclosed color regions."
    ),
    "enamel_inlay": (
        "Restyle the selected subject as a premium enamel-inlay display piece with smooth simplified geometry, a few large enclosed "
        "color fields, and shallow raised separators or structural grooves wherever two material colors meet. Keep separators broad "
        "and printable; avoid filigree, mosaic fragments, thin metallic wires, glossy reflections, or texture-only borders."
    ),
    "sculpture": (
        "Restyle only already-visible subject surfaces as a museum-quality marble or plaster sculpture with smooth carved facial "
        "planes, simplified solid hair, broad carved clothing folds, and a restrained matte stone or plaster finish. Preserve each "
        "subject's recognizable identity and visible cultural context. Do not reconstruct missing anatomy or unrelated subject "
        "regions."
    ),
}

CUSTOM_STYLE_ID = "custom"
MAX_CUSTOM_STYLE_BYTES = 1000
MAX_PALETTE_RECOMMENDATION_SUMMARY_BYTES = 400
MAX_PALETTE_RECOMMENDATION_NAME_BYTES = 80
MAX_PALETTE_RECOMMENDATION_USAGE_BYTES = 160
MAX_PALETTE_RECOMMENDATION_REASON_BYTES = 400


class OpenAIPreprocessorError(RuntimeError):
    """An OpenAI-compatible preprocessing request failed safely."""


@dataclass(frozen=True)
class PrintablePaletteRecommendationColor:
    hex: str
    name: str
    role: str
    usage: str
    reason: str

    def as_dict(self) -> dict[str, str]:
        return {
            "hex": self.hex,
            "name": self.name,
            "role": self.role,
            "usage": self.usage,
            "reason": self.reason,
        }


@dataclass(frozen=True)
class PrintablePaletteRecommendation:
    summary: str
    colors: tuple[PrintablePaletteRecommendationColor, ...]

    def as_dict(self) -> dict[str, Any]:
        return {"summary": self.summary, "colors": [color.as_dict() for color in self.colors]}


def _config() -> tuple[str, str, str, str]:
    key = os.environ.get("OPENAI_API_KEY", "")
    raw_base = os.environ.get("OPENAI_BASE_URL", _DEFAULT_BASE_URL).strip()
    if not key:
        raise OpenAIPreprocessorError("OPENAI_API_KEY is not configured.")
    parsed = urllib.parse.urlsplit(raw_base)
    if (
        parsed.scheme.lower() != "https"
        or not parsed.hostname
        or parsed.username
        or parsed.password
        or parsed.query
        or parsed.fragment
    ):
        raise OpenAIPreprocessorError("OPENAI_BASE_URL must be a credential-free HTTPS URL.")
    path = parsed.path.rstrip("/") or "/v1"
    base = urllib.parse.urlunsplit((parsed.scheme, parsed.netloc, path, "", ""))
    return (
        base,
        key,
        os.environ.get("OPENAI_TEXT_MODEL", "gpt-5.4"),
        os.environ.get("OPENAI_IMAGE_MODEL", "gpt-image-2"),
    )


def _read_json_response(response: Any) -> dict[str, Any]:
    content_length = response.headers.get("Content-Length")
    if content_length:
        try:
            if int(content_length) > _MAX_JSON_BYTES:
                raise OpenAIPreprocessorError("The preprocessing service returned an oversized response.")
        except ValueError:
            pass
    raw = response.read(_MAX_JSON_BYTES + 1)
    if len(raw) > _MAX_JSON_BYTES:
        raise OpenAIPreprocessorError("The preprocessing service returned an oversized response.")
    try:
        value = json.loads(raw.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError):
        raise OpenAIPreprocessorError("The preprocessing service returned an invalid response.") from None
    if not isinstance(value, dict):
        raise OpenAIPreprocessorError("The preprocessing service returned an invalid response.")
    return value


def _provider_request(path: str, body: bytes, content_type: str) -> dict[str, Any]:
    base, key, _, _ = _config()
    request = urllib.request.Request(base + path, data=body, method="POST")
    request.add_header("Authorization", "Bearer " + key)
    request.add_header("Content-Type", content_type)
    request.add_header("Accept", "application/json")
    try:
        with urllib.request.build_opener(_RejectRedirects()).open(request, timeout=_TIMEOUT_SECONDS) as response:
            return _read_json_response(response)
    except OpenAIPreprocessorError:
        raise
    except urllib.error.HTTPError as exc:
        if exc.code == 429:
            message = "The preprocessing service is rate limiting requests; try again later."
        elif 400 <= exc.code < 500:
            message = "The preprocessing service rejected the request."
        else:
            message = "The preprocessing service is temporarily unavailable."
        raise OpenAIPreprocessorError(message) from None
    except (urllib.error.URLError, TimeoutError, OSError):
        raise OpenAIPreprocessorError("Could not connect to the preprocessing service.") from None


class _RejectRedirects(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req: Any, fp: Any, code: int, msg: str, headers: Any, newurl: str) -> None:
        return None


def _content_text(value: Any) -> str:
    if isinstance(value, str):
        return value
    if isinstance(value, list):
        return "".join(_content_text(part) for part in value)
    if isinstance(value, dict):
        for key in ("text", "output_text", "content"):
            if key in value:
                return _content_text(value[key])
    return ""


def complete_text(system_prompt: str, user_content: str) -> str:
    if not isinstance(system_prompt, str) or not system_prompt.strip():
        raise OpenAIPreprocessorError("A system prompt is required.")
    if not isinstance(user_content, str) or not user_content.strip():
        raise OpenAIPreprocessorError("User content is required.")
    _, _, model, _ = _config()
    payload = {
        "model": model,
        "messages": [
            {"role": "system", "content": system_prompt},
            {"role": "user", "content": user_content},
        ],
    }
    result = _provider_request(
        "/chat/completions",
        json.dumps(payload, ensure_ascii=False, separators=(",", ":")).encode("utf-8"),
        "application/json",
    )
    content = _completion_content(result)
    if not content.strip():
        raise OpenAIPreprocessorError("The preprocessing service returned an empty response.")
    return content.strip()


def _completion_content(result: dict[str, Any]) -> str:
    choices = result.get("choices")
    content = ""
    if isinstance(choices, list) and choices and isinstance(choices[0], dict):
        message = choices[0].get("message")
        content = _content_text(message.get("content") if isinstance(message, dict) else message)
    if not content:
        content = _content_text(result.get("output_text")) or _content_text(result.get("output"))
    return content


def _style_profile(style: str, custom_style: str = "") -> str:
    if style == CUSTOM_STYLE_ID:
        if not isinstance(custom_style, str) or not custom_style.strip():
            raise OpenAIPreprocessorError("A custom style description is required.")
        description = custom_style.strip()
        if len(description.encode("utf-8")) > MAX_CUSTOM_STYLE_BYTES:
            raise OpenAIPreprocessorError("The custom style description exceeds the 1000-byte limit.")
        return (
            "Apply this user-defined visual style to the existing subject: "
            + description
            + ". Treat it only as appearance and shape-language direction. The subject identity, image-to-3D composition, "
            "printable palette, structural connections, stable base, and other hard constraints in this request take priority."
        )
    profile = STYLE_PROFILES.get(style)
    if profile is None:
        raise OpenAIPreprocessorError("The selected style is not supported.")
    return profile


def _designer_toy_profile(style: str, custom_style: str = "") -> str:
    return (
        _style_profile(style, custom_style)
        + " Render the existing subject as a premium full-color designer toy made from separate solid-color materials. "
        "Keep the selected profile's geometry, but replace monochrome stone, plaster, photographic, or natural-material "
        "treatment with intentional printable color blocking. The printable color treatment overrides any monochrome "
        "material wording in the base style profile."
    )


def _designer_toy_palette_direction(
    palette: tuple[str, ...],
    shadow_color: str = "",
    palette_roles: Mapping[str, str] | None = None,
) -> str:
    del shadow_color  # Accepted for backward-compatible callers; roles are now based on actual filament colors.
    try:
        assignment = assign_palette_roles(palette, palette_roles)
    except PrintablePaletteError as exc:
        raise OpenAIPreprocessorError(str(exc)) from None
    required = min(3, len(palette))
    role_text = ", ".join(f"{role}={color}" for role, color in assignment.color_by_role.items())
    return (
        "Treat these colors as the allowed printable palette: "
        + ", ".join(palette)
        + ". Every visible subject surface must use one of these material colors. Keep the outer silhouette, neck, limbs, "
        "load-bearing connections, base contact, and stacked architectural tiers visibly joined by opaque palette-colored geometry; "
        "never cut structural connections out as background or transparency. Cover at least 65 percent of the visible subject with "
        "the primary and structure materials. Assign the remaining colors to large semantic parts rather than lighting gradients. "
        "For a person or character, use the light material for the face only when structure-colored hair and a collar visibly "
        "enclose it. Never retain natural flesh tones. "
        "Do not introduce beige, gray, black, off-white, natural skin tones, or dark substitutes unless that exact shade is one of "
        "the listed printable colors; every listed color remains valid regardless of its common color name. Use at least "
        + str(required)
        + " listed colors in clearly visible, meaningful regions; do not hide required colors in speckles or tiny accents. "
        "Use broad contiguous regions with hard readable boundaries. A deterministic print-mapping step will convert the "
        "result to exact filament colors. Use these perceptual material roles derived from the actual loaded filaments: "
        + role_text
        + ". The primary color covers the largest subject material; the structure color covers hair, rear surfaces, seams, "
        "and the darkest load-bearing regions; the light color is reserved for enclosed highlights or a face panel; the accent "
        "color marks one secondary semantic part. Ignore any role that is absent from this smaller palette. "
        "Place the subject on a transparent background. If transparency is unavailable, use one uniform neutral studio background "
        "with no shadow; that background is not a model material and must not be used on the subject. "
    )


def _image_to_3d_composition_direction(transparent_background: bool = False) -> str:
    return (
        "Recompose the selected primary subject as a clean product-shot reference for image-to-3D rather than editing the "
        "photograph in place. Center one readable collectible on "
        + ("a transparent background" if transparent_background else "a plain bright background")
        + ", show a coherent complete silhouette, and use a front or gentle three-quarter view. Attach a person, animal, statue, "
        "character, or otherwise unstable prop to one simple integrated round or softly polygonal display base. If the selected "
        "subject is an inherently stable manufactured object such as a shoe, camera, cup, vehicle, or appliance, preserve its own "
        "flat contact geometry and do not add a pedestal unless the user explicitly requests one. A necessary base is the only new "
        "support element permitted. Remove scenery, floor shadows, text, logos, watermarks, camera UI, "
        "color cards, and unrelated people, plants, props, or landmarks. Do not combine separate scene elements into one object. "
        "Choose the subject named by the user when it is visible; otherwise choose the visually dominant foreground subject. "
        "Apply these source-dependent framing rules: if the complete person, animal, or object is visible, preserve the complete "
        "head-to-toe or whole-object form and its existing pose. If a person is cropped before the knees or only the upper body is "
        "visible, create a deliberately finished bust or half-body collectible: preserve only the visible head, torso, arms, and "
        "clothing, end the lower torso with a clean sculpted boundary on the base, and do not invent a pelvis, legs, or feet. If the "
        "source is a multi-subject scenic photograph, isolate exactly one requested or dominant subject and omit all secondary "
        "subjects and background scenery. Never duplicate a face, limb, tower, statue, accessory, or architectural element. "
    )


def preprocess_text(
    instruction: str,
    palette: tuple[str, ...] = (),
    style: str = "sculpture",
    custom_style: str = "",
) -> str:
    if not isinstance(instruction, str) or not instruction.strip():
        raise OpenAIPreprocessorError("A text instruction is required.")
    palette_instruction = ""
    if palette:
        palette_instruction = " Use only these printable filament colors: " + ", ".join(palette) + "."
    content = complete_text(
        (
            "Rewrite the user's request as one concise prompt for a text-to-3D model. "
            "Describe exactly one fused connected watertight printable object with stable geometry, a flat base, "
            "adequate wall thickness, and no unsupported details. Preserve the requested subject "
            "and style. Apply this visual profile: " + _style_profile(style, custom_style) + palette_instruction +
            " Return only the prompt, without markdown."
        ),
        instruction,
    )
    prompt = content.strip()
    if prompt.startswith("```") and prompt.endswith("```"):
        lines = prompt.splitlines()
        prompt = "\n".join(lines[1:-1]).strip()
    if not prompt:
        raise OpenAIPreprocessorError("The preprocessing service returned an empty prompt.")
    return prompt


def _image_kind(path: Path) -> tuple[str, str]:
    try:
        with path.open("rb") as stream:
            signature = stream.read(16)
    except OSError:
        raise OpenAIPreprocessorError("The input image could not be read.") from None
    if signature.startswith(b"\x89PNG\r\n\x1a\n"):
        return "image/png", "png"
    if signature.startswith(b"\xff\xd8\xff"):
        return "image/jpeg", "jpg"
    raise OpenAIPreprocessorError("The input image must be PNG or JPEG.")


def complete_vision(system_prompt: str, user_content: str, image_paths: tuple[Path, ...]) -> str:
    if not isinstance(system_prompt, str) or not system_prompt.strip():
        raise OpenAIPreprocessorError("A system prompt is required.")
    if not isinstance(user_content, str) or not user_content.strip():
        raise OpenAIPreprocessorError("User content is required.")
    if not image_paths or len(image_paths) > 2:
        raise OpenAIPreprocessorError("One or two review images are required.")
    total_size = 0
    content: list[dict[str, Any]] = [{"type": "text", "text": user_content}]
    for raw_path in image_paths:
        path = Path(raw_path)
        mime_type, _ = _image_kind(path)
        try:
            image = path.read_bytes()
        except OSError:
            raise OpenAIPreprocessorError("A review image could not be read.") from None
        total_size += len(image)
        if not image or total_size > _MAX_IMAGE_BYTES:
            raise OpenAIPreprocessorError("The combined review images exceed the 20 MB limit.")
        content.append(
            {
                "type": "image_url",
                "image_url": {
                    "url": f"data:{mime_type};base64," + base64.b64encode(image).decode("ascii"),
                    "detail": "high",
                },
            }
        )
    _, _, model, _ = _config()
    payload = {
        "model": model,
        "messages": [
            {"role": "system", "content": system_prompt},
            {"role": "user", "content": content},
        ],
    }
    result = _provider_request(
        "/chat/completions",
        json.dumps(payload, ensure_ascii=False, separators=(",", ":")).encode("utf-8"),
        "application/json",
    )
    response = _completion_content(result).strip()
    if not response:
        raise OpenAIPreprocessorError("The preprocessing service returned an empty response.")
    return response


def _bounded_recommendation_text(value: Any, field: str, maximum_bytes: int) -> str:
    if not isinstance(value, str) or not value.strip():
        raise OpenAIPreprocessorError(f"The palette recommendation {field} is required.")
    text = value.strip()
    if len(text.encode("utf-8")) > maximum_bytes:
        raise OpenAIPreprocessorError(f"The palette recommendation {field} is too long.")
    return text


def _palette_recommendation_payload(response: str) -> dict[str, Any]:
    text = response.strip()
    if text.startswith("```") and text.endswith("```"):
        lines = text.splitlines()
        if len(lines) >= 3:
            text = "\n".join(lines[1:-1]).strip()
    try:
        value = json.loads(text)
    except json.JSONDecodeError:
        raise OpenAIPreprocessorError("The palette recommendation is not valid JSON.") from None
    if not isinstance(value, dict):
        raise OpenAIPreprocessorError("The palette recommendation must be a JSON object.")
    return value


def recommend_printable_palette(
    instruction: str,
    style: str,
    custom_style: str = "",
    image_path: Path | None = None,
) -> PrintablePaletteRecommendation:
    """Recommend four provider-neutral design colors without binding Orca filament slots."""

    prompt = instruction.strip() if isinstance(instruction, str) else ""
    if not prompt and image_path is None:
        raise OpenAIPreprocessorError("A text instruction or reference image is required.")
    style_direction = _style_profile(style, custom_style)
    system_prompt = (
        "You are a color designer for printable 3D collectibles. Return exactly one JSON object and no markdown. "
        "Recommend exactly one four-color palette for the primary subject. The colors are ideal design targets, not known "
        "physical filaments. Use this schema: "
        '{"summary":string,"colors":[{"hex":"#RRGGBB","name":string,'
        '"role":"primary"|"structure"|"light"|"accent","usage":string,"reason":string}]}. '
        "Return every role exactly once. Choose broad solid material regions with strong perceptual separation; avoid gradients, "
        "near-duplicate shades, tiny accents, transparency, metallic effects and colors that only work as lighting. The primary "
        "color should cover the largest semantic region, structure should support silhouette and boundaries, light should provide "
        "a readable light material, and accent should distinguish one secondary semantic part. The accent should normally use a "
        "clearly different hue family from primary, not a lighter or darker substitute for the same material; only keep related "
        "hues when the subject semantics make that distinction unmistakable. Use concise Chinese for summary, "
        "name, usage and reason. Apply this style direction: "
        + style_direction
    )
    user_content = prompt or "Analyze the primary visible subject in the reference image."
    if image_path is None:
        response = complete_text(system_prompt, user_content)
    else:
        response = complete_vision(system_prompt, user_content, (Path(image_path),))

    payload = _palette_recommendation_payload(response)
    summary = _bounded_recommendation_text(
        payload.get("summary"), "summary", MAX_PALETTE_RECOMMENDATION_SUMMARY_BYTES
    )
    raw_colors = payload.get("colors")
    if not isinstance(raw_colors, list) or len(raw_colors) != len(PALETTE_ROLES):
        raise OpenAIPreprocessorError("The palette recommendation must contain exactly four colors.")

    records_by_role: dict[str, PrintablePaletteRecommendationColor] = {}
    raw_hex: list[str] = []
    for value in raw_colors:
        if not isinstance(value, dict):
            raise OpenAIPreprocessorError("Each palette recommendation color must be an object.")
        role = value.get("role")
        if not isinstance(role, str) or role not in PALETTE_ROLES or role in records_by_role:
            raise OpenAIPreprocessorError("The palette recommendation roles must be unique and complete.")
        raw_color = value.get("hex")
        if not isinstance(raw_color, str):
            raise OpenAIPreprocessorError("The palette recommendation color must use #RRGGBB format.")
        try:
            color = normalize_palette((raw_color,))[0]
        except PrintablePaletteError as exc:
            raise OpenAIPreprocessorError(str(exc)) from None
        raw_hex.append(color)
        records_by_role[role] = PrintablePaletteRecommendationColor(
            hex=color,
            name=_bounded_recommendation_text(
                value.get("name"), "color name", MAX_PALETTE_RECOMMENDATION_NAME_BYTES
            ),
            role=role,
            usage=_bounded_recommendation_text(
                value.get("usage"), "color usage", MAX_PALETTE_RECOMMENDATION_USAGE_BYTES
            ),
            reason=_bounded_recommendation_text(
                value.get("reason"), "color reason", MAX_PALETTE_RECOMMENDATION_REASON_BYTES
            ),
        )
    try:
        palette = normalize_palette(raw_hex)
        assignment = assign_palette_roles(palette, {role: records_by_role[role].hex for role in PALETTE_ROLES})
    except PrintablePaletteError as exc:
        raise OpenAIPreprocessorError(str(exc)) from None
    if len(palette) != len(PALETTE_ROLES):
        raise OpenAIPreprocessorError("The palette recommendation colors must be unique.")
    if assignment.low_contrast:
        raise OpenAIPreprocessorError("The palette recommendation does not provide enough color contrast.")
    return PrintablePaletteRecommendation(summary, tuple(records_by_role[role] for role in PALETTE_ROLES))


def _multipart_image(path: Path, instruction: str, model: str) -> tuple[bytes, str]:
    mime_type, extension = _image_kind(path)
    try:
        if path.stat().st_size > _MAX_IMAGE_BYTES:
            raise OpenAIPreprocessorError("The input image exceeds the 20 MB limit.")
        image = path.read_bytes()
    except OpenAIPreprocessorError:
        raise
    except OSError:
        raise OpenAIPreprocessorError("The input image could not be read.") from None
    boundary = "----OrcaAI" + uuid.uuid4().hex
    chunks: list[bytes] = []

    def field(name: str, value: str) -> None:
        chunks.extend(
            [
                f"--{boundary}\r\n".encode("ascii"),
                f'Content-Disposition: form-data; name="{name}"\r\n\r\n'.encode("ascii"),
                value.encode("utf-8"),
                b"\r\n",
            ]
        )

    field("model", model)
    field("prompt", instruction)
    chunks.extend(
        [
            f"--{boundary}\r\n".encode("ascii"),
            f'Content-Disposition: form-data; name="image"; filename="input.{extension}"\r\n'.encode("ascii"),
            f"Content-Type: {mime_type}\r\n\r\n".encode("ascii"),
            image,
            b"\r\n",
            f"--{boundary}--\r\n".encode("ascii"),
        ]
    )
    return b"".join(chunks), "multipart/form-data; boundary=" + boundary


def _validate_artifact_url(url: str) -> None:
    parsed = urllib.parse.urlsplit(url)
    if parsed.scheme.lower() != "https" or not parsed.hostname or parsed.username or parsed.password:
        raise OpenAIPreprocessorError("The preprocessing service returned an unsafe image location.")
    try:
        addresses = socket.getaddrinfo(parsed.hostname, parsed.port or 443, type=socket.SOCK_STREAM)
    except socket.gaierror:
        raise OpenAIPreprocessorError("The result image host could not be resolved.") from None
    if not addresses:
        raise OpenAIPreprocessorError("The result image host could not be resolved.")
    for address in addresses:
        try:
            ip = ipaddress.ip_address(address[4][0].split("%", 1)[0])
        except ValueError:
            raise OpenAIPreprocessorError("The preprocessing service returned an unsafe image location.") from None
        if not ip.is_global:
            raise OpenAIPreprocessorError("The preprocessing service returned an unsafe image location.")


class _SafeArtifactRedirects(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req: Any, fp: Any, code: int, msg: str, headers: Any, newurl: str) -> Any:
        _validate_artifact_url(newurl)
        return urllib.request.Request(newurl, headers={"Accept": "image/*"}, method="GET")


def _atomic_write(path: Path, data: bytes) -> Path:
    if len(data) > _MAX_IMAGE_BYTES:
        raise OpenAIPreprocessorError("The result image exceeds the 20 MB limit.")
    part = path.with_name(path.name + ".part")
    try:
        with part.open("wb") as stream:
            stream.write(data)
        os.replace(part, path)
    except OSError:
        try:
            part.unlink(missing_ok=True)
        except OSError:
            pass
        raise OpenAIPreprocessorError("The result image could not be written.") from None
    return path


def _download_image(url: str, output_path: Path) -> Path:
    _validate_artifact_url(url)
    request = urllib.request.Request(url, headers={"Accept": "image/*"}, method="GET")
    part = output_path.with_name(output_path.name + ".part")
    try:
        with urllib.request.build_opener(_SafeArtifactRedirects()).open(request, timeout=_TIMEOUT_SECONDS) as response:
            content_length = response.headers.get("Content-Length")
            if content_length:
                try:
                    if int(content_length) > _MAX_IMAGE_BYTES:
                        raise OpenAIPreprocessorError("The result image exceeds the 20 MB limit.")
                except ValueError:
                    pass
            total = 0
            with part.open("wb") as stream:
                while chunk := response.read(min(64 * 1024, _MAX_IMAGE_BYTES + 1 - total)):
                    total += len(chunk)
                    if total > _MAX_IMAGE_BYTES:
                        raise OpenAIPreprocessorError("The result image exceeds the 20 MB limit.")
                    stream.write(chunk)
        os.replace(part, output_path)
        return output_path
    except OpenAIPreprocessorError:
        try:
            part.unlink(missing_ok=True)
        except OSError:
            pass
        raise
    except (urllib.error.HTTPError, urllib.error.URLError, TimeoutError, OSError):
        try:
            part.unlink(missing_ok=True)
        except OSError:
            pass
        raise OpenAIPreprocessorError("The result image could not be downloaded.") from None


def _style_preview_prompt(
    instruction: str,
    palette: tuple[str, ...],
    style: str = "sculpture",
    shadow_color: str = "blue",
    palette_roles: Mapping[str, str] | None = None,
    custom_style: str = "",
) -> str:
    style_profile = _designer_toy_profile(style, custom_style) if palette else _style_profile(style, custom_style)
    color_direction = (
        _designer_toy_palette_direction(palette, shadow_color, palette_roles)
        if palette
        else "Use coherent natural colors that fit the subject and selected style. Preserve useful tonal modeling with broad, "
        "contiguous color regions for shape readability. "
    )
    return (
        "Transform the supplied reference into a polished designer-ready style preview for later image-to-3D. "
        "The supplied source image is the authority for the primary subject's identity and recognizable structure. "
        "User style and subject direction: "
        + instruction.strip()
        + "\nSelected style profile: "
        + style_profile
        + "\nImage-to-3D composition contract: "
        + _image_to_3d_composition_direction(bool(palette))
        + "Preserve the chosen subject's recognizable identity, facial expression, hairstyle, signature clothing or structural "
        "features, and visible pose. Simplify fine hair strands, fingers, jewelry, fabric patterns, foliage-like texture, and shallow "
        "surface noise into a few sturdy, connected, modelable forms. Do not turn the chosen person, animal, statue, building, or "
        "object into a different subject. For a real adult person, preserve adult age and natural facial feature sizes; do not "
        "enlarge the eyes, shrink the nose or mouth, narrow the jaw, or replace the face with a generic doll face. For an animal, "
        "preserve its actual coat pattern and markings; do not invent a white muzzle, chest patch, socks, blaze, or spots that are "
        "absent from the source. Do not invent unseen anatomy; use the explicit bust treatment for cropped people instead. "
        + color_direction
        + "Avoid dithering and tiny color speckles. Do not return the unchanged source."
    )


def build_style_preview_prompt(
    instruction: str,
    palette: tuple[str, ...],
    style: str = "sculpture",
    shadow_color: str = "blue",
    palette_roles: Mapping[str, str] | None = None,
    custom_style: str = "",
) -> str:
    """Return the exact provider prompt used for image-to-image previews.

    Benchmark and support tooling use this public boundary to persist an
    auditable request without duplicating the production prompt contract.
    """
    return _style_preview_prompt(instruction, palette, style, shadow_color, palette_roles, custom_style)


def _text_image_prompt(
    instruction: str,
    palette: tuple[str, ...],
    style: str = "q_cartoon",
    shadow_color: str = "blue",
    palette_roles: Mapping[str, str] | None = None,
    custom_style: str = "",
) -> str:
    if not isinstance(instruction, str) or not instruction.strip():
        raise OpenAIPreprocessorError("An image-generation prompt is required.")
    color_direction = (
        _designer_toy_palette_direction(palette, shadow_color, palette_roles)
        if palette
        else "Use coherent natural colors with broad, clean material regions. "
    )
    return (
        "Create one polished reference image for later image-to-3D generation. Subject: "
        + instruction.strip()
        + "\nSelected style profile: "
        + (_designer_toy_profile(style, custom_style) if palette else _style_profile(style, custom_style))
        + "\nPrintable composition constraints: Use one clearly readable primary subject, a complete silhouette, a stable pose, "
        "simple depth layering, large closed color regions, hard clean boundaries, and only structurally meaningful details. "
        "Treat the user description as a closed component inventory: do not add plausible category features, accessories, handles, "
        "tools, rods, decorations, or secondary objects that were not explicitly requested. Simplify ambiguous details instead of inventing them. "
        + ("Use a transparent background with no cast shadow. " if palette else "")
        + color_direction
        + ("Use palette colors only as solid semantic material regions, never as lighting highlights, reflections, rim light, or shading bands. " if palette else "")
        + "Do not use gradients, semi-transparent subject materials, soft shadows, photographic reflections, depth of field, blur, dithering, "
        "halftone dots, random noise, tiny isolated regions, dense texture, text, watermark, frame, or decorative clutter. "
        "The deterministic print pipeline will enforce the exact palette, so prioritize shape readability over tonal realism."
    )


def _save_provider_image(result: dict[str, Any], destination: Path) -> Path:
    data = result.get("data")
    item = data[0] if isinstance(data, list) and data and isinstance(data[0], dict) else None
    if not item:
        raise OpenAIPreprocessorError("The preprocessing service returned no image.")
    encoded = item.get("b64_json")
    if isinstance(encoded, str) and encoded:
        try:
            image = base64.b64decode(encoded, validate=True)
        except (ValueError, binascii.Error):
            raise OpenAIPreprocessorError("The preprocessing service returned invalid image data.") from None
        return _atomic_write(destination, image)
    url = item.get("url")
    if isinstance(url, str) and url:
        return _download_image(url, destination)
    raise OpenAIPreprocessorError("The preprocessing service returned no usable image.")


def generate_image(
    instruction: str,
    output_path: str | os.PathLike[str],
    palette: tuple[str, ...],
    style: str = "q_cartoon",
    shadow_color: str = "blue",
    palette_roles: Mapping[str, str] | None = None,
    custom_style: str = "",
) -> Path:
    destination = Path(output_path)
    _, _, _, model = _config()
    payload = json.dumps(
        {
            "model": model,
            "prompt": _text_image_prompt(instruction, palette, style, shadow_color, palette_roles, custom_style),
            "size": "1024x1024",
            "n": 1,
            "response_format": "b64_json",
        },
        ensure_ascii=False,
    ).encode("utf-8")
    return _save_provider_image(_provider_request("/images/generations", payload, "application/json"), destination)


def preprocess_image(
    input_path: str | os.PathLike[str],
    instruction: str,
    output_path: str | os.PathLike[str],
    palette: tuple[str, ...],
    style: str = "sculpture",
    shadow_color: str = "blue",
    palette_roles: Mapping[str, str] | None = None,
    custom_style: str = "",
) -> Path:
    if not isinstance(instruction, str) or not instruction.strip():
        raise OpenAIPreprocessorError("An image-edit instruction is required.")
    return edit_image(
        input_path,
        build_style_preview_prompt(instruction, palette, style, shadow_color, palette_roles, custom_style),
        output_path,
    )


def edit_image(
    input_path: str | os.PathLike[str],
    prompt: str,
    output_path: str | os.PathLike[str],
) -> Path:
    """Edit one image with an exact caller-owned prompt.

    Domain modules such as multiview preparation own their prompt contract while
    this adapter remains responsible only for the OpenAI-compatible transport.
    """
    if not isinstance(prompt, str) or not prompt.strip():
        raise OpenAIPreprocessorError("An image-edit prompt is required.")
    source = Path(input_path)
    destination = Path(output_path)
    _, _, _, model = _config()
    body, content_type = _multipart_image(source, prompt.strip(), model)
    result = _provider_request("/images/edits", body, content_type)
    return _save_provider_image(result, destination)
