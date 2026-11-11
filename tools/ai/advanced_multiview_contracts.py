"""Advanced-only, transport-neutral multiview identities and input contracts."""
from __future__ import annotations

import hashlib
import json
import uuid
from typing import Any, Mapping

from model_contracts import MAX_PROMPT_BYTES, RequestError
from model_request import _normalize_custom_style, _normalize_style

VIEWS = ("front", "left", "back", "right")
VIEW_CONTRACT_SCHEMA = "advanced-relative-camera-v2"
GROUP_SCHEMA = "advanced-multiview-group-v1"
OPERATION_SCHEMA = "advanced-multiview-operation-v1"
VERSION_SCHEMA = "advanced-multiview-design-v1"
PLAN_SCHEMA = "advanced-multiview-plan-v1"
CREATION_KIND = "advanced_multiview_v1"
MAX_GROUP_BYTES = 256 * 1024
MAX_RECORD_BYTES = 256 * 1024
ACTIVE_ATTEMPTS = frozenset({"planned", "blocked", "queued", "submitting", "running"})
MAX_DISPLAY_NAME_BYTES = 240

VIEW_CAMERA_CONTRACTS = {
    "front": {
        "signed_yaw_degrees": 0,
        "top_view_orbit": "none",
        "camera_result": "Use the exact main-reference camera as the zero-degree view.",
        "conditional_screen_result": "Preserve the main reference's existing semantic orientation exactly.",
    },
    "left": {
        "signed_yaw_degrees": 90,
        "top_view_orbit": "clockwise",
        "camera_result": "Move the camera to the relative-left side of the frozen subject.",
        "conditional_screen_result": (
            "If the subject's semantic front approximately faces the zero-degree camera, show its physical left side; "
            "a human or animal nose points toward image-right and the left ear/left-side features are the near side."
        ),
    },
    "back": {
        "signed_yaw_degrees": 180,
        "top_view_orbit": "half-turn",
        "camera_result": "Move the camera to the side opposite the zero-degree main-reference camera.",
        "conditional_screen_result": (
            "If the subject's semantic front approximately faces the zero-degree camera, show the physical rear; "
            "front-only facial or facade details must not remain visible."
        ),
    },
    "right": {
        "signed_yaw_degrees": -90,
        "top_view_orbit": "counterclockwise",
        "camera_result": "Move the camera to the relative-right side of the frozen subject.",
        "conditional_screen_result": (
            "If the subject's semantic front approximately faces the zero-degree camera, show its physical right side; "
            "a human or animal nose points toward image-left and the right ear/right-side features are the near side."
        ),
    },
}


def view_contract_snapshot() -> dict[str, Any]:
    """Return the stable advanced-only camera contract frozen into a confirmed plan."""
    return {
        "schema": VIEW_CONTRACT_SCHEMA,
        "views": {view: dict(VIEW_CAMERA_CONTRACTS[view]) for view in VIEWS},
    }


def identity(value: Any, name: str = "id") -> str:
    if not isinstance(value, str):
        raise RequestError("invalid_request", f"{name} must be a canonical UUID.", 400)
    try:
        normalized = str(uuid.UUID(value))
    except ValueError:
        raise RequestError("invalid_request", f"{name} must be a canonical UUID.", 400) from None
    if value != normalized:
        raise RequestError("invalid_request", f"{name} must be a canonical UUID.", 400)
    return normalized


def new_id() -> str:
    return str(uuid.uuid4())


def fingerprint(value: Any) -> str:
    return hashlib.sha256(json.dumps(value, ensure_ascii=False, sort_keys=True,
                                     separators=(",", ":"), allow_nan=False).encode("utf-8")).hexdigest()


def text(value: Any, name: str, maximum: int, *, required: bool = False) -> str:
    if not isinstance(value, str) or len(value.strip().encode("utf-8")) > maximum:
        raise RequestError("invalid_request", f"{name} must be text within its byte limit.", 400)
    value = value.strip()
    if required and not value:
        raise RequestError("invalid_request", f"{name} is required.", 400)
    return value


def display_name(value: Any) -> str:
    value = text(value, "display_name", MAX_DISPLAY_NAME_BYTES)
    if any(ord(character) < 32 for character in value):
        raise RequestError("invalid_request", "display_name must be a single line.", 400)
    return value


def draft_fields(value: Mapping[str, Any]) -> dict[str, Any]:
    """Reuse existing style semantics, without mutating the legacy defaults."""
    if not isinstance(value, Mapping):
        raise RequestError("invalid_request", "A draft object is required.", 400)
    style = _normalize_style(value.get("style"))
    provider = value.get("design_provider", "gpt")
    if not isinstance(provider, str) or provider not in {"gpt", "yoyoclaw"}:
        raise RequestError("invalid_request", "Unsupported design provider.", 400)
    return {
        "prompt": text(value.get("prompt", ""), "prompt", MAX_PROMPT_BYTES),
        "style": style,
        "custom_style": _normalize_custom_style(value.get("custom_style", ""), style),
        "design_provider": provider,
        "design_model": text(value.get("design_model", ""), "design_model", 256),
    }


def targets(value: Any, *, default_all: bool = True) -> tuple[str, ...]:
    if value is None:
        value = []
    if not isinstance(value, (list, tuple)) or any(not isinstance(v, str) or v not in VIEWS for v in value):
        raise RequestError("invalid_request", "Named front, left, back and right views are required.", 400)
    if len(value) != len(set(value)):
        raise RequestError("invalid_request", "Duplicate view names are not allowed.", 400)
    if not value and default_all:
        return VIEWS
    return tuple(view for view in VIEWS if view in value)


def revision(value: Any) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        raise RequestError("invalid_request", "expected_group_revision must be a nonnegative integer.", 400)
    return value


def check_revision(group: Mapping[str, Any], expected: Any) -> None:
    if group["revision"] != revision(expected):
        raise RequestError("revision_conflict", "The group changed; refresh before submitting.", 409)
