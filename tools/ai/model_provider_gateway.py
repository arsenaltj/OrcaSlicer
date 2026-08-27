from __future__ import annotations

from dataclasses import dataclass
import os
from pathlib import Path
from typing import Any, Callable, Mapping

try:
    from .tripo_client import (
        TripoError,
        create_conversion,
        create_image_task,
        create_text_task,
        download_task_artifact,
        upload_image,
        wait_for_task,
    )
except ImportError:
    from tripo_client import (
        TripoError,
        create_conversion,
        create_image_task,
        create_text_task,
        download_task_artifact,
        upload_image,
        wait_for_task,
    )


_MODEL_FACE_LIMITS = (100000, 300000, 500000, 1000000)
_GENERATION_PROFILE_FACE_LIMITS = {"quality": 1000000, "performance": 300000}


@dataclass(frozen=True)
class ProviderPolicy:
    design_providers: tuple[str, ...] = ("gpt", "image2")
    geometry_provider: str = "tripo"
    automatic_fallback: bool = False
    max_paid_model_tasks_per_confirmation: int = 1


def provider_policy() -> ProviderPolicy:
    return ProviderPolicy()


@dataclass(frozen=True)
class ModelTaskRequest:
    source: str
    prompt: str = ""
    image_path: Path | None = None
    face_limit: int = 1000000
    generation_profile: str = "quality"


@dataclass(frozen=True)
class ProviderTaskRef:
    provider: str
    task_id: str
    reused: bool


class ProviderGatewayError(RuntimeError):
    def __init__(
        self,
        message: str,
        *,
        code: str,
        category: str,
        provider: str = "",
        operation: str = "",
        retryable: bool = False,
        ambiguous: bool = False,
    ) -> None:
        super().__init__(message)
        self.code = code
        self.category = category
        self.provider = provider
        self.operation = operation
        self.retryable = retryable
        self.ambiguous = ambiguous


class PaidTaskAuthorization:
    def __init__(self, request_id: str, provider: str, operation: str) -> None:
        self.request_id = request_id
        self.provider = provider
        self.operation = operation
        self._consumed = False

    @classmethod
    def confirmed(cls, request_id: str) -> PaidTaskAuthorization:
        normalized = request_id.strip() if isinstance(request_id, str) else ""
        if not normalized:
            raise ProviderGatewayError(
                "A paid task authorization requires a request ID.",
                code="invalid_authorization",
                category="authorization",
            )
        return cls(normalized, "tripo", "model_generation")

    @property
    def consumed(self) -> bool:
        return self._consumed

    def consume(self, provider: str, operation: str) -> None:
        if provider != self.provider or operation != self.operation:
            raise ProviderGatewayError(
                "The paid task authorization does not cover this provider operation.",
                code="authorization_scope_mismatch",
                category="authorization",
                provider=provider,
                operation=operation,
            )
        if self._consumed:
            raise ProviderGatewayError(
                "The paid task authorization has already been consumed.",
                code="authorization_consumed",
                category="authorization",
                provider=provider,
                operation=operation,
            )
        self._consumed = True


def _classify_tripo_error(
    error: TripoError,
    operation: str,
    *,
    creation_ambiguous: bool = False,
) -> ProviderGatewayError:
    message = str(error) or "The model provider request failed."
    lowered = message.lower()
    code = "provider_failed"
    category = "provider"
    retryable = False
    ambiguous = False
    if "not configured" in lowered:
        code, category = "provider_not_configured", "configuration"
    elif "rate limiting" in lowered or "rate limit" in lowered:
        code, category, retryable = "provider_rate_limited", "availability", True
    elif "deadline expired" in lowered or "timed out" in lowered or "timeout" in lowered:
        code, category, retryable = "provider_timeout", "availability", True
        ambiguous = creation_ambiguous
    elif "cancelled" in lowered or "canceled" in lowered:
        code, category = "provider_cancelled", "cancellation"
    elif "could not connect" in lowered or "temporarily unavailable" in lowered:
        code, category, retryable = "provider_unavailable", "availability", True
        ambiguous = creation_ambiguous
    elif "unsafe artifact" in lowered or "unsafe artifact location" in lowered:
        code, category = "unsafe_artifact", "security"
    elif "invalid" in lowered or "oversized" in lowered or "no downloadable artifact" in lowered:
        code, category = "invalid_provider_result", "validation"
    elif "rejected" in lowered:
        code, category = "provider_rejected", "request"
    return ProviderGatewayError(
        message,
        code=code,
        category=category,
        provider="tripo",
        operation=operation,
        retryable=retryable,
        ambiguous=ambiguous,
    )


