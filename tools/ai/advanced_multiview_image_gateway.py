"""Named-reference adapters used only by the advanced workflow.

Legacy single-image prompts and transports remain unchanged. Every method makes
at most one creation request; failed downloads never cause another generation.
"""
from __future__ import annotations

import json
from pathlib import Path
import uuid
from typing import Any

from advanced_multiview_contracts import VIEW_CAMERA_CONTRACTS
from advanced_multiview_design_workflow import DesignImageRequest, DesignImageResult
from model_contracts import DEFAULT_IMAGE_INSTRUCTION, MAX_IMAGE_BYTES
from model_provider_gateway import ProviderGatewayError
from yoyoclaw_provider_gateway import IMAGE_ROLE, YoYoClawModelProviderGateway
import openai_preprocessor as gpt


YOYOCLAW_RESPONSE_SCHEMA = "advanced-yoyoclaw-image-response-v1"
SUPPORTED_YOYOCLAW_IMAGE_MODELS = {
    "image-model-library/Doubao-Seedream-5.0-Pro-PC-YOYO",
    "image-model-library/Hy-Image-3.0-PC-YOYO",
    "image-model-library/Qwen-Image-3.0-PC-YOYO",
}


def design_prompt(request: DesignImageRequest) -> str:
    roles = "; ".join(f"image {index + 1}: {role}" for index, (role, _) in enumerate(request.references))
    user_instruction = request.prompt.strip() or DEFAULT_IMAGE_INSTRUCTION
    contract = request.view_contract or VIEW_CAMERA_CONTRACTS[request.view]
    signed_angle = int(contract["signed_yaw_degrees"])
    orbit = contract["top_view_orbit"]
    base_prompt = (gpt.build_geometry_reference_prompt(user_instruction, request.style, request.custom_style)
                   if request.references else
                   gpt.build_text_geometry_reference_prompt(user_instruction, request.style, request.custom_style))
    single_output = (
        " Return exactly ONE standalone design image. Do not output a contact sheet, multiple panels, labels, "
        "watermarks, or duplicate views."
    )

    if not request.initial and request.references:
        return (
            f"Edit the FIRST image, the current {request.view} design. Apply only the requested description and style "
            "changes. Preserve the same subject identity, shape, proportions, pose, camera viewpoint, rotation, "
            "silhouette, framing, crop, and all details not requested to change. An optional target-view upload is a "
            f"consistency reference. Reference roles: {roles}. " + base_prompt + single_output
        )

    target_upload = f"{request.view}_upload"
    if request.initial and request.references and any(role == target_upload for role, _ in request.references):
        return (
            f"Generate the {request.view} design directly from the FIRST image, the uploaded {request.view} view. "
            "Keep the uploaded subject's identity, shape, proportions, pose, camera viewpoint, rotation, silhouette, "
            "framing, crop, visible extent, and main appearance. Do not reinterpret it from another view. "
            f"Reference roles: {roles}. " + base_prompt + single_output
        )

    if request.references:
        return (
            f"Generate the missing {request.view} design from the supplied main reference. Treat the main-reference "
            f"camera as 0 degrees. Looking down from above, move only the camera to signed yaw {signed_angle:+d} degrees "
            f"({orbit}) for the {request.view} view; keep the subject fixed. {contract['camera_result']} "
            "Preserve the shape, proportions, and pose of the person, figurine, pet, building, or other object. Preserve "
            "the original framing and crop where practical. For regions that are not visible in the reference, make a "
            f"reasonable inference consistent with the same subject. Reference roles: {roles}. " + base_prompt +
            " Multiview override: only the source-viewpoint and unseen-region restrictions in the preceding image-to-3D "
            "contract are replaced by the target camera rotation and reasonable hidden-region inference stated above. "
            "All subject isolation, background replacement, geometry, identity, style, lighting, support, crop, and "
            "single-subject rules remain in force." + single_output
        )

    return (
        f"Create the {request.view} design from the description. This image establishes the main-reference camera at "
        f"0 degrees for the four-view set. Keep the subject centered and clearly visible. " + base_prompt + single_output
    )


def _provider_request_id(result: dict[str, Any]) -> str:
    for key in ("requestId", "request_id", "id"):
        request_id = result.get(key)
        if isinstance(request_id, str) and request_id:
            return request_id
    data = result.get("data")
    if isinstance(data, dict):
        nested = _provider_request_id(data)
        if nested:
            return nested
    item = data[0] if isinstance(data, list) and data and isinstance(data[0], dict) else {}
    for key in ("requestId", "request_id", "generation_id", "id"):
        generation_id = item.get(key)
        if isinstance(generation_id, str) and generation_id:
            return generation_id
    return ""


