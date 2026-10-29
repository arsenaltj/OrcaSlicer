"""YoYoClaw adapters for model generation and provider catalog discovery."""
from __future__ import annotations

from pathlib import Path
import re
from typing import Any, Mapping

try:
    from .model_provider_gateway import ModelTaskRequest, PaidTaskAuthorization, ProviderGatewayError, ProviderTaskRef
    from .yoyoclaw_client import YoYoClawClient, YoYoClawError
except ImportError:  # pragma: no cover - direct sidecar execution
    from model_provider_gateway import ModelTaskRequest, PaidTaskAuthorization, ProviderGatewayError, ProviderTaskRef
    from yoyoclaw_client import YoYoClawClient, YoYoClawError


IMAGE_LIBRARY = "image-model-library"
GEOMETRY_LIBRARY = "3d-model-library"
IMAGE_ROLE = "image"
GEOMETRY_ROLE = "geometry"
PREFERRED_IMAGE_MODEL = f"{IMAGE_LIBRARY}/Qwen-Image-3.0-PC-YOYO"
PREFERRED_GEOMETRY_MODEL = f"{GEOMETRY_LIBRARY}/Hy-3D-3.1-PC-YOYO"


def validate_options(face_limit: int, geometry_quality: str, texture_quality: str) -> None:
    if face_limit not in {300000, 1000000} or geometry_quality != "standard" or texture_quality != "standard":
        raise ProviderGatewayError(
            "YOYOClaw supports 300000 or 1000000 faces here, with standard geometry and texture.",
            code="invalid_model_request", category="validation", provider="yoyoclaw", operation="model_generation",
        )


def _error(error: YoYoClawError, operation: str) -> ProviderGatewayError:
    return ProviderGatewayError(
        str(error), code=error.code, category=error.category, provider="yoyoclaw",
        operation=operation, retryable=error.retryable, ambiguous=error.ambiguous,
    )


def _normalise_model(value: str, library: str) -> str:
    value = value.strip()
    prefix = library + "/"
    return value if value.startswith(prefix) else prefix + value


def _supported_model(name: str, library: str) -> bool:
    """Validate the catalog namespace, leaving model eligibility to the whitelist.

    The provider endpoint already separates image and geometry libraries.  A
    model-name prefix would hide newly published, otherwise usable models and
    would make the UI depend on a stale vendor naming convention.
    """
    prefix = library + "/"
    return (
        isinstance(name, str)
        and name.startswith(prefix)
        and bool(name[len(prefix):].strip())
    )


_UNAVAILABLE_STATUS = {
    "unavailable",
    "disabled",
    "inactive",
    "offline",
    "down",
    "maintenance",
    "deprecated",
    "retired",
    "removed",
    "unlisted",
    "not_available",
    "not_ready",
    "error",
    "failed",
    "blocked",
    "suspended",
    "expired",
}


def _is_explicitly_unavailable(item: Mapping[str, Any]) -> bool:
    """Ignore whitelist rows that explicitly say the model cannot be used.

    YoYoClaw deployments have returned both boolean flags and textual status
    fields over time.  Missing fields remain compatible with older responses;
    only an explicit negative value/status removes a model from the catalog.
    """
    for key in ("available", "enabled", "active", "is_available", "is_enabled"):
        value = item.get(key)
        if isinstance(value, bool) and not value:
            return True
        if isinstance(value, (int, float)) and value == 0:
            return True
        if isinstance(value, str) and value.strip().lower() in {
            "0", "false", "no", "off", "disabled", "unavailable",
        }:
            return True

    for key in ("status", "state"):
        value = item.get(key)
        if isinstance(value, str):
            normalized = value.strip().lower().replace("-", "_").replace(" ", "_")
            if normalized in _UNAVAILABLE_STATUS:
                return True
    return False


def _model_entries(raw: list[dict[str, Any]], library: str) -> list[dict[str, Any]]:
    entries: list[dict[str, Any]] = []
    seen: set[str] = set()
    for item in raw:
        if not isinstance(item, Mapping) or _is_explicitly_unavailable(item):
            continue
        raw_name = ""
        for key in ("id", "model_id", "modelId", "model", "name"):
            candidate = item.get(key)
            if isinstance(candidate, str) and candidate.strip():
                raw_name = candidate
                break
        if not isinstance(raw_name, str) or not raw_name.strip():
            continue
        model_id = _normalise_model(raw_name, library)
        if not _supported_model(model_id, library) or model_id in seen:
            continue
        seen.add(model_id)
        display_name = item.get("display_name") or item.get("label") or item.get("title")
        if not isinstance(display_name, str) or not display_name.strip():
            name_value = item.get("name")
            display_name = name_value if isinstance(name_value, str) and name_value.strip() else raw_name.rsplit("/", 1)[-1]
        is_default = any(
            item.get(key) is True
            for key in ("default", "is_default", "isDefault")
        )
        entries.append({
            "id": model_id,
            "name": str(display_name),
            "available": True,
            "raw_name": raw_name,
            "default": is_default,
            "multiplier": item.get("multiplier"),
            "discount_multiplier": item.get("discount_multiplier"),
        })
    return entries


