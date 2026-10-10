"""Durable job serialization and safe relative artifact paths."""
from __future__ import annotations

import json
import os
import shutil
import time
from color_intent import ColorIntentError, verify_color_intent_manifest_file
from model_contracts import (
    DEFAULT_IMAGE_INSTRUCTION,
    DEFAULT_MODEL_FACE_LIMIT,
    JOB_STATE_FILENAME,
    JOB_STATE_VERSION,
    Job,
    MAX_JOB_STATE_BYTES,
    RequestError,
)
from model_job_support import (
    apply_legacy_material_fragmentation_gate as _apply_legacy_material_fragmentation_gate,
)
from model_request import (
    _generation_options,
    _generation_provider,
    _normalize_custom_style,
    _normalize_face_limit,
    _normalize_generation_profile,
    _normalize_palette,
    _normalize_palette_color_count,
    _normalize_palette_recommendation,
    _normalize_palette_roles,
    _normalize_print_settings,
    _normalize_style,
)
from pathlib import Path
from tripo_client import TripoError
from typing import Any

def _job_path_value(job: Job, path: Path | None) -> str:
    if path is None:
        return ""
    try:
        return path.resolve().relative_to(job.directory.resolve()).as_posix()
    except (OSError, ValueError):
        return ""

def _job_file(job: Job, value: Any) -> Path | None:
    if not isinstance(value, str) or not value:
        return None
    candidate = (job.directory / value).resolve()
    try:
        candidate.relative_to(job.directory.resolve())
    except ValueError:
        return None
    return candidate

def _copy_job_file(source: Path | None, job: Job, name: str) -> Path | None:
    if source is None:
        return None
    try:
        resolved = source.resolve(strict=True)
        if not resolved.is_file():
            return None
        suffix = resolved.suffix.lower()
        destination = job.directory / f"{name}{suffix}"
        shutil.copy2(resolved, destination)
        return destination
    except OSError:
        raise RequestError(
            "service_unavailable",
            "The reference files could not be copied for texture generation.",
            503,
            True,
        ) from None

def _persist_job(job: Job, *, touch: bool = True, required: bool = False) -> None:
    if touch:
        job.updated_at = time.time()
    payload = {
        "version": JOB_STATE_VERSION,
        "id": job.id,
        "source": job.source,
        "state": job.state,
        "phase": job.phase,
        "message": job.message,
        "progress": job.progress,
        "palette": list(job.palette),
        "palette_roles": job.palette_roles,
        "palette_color_count": job.palette_color_count,
        "print_settings": job.print_settings,
        "style": job.style,
        "custom_style": job.custom_style,
        "face_limit": job.face_limit,
        "generation_profile": job.generation_profile,
        "geometry_quality": job.geometry_quality,
        "texture_quality": job.texture_quality,
        "output_format": job.output_format,
        "provider": job.provider,
        "user_prompt": "" if job.source == "image" and job.user_prompt == DEFAULT_IMAGE_INSTRUCTION else job.user_prompt,
        "prepared_prompt": job.prepared_prompt,
        "source_design_job_id": job.source_design_job_id,
        "input_path": _job_path_value(job, job.input_path),
        "raw_preview_path": _job_path_value(job, job.raw_preview_path),
        "strict_preview_path": _job_path_value(job, job.strict_preview_path),
        "preview_path": _job_path_value(job, job.preview_path),
        "model_reference_path": _job_path_value(job, job.model_reference_path),
        "geometry_reference_path": _job_path_value(job, job.geometry_reference_path),
        "preview_content_type": job.preview_content_type,
        "heatmap_path": _job_path_value(job, job.heatmap_path),
        "metadata_path": _job_path_value(job, job.metadata_path),
        "background_mask_path": _job_path_value(job, job.background_mask_path),
        "subject_mask_path": _job_path_value(job, job.subject_mask_path),
        "mask_paths": {key: _job_path_value(job, path) for key, path in job.mask_paths.items()},
        "image_metrics": job.image_metrics,
        "preprocess_failure": job.preprocess_failure,
        "artifact_path": _job_path_value(job, job.artifact_path),
        "artifact_format": job.artifact_format,
        "color_intent_path": _job_path_value(job, job.color_intent_path),
        "color_intent_schema": job.color_intent_schema,
        "color_intent_sha256": job.color_intent_sha256,
        "palette_recommendation": job.palette_recommendation,
        "palette_recommendation_confirmed": job.palette_recommendation_confirmed,
        "generate_image": job.generate_image,
        "attempts": job.attempts,
        "updated_at": job.updated_at,
    }
    temporary = job.directory / f"{JOB_STATE_FILENAME}.part"
    destination = job.directory / JOB_STATE_FILENAME
    try:
        encoded = json.dumps(payload, ensure_ascii=False, indent=2)
        if len(encoded.encode("utf-8")) > MAX_JOB_STATE_BYTES:
            raise OSError("job state exceeds its size limit")
        temporary.write_text(encoded, encoding="utf-8")
        os.replace(temporary, destination)
    except OSError:
        try:
            temporary.unlink(missing_ok=True)
        except OSError:
            pass
        if required:
            raise TripoError("The job state could not be saved.") from None