def multipart_references(request: DesignImageRequest, prompt: str, model: str) -> tuple[bytes, str]:
    if not 1 <= len(request.references) <= 5:
        raise ProviderGatewayError("One to five named references are required.", code="invalid_image_request",
                                   category="validation", provider="gpt")
    boundary = "----OrcaAdvanced" + uuid.uuid4().hex
    chunks = []

    def field(name: str, value: str) -> None:
        chunks.extend([f"--{boundary}\r\nContent-Disposition: form-data; name=\"{name}\"\r\n\r\n".encode(),
                       value.encode("utf-8"), b"\r\n"])

    for name, value in {"model": model, "prompt": prompt, "n": "1", "size": "auto", "background": "opaque",
                        "quality": gpt._image_quality(), "input_fidelity": "high"}.items():
        field(name, value)
    for index, (_, path) in enumerate(request.references):
        mime, extension = gpt._image_kind(path)
        if not path.is_file() or path.stat().st_size > MAX_IMAGE_BYTES:
            raise ProviderGatewayError("A reference image is unavailable or oversized.", code="invalid_image_request",
                                       category="validation", provider="gpt")
        chunks.extend([f"--{boundary}\r\nContent-Disposition: form-data; name=\"image[]\"; "
                       f"filename=\"reference-{index}.{extension}\"\r\nContent-Type: {mime}\r\n\r\n".encode(),
                       path.read_bytes(), b"\r\n"])
    chunks.append(f"--{boundary}--\r\n".encode())
    return b"".join(chunks), "multipart/form-data; boundary=" + boundary


