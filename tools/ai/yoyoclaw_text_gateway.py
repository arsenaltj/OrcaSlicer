from __future__ import annotations

from collections.abc import Callable, Mapping
from contextlib import nullcontext
from typing import Any

from yoyoclaw_client import TextRequestDiagnostics, YoYoClawClient, YoYoClawError


TEXT_LIBRARY = "model-library"
AUTO_MODEL = "auto"
FALLBACK_MODEL_FAMILIES = ("qwen", "glm")


def available_text_models(rows: Any, *, requires_image: bool = False) -> list[dict[str, Any]]:
    candidates: list[dict[str, Any]] = []
    if not isinstance(rows, list):
        return candidates
    for row in rows:
        if not isinstance(row, Mapping) or not isinstance(row.get("name"), str):
            continue
        name = row["name"].strip()
        if not name:
            continue
        availability = row.get("availability", {})
        if isinstance(availability, Mapping) and availability.get("state") not in (None, "available"):
            continue
        if row.get("available") is False or row.get("enabled") is False:
            continue
        if str(row.get("state", "")).lower() in {"disabled", "offline", "maintenance", "unavailable"}:
            continue
        modalities = row.get("modalities", {})
        inputs = {
            _normalized_identifier(value)
            for value in (modalities.get("input", []) if isinstance(modalities, Mapping) else [])
        }
        outputs = {
            _normalized_identifier(value)
            for value in (modalities.get("output", []) if isinstance(modalities, Mapping) else [])
        }
        if "text" not in inputs or "text" not in outputs or (requires_image and "image" not in inputs):
            continue
        candidates.append(dict(row))
    return candidates


def _normalized_identifier(value: Any) -> str:
    return value.strip().casefold() if isinstance(value, str) else ""


def select_preferred_text_model(rows: Any, *, requires_image: bool = False) -> str:
    candidates = available_text_models(rows, requires_image=requires_image)
    for row in candidates:
        if AUTO_MODEL in {
            _normalized_identifier(row["name"]),
            _normalized_identifier(row.get("display_name")),
        }:
            return row["name"]

    for family in FALLBACK_MODEL_FAMILIES:
        for row in candidates:
            identifiers = (
                _normalized_identifier(row["name"]),
                _normalized_identifier(row.get("display_name")),
            )
            if any(identifier.startswith(family) for identifier in identifiers):
                return row["name"]

    raise YoYoClawError(
        "The YOYOClaw text whitelist does not contain an available compatible Auto, Qwen, or GLM model.",
        code="preferred_text_model_unavailable", category="availability",
    )


def select_slicing_text_model(rows: Any, *, requires_image: bool = False) -> str:
    candidates = available_text_models(rows, requires_image=requires_image)
    for row in candidates:
        identifiers = (
            _normalized_identifier(row["name"]),
            _normalized_identifier(row.get("display_name")),
        )
        if any(identifier.startswith("qwen") and "max" in identifier for identifier in identifiers):
            return row["name"]
    for row in candidates:
        if AUTO_MODEL in {
            _normalized_identifier(row["name"]),
            _normalized_identifier(row.get("display_name")),
        }:
            return row["name"]
    raise YoYoClawError(
        "The YOYOClaw text whitelist does not contain an available compatible Qwen Max or Auto model.",
        code="preferred_text_model_unavailable", category="availability",
    )


class YoYoClawTextGateway:
    def __init__(
        self, client_factory: Callable[[], YoYoClawClient] = YoYoClawClient,
        *, model_selector: Callable[..., str] = select_preferred_text_model,
    ) -> None:
        self._client_factory = client_factory
        self._model_selector = model_selector

    def availability(self, *, requires_image: bool = False) -> dict[str, Any]:
        client = self._client_factory()
        try:
            if not client.config.configured:
                return {"available": False, "code": "provider_not_configured"}
            model = self._model_selector(
                client.whitelist_catalog(TEXT_LIBRARY)["models"], requires_image=requires_image
            )
            return {"available": True, "model": model}
        except YoYoClawError as error:
            return {"available": False, "code": error.code}
        finally:
            client.close()

    def complete(
        self, messages: list[dict[str, Any]], *, requires_image: bool = False,
        diagnostics: TextRequestDiagnostics | None = None,
    ) -> str:
        client = self._client_factory()
        try:
            if not client.config.configured:
                raise YoYoClawError(
                    "YOYOClaw credentials are not configured.",
                    code="provider_not_configured", category="configuration",
                )
            if diagnostics is not None:
                with diagnostics.measure("authentication"):
                    if not client.authenticated:
                        client.login()
            with diagnostics.measure("whitelist") if diagnostics is not None else nullcontext():
                rows = client.whitelist_catalog(TEXT_LIBRARY)["models"]
            model = self._model_selector(rows, requires_image=requires_image)
            if diagnostics is not None:
                diagnostics.record("model_selected", requested_model=model)
                return client.text_chat(model, messages, timeout=180.0, diagnostics=diagnostics)
            return client.text_chat(model, messages, timeout=180.0)
        finally:
            client.close()