def _compatible_version(model_id: str, role: str) -> tuple[int, ...] | None:
    name = model_id.rsplit("/", 1)[-1].strip().lower()
    pattern = r"qwen-image-(\d+(?:\.\d+)*)(?:[-_].*)?" if role == IMAGE_ROLE else \
              r"hy-3d-(\d+(?:\.\d+)*)(?:[-_].*)?"
    match = re.fullmatch(pattern, name)
    if match is None:
        return None
    return tuple(int(part) for part in match.group(1).split("."))


def _resolve_preferred_model(entries: list[dict[str, Any]], role: str,
                             server_default: str = "") -> dict[str, Any] | None:
    preferred = PREFERRED_IMAGE_MODEL if role == IMAGE_ROLE else PREFERRED_GEOMETRY_MODEL
    by_id = {
        str(item.get("id", "")).casefold(): item
        for item in entries if isinstance(item.get("id"), str)
    }
    exact = by_id.get(preferred.casefold())
    if exact is not None:
        return exact

    library = IMAGE_LIBRARY if role == IMAGE_ROLE else GEOMETRY_LIBRARY
    if isinstance(server_default, str) and server_default.strip():
        normalized_default = _normalise_model(server_default, library)
        candidate = by_id.get(normalized_default.casefold())
        if candidate is not None and _compatible_version(str(candidate["id"]), role) is not None:
            return candidate

    compatible = [
        (_compatible_version(str(item["id"]), role), str(item["id"]).casefold(), item)
        for item in entries
        if _compatible_version(str(item["id"]), role) is not None
    ]
    if not compatible:
        return None
    compatible.sort(key=lambda candidate: (candidate[0], candidate[1]), reverse=True)
    return compatible[0][2]