def _load_job(directory: Path) -> Job | None:
    state_path = directory / JOB_STATE_FILENAME
    try:
        if not state_path.is_file() or state_path.stat().st_size > MAX_JOB_STATE_BYTES:
            return None
        payload = json.loads(state_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError):
        return None
    if not isinstance(payload, dict) or payload.get("version") != JOB_STATE_VERSION:
        return None
    job_id = payload.get("id")
    source = payload.get("source")
    if not isinstance(job_id, str) or directory.name != job_id or source not in {"text", "image"}:
        return None
    try:
        palette = _normalize_palette(payload.get("palette", []))
        palette_roles = _normalize_palette_roles(payload.get("palette_roles"), palette)
        palette_color_count = _normalize_palette_color_count(payload.get("palette_color_count"))
        style = _normalize_style(payload.get("style"))
        custom_style = _normalize_custom_style(payload.get("custom_style"), style)
        face_limit = _normalize_face_limit(payload.get("face_limit", DEFAULT_MODEL_FACE_LIMIT))
        # Drafts with an incompatible model choice must remain recoverable so the
        # user can correct that choice before submitting any paid model task.
        geometry_quality, texture_quality, output_format = _generation_options(
            payload, face_limit, validate_provider_constraints=False)
        raw_generation_profile = payload.get("generation_profile")
        generation_profile = _normalize_generation_profile(raw_generation_profile) if raw_generation_profile is not None else \
            ("quality" if face_limit >= 500000 else "performance")
        print_settings = _normalize_print_settings(payload.get("print_settings"))
        palette_recommendation = _normalize_palette_recommendation(
            payload.get("palette_recommendation"), palette_color_count
        )
    except RequestError:
        return None
    attempts = payload.get("attempts", [])
    if not isinstance(attempts, list) or any(not isinstance(attempt, dict) for attempt in attempts):
        return None
    job = Job(
        id=job_id,
        source=source,
        directory=directory,
        palette=palette,
        palette_roles=palette_roles,
        palette_color_count=palette_color_count,
        style=style,
        custom_style=custom_style,
        face_limit=face_limit,
        generation_profile=generation_profile,
        geometry_quality=geometry_quality,
        texture_quality=texture_quality,
        output_format=output_format,
        provider=_generation_provider(payload),
        print_settings=print_settings,
    )
    job.source_design_job_id = str(payload.get("source_design_job_id", ""))
    job.state = str(payload.get("state", "failed"))
    job.phase = str(payload.get("phase", job.state))
    job.message = str(payload.get("message", "Recovered model job."))
    job.progress = max(0, min(int(payload.get("progress", 0)), 100))
    job.user_prompt = str(payload.get("user_prompt", ""))
    job.prepared_prompt = str(payload.get("prepared_prompt", ""))
    job.generate_image = payload.get("generate_image") is True
    job.input_path = _job_file(job, payload.get("input_path"))
    job.raw_preview_path = _job_file(job, payload.get("raw_preview_path"))
    job.strict_preview_path = _job_file(job, payload.get("strict_preview_path"))
    job.preview_path = _job_file(job, payload.get("preview_path"))
    job.model_reference_path = _job_file(job, payload.get("model_reference_path"))
    job.geometry_reference_path = _job_file(job, payload.get("geometry_reference_path"))
    job.preview_content_type = str(payload.get("preview_content_type", ""))
    job.heatmap_path = _job_file(job, payload.get("heatmap_path"))
    job.metadata_path = _job_file(job, payload.get("metadata_path"))
    job.background_mask_path = _job_file(job, payload.get("background_mask_path"))
    job.subject_mask_path = _job_file(job, payload.get("subject_mask_path"))
    raw_masks = payload.get("mask_paths", {})
    if isinstance(raw_masks, dict):
        job.mask_paths = {
            str(key): path for key, value in raw_masks.items()
            if (path := _job_file(job, value)) is not None
        }
    raw_metrics = payload.get("image_metrics", {})
    job.image_metrics = raw_metrics if isinstance(raw_metrics, dict) else {}
    _apply_legacy_material_fragmentation_gate(job)
    raw_preprocess_failure = payload.get("preprocess_failure", {})
    job.preprocess_failure = raw_preprocess_failure if isinstance(raw_preprocess_failure, dict) else {}
    job.artifact_path = _job_file(job, payload.get("artifact_path"))
    job.artifact_format = str(payload.get("artifact_format", ""))
    job.color_intent_path = _job_file(job, payload.get("color_intent_path"))
    job.color_intent_schema = str(payload.get("color_intent_schema", ""))
    job.color_intent_sha256 = str(payload.get("color_intent_sha256", ""))
    if job.color_intent_path is not None:
        try:
            verified = verify_color_intent_manifest_file(
                job.color_intent_path, job.artifact_path,
                expected_schema=job.color_intent_schema, expected_sha256=job.color_intent_sha256,
            )
            job.color_intent_schema, job.color_intent_sha256 = verified.schema, verified.sha256
        except ColorIntentError:
            job.color_intent_path, job.color_intent_schema, job.color_intent_sha256 = None, "", ""
    else:
        job.color_intent_schema, job.color_intent_sha256 = "", ""
    job.palette_recommendation = palette_recommendation
    job.palette_recommendation_confirmed = bool(payload.get("palette_recommendation_confirmed", False))
    job.attempts = attempts
    try:
        job.updated_at = float(payload.get("updated_at", state_path.stat().st_mtime))
    except (TypeError, ValueError, OSError):
        job.updated_at = time.time()
    return job
