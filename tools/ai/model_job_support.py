from __future__ import annotations

from pathlib import Path
from typing import Any

from model_input_image_quality import ModelInputImageQualityError, assess_model_input_image


def image_type(data: bytes) -> str | None:
    if data.startswith(b"\x89PNG\r\n\x1a\n"):
        return "image/png"
    if data.startswith(b"\xff\xd8\xff"):
        return "image/jpeg"
    return None


def file_info(path: Path | None) -> tuple[bool, int]:
    if path is None:
        return False, 0
    try:
        size = path.stat().st_size
    except OSError:
        return False, 0
    return size > 0, size


def stored_image_type(path: Path | None) -> str:
    if path is None:
        return ""
    try:
        with path.open("rb") as stream:
            return image_type(stream.read(16)) or ""
    except OSError:
        return ""


def preprocess_failure_payload(error: Exception) -> dict[str, Any]:
    return {
        "code": str(getattr(error, "code", "image_preprocess_failed")),
        "retryable": getattr(error, "retryable", False) is True,
        "ambiguous": getattr(error, "ambiguous", False) is True,
    }


def generation_prompt(
    prompt: str, palette: tuple[str, ...], *, max_prompt_bytes: int
) -> str:
    suffix = (
        " Generate a watertight printable model with a stable flat base. Preserve meaningful separate parts "
        "and material regions in their original relative positions; do not create unintended floating debris, "
        "internal shells, holes, or non-manifold geometry."
    )
    if palette:
        suffix += " Use only these printable filament colors: " + ", ".join(palette) + "."
    else:
        suffix += " Preserve coherent natural colors with broad, clean material regions."
    max_prefix_bytes = max_prompt_bytes - len(suffix.encode("utf-8"))
    prefix = prompt.strip().encode("utf-8")[: max(0, max_prefix_bytes)].decode("utf-8", errors="ignore").rstrip()
    return prefix + suffix


def assess_job_model_reference(job: Any) -> dict[str, Any]:
    reference = job.model_reference_path or job.preview_path
    if reference is None:
        raise ModelInputImageQualityError("The model reference image is unavailable.")
    quality = assess_model_input_image(reference)
    job.image_metrics["model_input_quality"] = quality
    return quality


def model_input_quality_message(quality: dict[str, Any]) -> str:
    blockers = quality.get("blockers", [])
    primary = str(blockers[0]) if blockers else ""
    return {
        "subject_not_detected": "No clear subject was found; use a clearer image or regenerate the preview.",
        "subject_too_small": "The subject is too small; enlarge it and regenerate the preview.",
        "subject_or_background_fills_frame": "The subject or background fills the frame; regenerate with clear margins.",
        "subject_cropped": "The subject touches the frame and may be cropped; regenerate with the complete silhouette visible.",
        "fragmented_subject": "The reference contains disconnected subjects or fragments; regenerate with one connected subject.",
        "excessive_semitransparency": "The subject contains too much transparency for reliable 3D generation.",
        "background_not_isolated": "The background is too complex; regenerate on a transparent or plain background.",
    }.get(primary, "The preview does not meet the image-to-3D input requirements; regenerate it.")


def printable_preview_message(job: Any, fallback: str) -> str:
    model_input_quality = job.image_metrics.get("model_input_quality", {})
    if isinstance(model_input_quality, dict) and not bool(model_input_quality.get("model_input_eligible", True)):
        return model_input_quality_message(model_input_quality)
    if job.palette and not bool(job.image_metrics.get("palette_quality_ok", True)):
        subject_ratio = float(job.image_metrics.get("printable_subject_area_ratio", 0.0))
        continuity = float(job.image_metrics.get("largest_subject_component_ratio", 0.0))
        if subject_ratio < 0.18:
            return "The printable subject is too small in the preview; regenerate with a larger subject."
        if continuity < 0.90:
            return "The printable subject is disconnected; regenerate with one connected subject."
        return "The printable preview failed its geometry quality check; regenerate the preview."
    return fallback