class ModelProviderGateway:
    def __init__(
        self,
        *,
        create_text_task: Callable[[str, int, str], str] = create_text_task,
        upload_image: Callable[[str | os.PathLike[str]], str] = upload_image,
        create_image_task: Callable[[str, int, str], str] = create_image_task,
        create_conversion: Callable[[str, str], str] = create_conversion,
        wait_for_task: Callable[..., dict[str, Any]] = wait_for_task,
        download_task_artifact: Callable[[Mapping[str, Any], str | os.PathLike[str], int], Path] =
            download_task_artifact,
    ) -> None:
        self._create_text_task = create_text_task
        self._upload_image = upload_image
        self._create_image_task = create_image_task
        self._create_conversion = create_conversion
        self._wait_for_task = wait_for_task
        self._download_task_artifact = download_task_artifact

    def model_generation_available(self) -> bool:
        return bool(os.environ.get("TRIPO_API_KEY", ""))

    def start_or_reuse_model_task(
        self,
        request: ModelTaskRequest,
        *,
        existing_task_id: str = "",
        authorization: PaidTaskAuthorization | None = None,
    ) -> ProviderTaskRef:
        if not isinstance(existing_task_id, str):
            raise ProviderGatewayError(
                "The existing model task reference is invalid.",
                code="invalid_task_reference",
                category="validation",
                provider="tripo",
                operation="model_generation",
            )
        existing = existing_task_id.strip()
        if existing:
            return ProviderTaskRef(provider="tripo", task_id=existing, reused=True)

        source = request.source.strip().lower() if isinstance(request.source, str) else ""
        if request.face_limit not in _MODEL_FACE_LIMITS:
            raise ProviderGatewayError(
                "The model face target must be 100000, 300000, 500000, or 1000000 triangles.",
                code="invalid_model_request",
                category="validation",
                provider="tripo",
                operation="model_generation",
            )
        if request.generation_profile not in {"quality", "performance"}:
            raise ProviderGatewayError(
                "The generation profile must be quality or performance.",
                code="invalid_model_request",
                category="validation",
                provider="tripo",
                operation="model_generation",
            )
        if request.face_limit != _GENERATION_PROFILE_FACE_LIMITS[request.generation_profile]:
            raise ProviderGatewayError(
                "The model face target does not match the selected generation profile.",
                code="invalid_model_request",
                category="validation",
                provider="tripo",
                operation="model_generation",
            )
        prompt = request.prompt.strip() if isinstance(request.prompt, str) else ""
        image_path = Path(request.image_path) if request.image_path is not None else None
        if source == "text" and not prompt:
            raise ProviderGatewayError(
                "A text prompt is required.",
                code="invalid_model_request",
                category="validation",
                provider="tripo",
                operation="model_generation",
            )
        if source == "image" and (image_path is None or not image_path.is_file()):
            raise ProviderGatewayError(
                "A readable model reference image is required.",
                code="invalid_model_request",
                category="validation",
                provider="tripo",
                operation="model_generation",
            )
        if source not in {"text", "image"}:
            raise ProviderGatewayError(
                "The model request source is unsupported.",
                code="invalid_model_request",
                category="validation",
                provider="tripo",
                operation="model_generation",
            )
        if authorization is None:
            raise ProviderGatewayError(
                "Explicit confirmation is required before creating a paid model task.",
                code="authorization_required",
                category="authorization",
                provider="tripo",
                operation="model_generation",
            )
        authorization.consume("tripo", "model_generation")
        try:
            if source == "text":
                task_id = self._create_text_task(prompt, request.face_limit, request.generation_profile)
            else:
                assert image_path is not None
                token = self._upload_image(image_path)
                task_id = self._create_image_task(token, request.face_limit, request.generation_profile)
        except TripoError as error:
            raise _classify_tripo_error(error, "model_generation", creation_ambiguous=True) from None
        if not isinstance(task_id, str) or not task_id.strip():
            raise ProviderGatewayError(
                "The model provider returned an invalid task reference.",
                code="invalid_provider_result",
                category="validation",
                provider="tripo",
                operation="model_generation",
                ambiguous=True,
            )
        return ProviderTaskRef(provider="tripo", task_id=task_id.strip(), reused=False)

    def start_or_reuse_conversion(
        self,
        generation_task_id: str,
        format_name: str,
        *,
        existing_task_id: str = "",
        allow_create: bool,
    ) -> ProviderTaskRef:
        generation_id = generation_task_id.strip() if isinstance(generation_task_id, str) else ""
        output_format = format_name.strip().lower() if isinstance(format_name, str) else ""
        existing = existing_task_id.strip() if isinstance(existing_task_id, str) else ""
        if not generation_id or not output_format:
            raise ProviderGatewayError(
                "A generation task and output format are required for conversion.",
                code="invalid_conversion_request",
                category="validation",
                provider="tripo",
                operation="model_conversion",
            )
        if existing:
            return ProviderTaskRef(provider="tripo", task_id=existing, reused=True)
        if not allow_create:
            raise ProviderGatewayError(
                "A new conversion task was not authorized for this recovery step.",
                code="conversion_creation_not_allowed",
                category="authorization",
                provider="tripo",
                operation="model_conversion",
            )
        try:
            task_id = self._create_conversion(generation_id, output_format)
        except TripoError as error:
            raise _classify_tripo_error(error, "model_conversion", creation_ambiguous=True) from None
        if not isinstance(task_id, str) or not task_id.strip():
            raise ProviderGatewayError(
                "The model provider returned an invalid conversion task reference.",
                code="invalid_provider_result",
                category="validation",
                provider="tripo",
                operation="model_conversion",
                ambiguous=True,
            )
        return ProviderTaskRef(provider="tripo", task_id=task_id.strip(), reused=False)

    def wait_for_task(
        self,
        task_id: str,
        *,
        stop_event: Any = None,
        progress: Callable[[int | float | None], None] | None = None,
    ) -> dict[str, Any]:
        normalized = task_id.strip() if isinstance(task_id, str) else ""
        if not normalized:
            raise ProviderGatewayError(
                "A provider task reference is required.",
                code="invalid_task_reference",
                category="validation",
                provider="tripo",
                operation="task_poll",
            )
        try:
            return self._wait_for_task(normalized, stop_event=stop_event, progress=progress)
        except TripoError as error:
            raise _classify_tripo_error(error, "task_poll") from None

    def download_artifact(
        self,
        task_result: Mapping[str, Any],
        output_path: str | os.PathLike[str],
        max_bytes: int,
    ) -> Path:
        if max_bytes <= 0:
            raise ProviderGatewayError(
                "The artifact size limit must be positive.",
                code="invalid_download_request",
                category="validation",
                provider="tripo",
                operation="artifact_download",
            )
        try:
            return self._download_task_artifact(task_result, output_path, max_bytes)
        except TripoError as error:
            raise _classify_tripo_error(error, "artifact_download") from None