class AdvancedMultiviewImageGateway:
    def __init__(self, yoyoclaw: YoYoClawModelProviderGateway | None = None) -> None:
        self.yoyoclaw = yoyoclaw or YoYoClawModelProviderGateway()

    def recover(self, destination: Path) -> DesignImageResult | None:
        """Download/decode a previously obtained response; never create an image."""
        envelope = destination.with_suffix(".response.json")
        if not envelope.exists():
            return DesignImageResult() if destination.is_file() else None
        from advanced_multiview_repository import AdvancedMultiviewRepository
        result = AdvancedMultiviewRepository.read_json(envelope, maximum=32 * 1024 * 1024)
        if result.get("schema") == YOYOCLAW_RESPONSE_SCHEMA:
            response = result.get("response")
            if not isinstance(response, dict):
                raise ProviderGatewayError("The saved YOYOClaw image response is invalid.",
                                           code="invalid_provider_result", category="download",
                                           provider="yoyoclaw")
            if not destination.is_file():
                self.yoyoclaw.download_design_response(response, destination)
            return DesignImageResult(provider_request_id=_provider_request_id(response))
        try:
            if not destination.is_file():
                gpt._save_provider_image(result, destination)
        except gpt.OpenAIPreprocessorError:
            raise ProviderGatewayError("The saved image response is awaiting download.",
                                       code="result_download_failed", category="download", provider="gpt",
                                       retryable=True) from None
        return DesignImageResult(provider_request_id=_provider_request_id(result))

    @staticmethod
    def _yoyoclaw_base(provider: str, model: str) -> dict[str, Any]:
        return {"initial_text": "unknown", "initial_named_references": "unknown",
                "edit_current_with_context": "unknown", "max_reference_images": 0,
                "provider": provider, "model": model, "remote_task_resume": False}

    def _yoyoclaw_capability(self, model: str, catalog: dict[str, Any]) -> dict[str, Any]:
        base = self._yoyoclaw_base("yoyoclaw", model)
        if catalog.get("state") != "ready":
            base["reason"] = str(catalog.get("state") or "provider_unavailable")
            return base
        available = {
            str(item.get("id")) for item in catalog.get("models", [])
            if isinstance(item, dict) and item.get("available", True)
        }
        if model not in available:
            base["reason"] = "model_unavailable" if model else "model_required"
            return base
        if model not in SUPPORTED_YOYOCLAW_IMAGE_MODELS:
            base["reason"] = "unverified_image_model"
            return base
        base.update(initial_text="supported", initial_named_references="supported",
                    edit_current_with_context="supported", max_reference_images=3,
                    verification="official_contract_and_three-reference-provider-probe")
        return base

    def catalog_capabilities(self) -> list[dict[str, Any]]:
        result = [self.capabilities("gpt", "")]
        catalog = self.yoyoclaw.catalog(IMAGE_ROLE)
        models = [str(item.get("id")) for item in catalog.get("models", [])
                  if isinstance(item, dict) and isinstance(item.get("id"), str)]
        if models:
            result.extend(self._yoyoclaw_capability(model, catalog) for model in models)
        else:
            result.append(self._yoyoclaw_capability("", catalog))
        return result

    def capabilities(self, provider: str, model: str) -> dict[str, Any]:
        base = {"initial_text": "unknown", "initial_named_references": "unknown",
                "edit_current_with_context": "unknown", "max_reference_images": 0,
                "provider": provider, "model": model, "remote_task_resume": False}
        if provider == "gpt":
            try:
                config = gpt._image_config()
                actual = config.model
                if model and model != actual:
                    return {**base, "reason": "configured_model_mismatch"}
                base["model"] = actual
                if actual.lower().startswith(("gpt-image-1", "gpt-image-2")):
                    base.update(initial_text="supported", initial_named_references="supported",
                                edit_current_with_context="supported", max_reference_images=5,
                                verification="adapter_contract; real model acceptance must be verified")
                else:
                    base["reason"] = "unverified_image_model"
            except gpt.OpenAIPreprocessorError:
                base["reason"] = "provider_not_configured"
        elif provider == "yoyoclaw":
            return self._yoyoclaw_capability(model, self.yoyoclaw.catalog(IMAGE_ROLE))
        else:
            base["reason"] = "unsupported_provider"
        return base

    def generate(self, request: DesignImageRequest, destination: Path) -> DesignImageResult:
        if request.provider == "yoyoclaw":
            capability = self.capabilities(request.provider, request.model)
            if capability.get("initial_text") != "supported":
                raise ProviderGatewayError("The selected YOYOClaw image model is unavailable.",
                                           code=str(capability.get("reason") or "model_unavailable"),
                                           category="availability", provider="yoyoclaw")
            if len(request.references) > 3:
                raise ProviderGatewayError("YOYOClaw accepts at most three ordered references.",
                                           code="invalid_image_request", category="validation",
                                           provider="yoyoclaw")
            prompt = design_prompt(request)
            response = self.yoyoclaw.generate_design_response(
                request.model, prompt, tuple(path for _, path in request.references))
            envelope = destination.with_suffix(".response.json")
            from advanced_multiview_repository import AdvancedMultiviewRepository
            AdvancedMultiviewRepository.atomic_json(envelope, {
                "schema": YOYOCLAW_RESPONSE_SCHEMA,
                "provider": "yoyoclaw",
                "model": request.model,
                "response": response,
            }, maximum=32 * 1024 * 1024)
            self.yoyoclaw.download_design_response(response, destination)
            return DesignImageResult(provider_request_id=_provider_request_id(response))
        if request.provider != "gpt":
            raise ProviderGatewayError("This image model's named-reference capability is not verified.",
                                       code="unsupported_multiview", category="validation", provider=request.provider)
        config = gpt._image_config()
        if request.model and request.model != config.model:
            raise ProviderGatewayError("The configured image model changed after confirmation.",
                                       code="model_changed", category="validation", provider="gpt")
        prompt = design_prompt(request)
        try:
            if request.references:
                body, content_type = multipart_references(request, prompt, config.model)
                result = gpt._image_provider_request("/images/edits", body, content_type)
            else:
                body = json.dumps({"model": config.model, "prompt": prompt, "n": 1, "size": "1024x1024",
                                   "quality": gpt._image_quality(), "background": "opaque"}, ensure_ascii=False).encode()
                result = gpt._image_provider_request("/images/generations", body, "application/json")
            # Save the response envelope first: a later download failure must reuse this response.
            envelope = destination.with_suffix(".response.json")
            from advanced_multiview_repository import AdvancedMultiviewRepository
            AdvancedMultiviewRepository.atomic_json(envelope, result, maximum=32 * 1024 * 1024)
            gpt._save_provider_image(result, destination)
            return DesignImageResult(provider_request_id=_provider_request_id(result))
        except gpt.OpenAIPreprocessorError as error:
            raise ProviderGatewayError(str(error), code=error.code, category="provider", provider="gpt",
                                       ambiguous=error.ambiguous, retryable=error.retryable) from None