class YoYoClawModelProviderGateway:
    def __init__(self, client_factory: Any = YoYoClawClient) -> None:
        self._client_factory = client_factory
        self._task_models: dict[str, str] = {}

    def _client(self) -> YoYoClawClient:
        return self._client_factory()

    def model_generation_available(self) -> bool:
        return bool(self._client().config.configured)

    def catalog(self, role: str) -> dict[str, Any]:
        if role not in {IMAGE_ROLE, GEOMETRY_ROLE}:
            return {
                "role": role, "provider": "yoyoclaw", "state": "error", "models": [],
                "default_model": "", "error": {"code": "invalid_role", "message": "Unsupported model catalog role."},
            }
        library = IMAGE_LIBRARY if role == IMAGE_ROLE else GEOMETRY_LIBRARY
        if not self.model_generation_available():
            return {
                "role": role, "provider": "yoyoclaw", "state": "not_configured", "models": [],
                "default_model": "", "error": {"code": "provider_not_configured", "message": "YOYOClaw credentials are not configured."},
            }
        client = self._client()
        try:
            catalog_reader = getattr(client, "whitelist_catalog", None)
            if callable(catalog_reader):
                catalog_payload = catalog_reader(library)
                raw_models = catalog_payload.get("models", []) if isinstance(catalog_payload, Mapping) else []
                server_default = catalog_payload.get("default_model", "") if isinstance(catalog_payload, Mapping) else ""
            else:
                raw_models = client.whitelist(library)
                server_default = ""
            entries = _model_entries(raw_models, library)
        except YoYoClawError as exc:
            return {
                "role": role, "provider": "yoyoclaw", "state": "error", "models": [], "default_model": "",
                "error": {"code": exc.code, "message": str(exc), "retryable": exc.retryable},
            }
        selected = _resolve_preferred_model(entries, role, server_default)
        selected_models = [selected] if selected is not None else []
        default = str(selected["id"]) if selected is not None else ""
        return {
            "role": role, "provider": "yoyoclaw", "state": "ready" if selected is not None else "empty",
            "models": selected_models, "default_model": default,
            "error": None if selected is not None else {
                "code": "compatible_model_unavailable",
                "message": "No compatible YOYOClaw model is available for this function.",
            },
        }

    def _validate_geometry_model(self, model_id: str) -> str:
        if not isinstance(model_id, str) or not model_id.strip():
            raise ProviderGatewayError(
                "A YOYOClaw 3D model must be selected.", code="model_required", category="validation",
                provider="yoyoclaw", operation="model_generation",
            )
        normalized = _normalise_model(model_id, GEOMETRY_LIBRARY)
        if not _supported_model(normalized, GEOMETRY_LIBRARY):
            raise ProviderGatewayError(
                "The selected YOYOClaw model is not supported for image-to-3D generation.",
                code="model_unavailable", category="validation", provider="yoyoclaw", operation="model_generation",
            )
        return normalized

    def start_or_reuse_model_task(
        self,
        request: ModelTaskRequest,
        *,
        existing_task_id: str = "",
        authorization: PaidTaskAuthorization | None = None,
    ) -> ProviderTaskRef:
        if not isinstance(existing_task_id, str):
            raise ProviderGatewayError("Invalid YOYOClaw task reference.", code="invalid_task_reference", category="validation", provider="yoyoclaw")
        existing = existing_task_id.strip()
        if existing:
            return ProviderTaskRef("yoyoclaw", existing, True)
        if request.source != "image" or request.image_path is None or not Path(request.image_path).is_file():
            raise ProviderGatewayError(
                "YOYOClaw requires the confirmed 2D design image as its 3D input.",
                code="invalid_model_request", category="validation", provider="yoyoclaw", operation="model_generation",
            )
        if request.output_format not in {"glb", "obj"}:
            raise ProviderGatewayError("Unsupported YOYOClaw output format.", code="invalid_model_request", category="validation", provider="yoyoclaw", operation="model_generation")
        model_id = self._validate_geometry_model(request.model_id)
        catalog = self.catalog(GEOMETRY_ROLE)
        available_models = {
            item.get("id") for item in catalog.get("models", []) if isinstance(item, Mapping)
        }
        if catalog.get("state") != "ready" or model_id not in available_models:
            error = catalog.get("error") if isinstance(catalog.get("error"), Mapping) else {}
            raise ProviderGatewayError(
                str(error.get("message") or "The selected YOYOClaw 3D model is unavailable."),
                code=str(error.get("code") or "model_unavailable"), category="availability",
                provider="yoyoclaw", operation="model_generation", retryable=bool(error.get("retryable")),
            )
        if authorization is None:
            raise ProviderGatewayError(
                "Explicit confirmation is required before creating a paid model task.",
                code="authorization_required", category="authorization", provider="yoyoclaw", operation="model_generation",
            )
        client = self._client()
        authorization.consume("yoyoclaw", "model_generation")
        try:
            task_id = client.submit_3d(model_id, Path(request.image_path))
        except YoYoClawError as exc:
            raise _error(exc, "model_generation") from None
        self._task_models[task_id] = model_id
        return ProviderTaskRef("yoyoclaw", task_id, False)

    def wait_for_task(self, task_id: str, *, stop_event: Any = None, progress: Any = None,
                      model_id: str = "") -> dict[str, Any]:
        model_id = model_id.strip() if isinstance(model_id, str) else ""
        if not model_id:
            model_id = self._task_models.get(task_id, "")
        if not model_id:
            catalog = self.catalog(GEOMETRY_ROLE)
            model_id = str(catalog.get("default_model", ""))
        if not model_id:
            raise ProviderGatewayError(
                "The YOYOClaw 3D model used by this task is unavailable.",
                code="model_unavailable", category="availability", provider="yoyoclaw", operation="task_poll",
            )
        model_id = self._validate_geometry_model(model_id)
        self._task_models[task_id] = model_id
        try:
            return self._client().wait_3d(model_id, task_id, stop_event=stop_event, progress=progress)
        except YoYoClawError as exc:
            raise _error(exc, "task_poll") from None

    def download_artifact(
        self,
        task_result: Mapping[str, Any],
        output_path: str | Path,
        max_bytes: int,
        *,
        output_format: str = "glb",
        progress: Any = None,
    ) -> Path:
        try:
            options = {"progress": progress} if progress is not None else {}
            return self._client().download_result_artifact(task_result, output_path, max_bytes, output_format=output_format, **options)
        except YoYoClawError as exc:
            raise _error(exc, "artifact_download") from None


def generate_design_image(
    model: str,
    prompt: str,
    output_path: str | Path,
    *,
    image_path: Path | None = None,
    client_factory: Any = YoYoClawClient,
) -> Path:
    """Generate and download a YoYoClaw 2D design into the existing preview path."""
    if not isinstance(model, str) or not model.strip() or not _supported_model(_normalise_model(model, IMAGE_LIBRARY), IMAGE_LIBRARY):
        raise YoYoClawError("A supported YOYOClaw image model must be selected.", code="model_unavailable", category="validation")
    client = client_factory()
    try:
        model_id = _normalise_model(model, IMAGE_LIBRARY)
        catalog_reader = getattr(client, "whitelist_catalog", None)
        if callable(catalog_reader):
            catalog_payload = catalog_reader(IMAGE_LIBRARY)
            raw_models = catalog_payload.get("models", []) if isinstance(catalog_payload, Mapping) else []
            server_default = catalog_payload.get("default_model", "") if isinstance(catalog_payload, Mapping) else ""
        else:
            raw_models = client.whitelist(IMAGE_LIBRARY)
            server_default = ""
        entries = _model_entries(raw_models, IMAGE_LIBRARY)
        selected = _resolve_preferred_model(entries, IMAGE_ROLE, server_default)
        if selected is None or model_id.casefold() != str(selected["id"]).casefold():
            raise YoYoClawError("The selected YOYOClaw image model is unavailable.", code="model_unavailable", category="availability")
        model_id = str(selected["id"])
        url = client.generate_image(model_id, prompt, image_path=image_path)
        return client.download(url, output_path, 20 * 1024 * 1024)
    except YoYoClawError:
        raise
