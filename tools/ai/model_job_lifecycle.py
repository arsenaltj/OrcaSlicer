"""ModelJobLifecycle: one application capability over explicit runtime ports."""
from __future__ import annotations

import json
import uuid
from ai_diagnostics import event as diagnostic_event
from dataclasses import asdict, dataclass
from model_contracts import (
    DEFAULT_IMAGE_INSTRUCTION,
    DEFAULT_MODEL_FACE_LIMIT,
    JOB_STATE_FILENAME,
    Job,
    JobStopped,
    MAX_ARTIFACT_BYTES,
    MAX_GENERATION_ATTEMPTS,
    MAX_JOB_STATE_BYTES,
    MIN_MODEL_REFERENCE_EDGE,
    MIN_SOURCE_IMAGE_EDGE,
    MODEL_QUALITY_FILENAME,
    RequestError,
    SidecarRestart,
)
from model_creation_policy import _public_design_timing
from model_input_image_quality import ModelInputImageQualityError
from model_job_repository import _copy_job_file, _load_job
from model_job_support import (
    file_info as _file_info,
    legacy_face_error_is_recoverable as _legacy_face_error_is_recoverable,
    mark_prepaid_multiview_retry as _mark_prepaid_multiview_retry,
    model_input_quality_message as _model_input_quality_message,
    preprocess_failure_payload,
    printable_preview_message as _printable_preview_message,
    stale_high_quality_face_gate_is_recoverable as _stale_high_quality_face_gate_is_recoverable,
    stored_image_type as _stored_image_type,
)
from model_refinement import build_model_refinement_advice
from model_request import (
    _generation_options,
    _normalize_face_limit,
    _normalize_palette_color_count,
)
from openai_preprocessor import OpenAIPreprocessorError
from portrait_geometry_reference import (
    _geometry_generation_reference,
    _identity_preserving_portrait_geometry_enabled,
    _model_generation_reference,
)
from printable_image_pipeline import PrintSettings
from printable_visual_quality import REPORT_FILENAME as VISUAL_QUALITY_FILENAME
from typing import Any, Callable, ContextManager, MutableMapping

@dataclass(frozen=True)
class ModelJobLifecyclePorts:
    apply_preview_visual_quality_gate: Callable[..., Any]
    assess_reference_advice: Callable[..., Any]
    generate_job: Callable[..., Any]
    is_shutting_down: Callable[[], bool]
    jobs: MutableMapping[str, Job]
    lock: ContextManager[Any]
    model_output_root: Callable[..., Any]
    persist_job: Callable[..., Any]
    retexture_job: Callable[..., Any]
    submit: Callable[..., Any]
    validate_image_file: Callable[..., Any]


class ModelJobLifecycle:
    def __init__(self, ports: ModelJobLifecyclePorts):
        self.ports = ports

    def new_job(self,
        source: str,
        palette: tuple[str, ...] = (),
        palette_roles: dict[str, str] | None = None,
        style: str = "sculpture",
        custom_style: str = "",
        print_settings: dict[str, Any] | None = None,
        palette_color_count: int | None = None,
        provider: str = "tripo",
        generation_options: dict[str, Any] | None = None,
    ) -> Job:
        options = dict(generation_options or {}, provider=provider)
        face_limit = options.get("face_limit", DEFAULT_MODEL_FACE_LIMIT)
        if isinstance(face_limit, str) and len(face_limit) <= 7 and face_limit.isascii() and face_limit.isdecimal():
            face_limit = int(face_limit)
        face_limit = _normalize_face_limit(face_limit)
        # A design preview has no 3D provider request. Preserve the draft choices;
        # the explicit model confirmation validates their provider-specific combination.
        geometry, texture, output = _generation_options(options, face_limit, validate_provider_constraints=False)
        job_id = str(uuid.uuid4())
        output_root = self.ports.model_output_root()
        directory = output_root / job_id
        try:
            directory.mkdir(parents=True, exist_ok=False)
        except OSError:
            raise RequestError("service_unavailable", "The generated-model directory could not be created.", 503, True) from None
        job = Job(
            id=job_id,
            source=source,
            directory=directory,
            palette=palette,
            palette_roles=dict(palette_roles or {}),
            palette_color_count=_normalize_palette_color_count(
                palette_color_count if palette_color_count is not None else (len(palette) or None)
            ),
            style=style,
            custom_style=custom_style,
            print_settings=print_settings or asdict(PrintSettings()),
            provider=provider,
            face_limit=face_limit,
            generation_profile="quality" if face_limit >= 500000 else "performance",
            geometry_quality=geometry,
            texture_quality=texture,
            output_format=output,
        )
        self.ports.persist_job(job)
        return job

    def reuse_design_job(self, source: Job) -> Job:
        """Fork only saved design inputs. No provider calls, tasks, or model artifacts."""
        if source.state not in {"awaiting_confirmation", "ready", "stopped", "failed", "cancelled"}:
            raise RequestError("invalid_job_state", "The source design is still being generated.", 409)
        reference = _model_generation_reference(source)
        if reference is None:
            raise RequestError("invalid_model_reference", "The saved design image is missing.", 409)
        paths = {}
        for name in ("input_path", "raw_preview_path", "preview_path", "model_reference_path", "geometry_reference_path"):
            path = getattr(source, name)
            if path is None:
                continue
            try:
                resolved = path.resolve(strict=True)
                resolved.relative_to(source.directory.resolve(strict=True))
                if not resolved.is_file():
                    raise ValueError("not a file")
                self.ports.validate_image_file(resolved, minimum_edge=64)
            except (ValueError, OSError):
                raise RequestError("invalid_model_reference", "A saved design file is missing or invalid.", 409) from None
            paths[name] = resolved
        if not any(name in paths for name in ("raw_preview_path", "preview_path", "model_reference_path")):
            raise RequestError("invalid_model_reference", "The saved design image is missing.", 409)
        child = self.new_job(source.source, palette=source.palette, palette_roles=source.palette_roles,
                         palette_color_count=source.palette_color_count, style=source.style,
                         custom_style=source.custom_style, print_settings=dict(source.print_settings),
                         provider=source.provider, generation_options={
                             "face_limit": source.face_limit, "geometry_quality": source.geometry_quality or "standard",
                             "texture_quality": source.texture_quality, "output_format": source.output_format})
        child.source_design_job_id = source.source_design_job_id or source.id
        child.user_prompt = source.user_prompt
        child.prepared_prompt = source.prepared_prompt
        child.preview_content_type = source.preview_content_type
        for name, path in paths.items():
            setattr(child, name, _copy_job_file(path, child, name.removesuffix("_path")))
        child.state = child.phase = "awaiting_confirmation"
        child.message = "Saved design is ready for explicit model generation confirmation."
        child.progress = 15
        self.ports.persist_job(child, required=True)
        return child

    def restore_jobs(self, *, resume_jobs: bool = True) -> list[Job]:
        output_root = self.ports.model_output_root()
        try:
            directories = [path for path in output_root.iterdir() if path.is_dir()]
        except OSError:
            return []
        restored: list[Job] = []
        for directory in directories:
            job = _load_job(directory)
            if job is None:
                continue
            if job.input_path is not None:
                try:
                    self.ports.validate_image_file(job.input_path, minimum_edge=MIN_SOURCE_IMAGE_EDGE)
                except ValueError:
                    job.input_path = None
            for attribute in (
                "raw_preview_path", "strict_preview_path", "preview_path", "model_reference_path",
                "geometry_reference_path",
            ):
                path = getattr(job, attribute)
                if path is None:
                    continue
                try:
                    self.ports.validate_image_file(
                        path,
                        minimum_edge=MIN_MODEL_REFERENCE_EDGE,
                        require_visual_detail=True,
                    )
                except ValueError:
                    setattr(job, attribute, None)
            if job.state == "awaiting_confirmation" and (job.model_reference_path or job.preview_path) is not None:
                try:
                    quality, generation_quality = self.ports.assess_reference_advice(job)
                    cached_visual_quality = job.image_metrics.get("preview_visual_quality")
                    if isinstance(cached_visual_quality, dict):
                        self.ports.apply_preview_visual_quality_gate(job, cached_visual_quality)
                        quality = job.image_metrics.get("model_input_quality", quality)
                        generation_quality = job.image_metrics.get(
                            "generation_input_quality", generation_quality
                        )
                    if not bool(generation_quality.get("model_input_eligible", False)):
                        job.message = _model_input_quality_message(generation_quality)
                    elif not bool(quality.get("model_input_eligible", False)):
                        job.message = _model_input_quality_message(quality)
                    elif job.phase == "awaiting_confirmation":
                        job.message = _printable_preview_message(job, "Review the prepared image before generation.")
                except ModelInputImageQualityError:
                    job.state = "failed"
                    job.phase = "failed"
                    job.message = "The saved image preview could not be checked. Generate the preview again."
                    job.progress = 0
            if (
                job.source == "image"
                and job.state in {"recommending_palette", "preprocessing", "awaiting_palette_confirmation", "awaiting_confirmation"}
                and job.input_path is None
            ):
                job.state = "failed"
                job.phase = "failed"
                job.message = "The saved reference image is missing or damaged. Select the image again."
                job.progress = 0
            elif job.source == "image" and job.state == "awaiting_confirmation" and job.preview_path is None:
                job.state = "failed"
                job.phase = "failed"
                job.message = "The saved image preview is missing or damaged. Generate the preview again."
                job.progress = 0
            latest_attempt = job.attempts[-1] if job.attempts else {}
            has_paid_model_task = any(
                isinstance(attempt.get("generation_task_id"), str) and bool(attempt.get("generation_task_id"))
                for attempt in job.attempts
            )
            recoverable_multiview_failure = (
                job.state == "failed"
                and not has_paid_model_task
                and not job.attempts
                and job.source == "image"
                and job.style == "realistic"
                and job.generation_profile == "quality"
                and not _identity_preserving_portrait_geometry_enabled(job)
                and job.model_reference_path is not None
                and job.progress >= 10
            )
            if recoverable_multiview_failure:
                _mark_prepaid_multiview_retry(job, (
                    "Four-view portrait preparation stopped before any paid Tripo task was created. "
                    "The approved preview is preserved and can be retried."
                ), "legacy_prepaid_multiview_failure")
            if self.can_manually_retry_hunyuan(job):
                job.state = "awaiting_confirmation"
                job.phase = "model_retry"
                job.message = "The previous Hunyuan submission was rejected. Review the saved design and explicitly confirm a new paid submission."
                job.progress = 15
            recoverable_error = str(latest_attempt.get("error", "")).lower()
            can_retry_download = (
                job.state == "failed"
                and isinstance(latest_attempt.get("generation_task_id"), str)
                and bool(latest_attempt.get("generation_task_id"))
                and (
                    (isinstance(latest_attempt.get("conversion_task_id"), str)
                     and bool(latest_attempt.get("conversion_task_id")))
                    or (job.provider == "tripo" and job.output_format == "glb"
                        and not latest_attempt.get("conversion_submission_started"))
                )
                and (any(marker in recoverable_error for marker in (
                    "unsafe artifact location",
                    "invalid obj package",
                    "artifact host could not be resolved",
                    "artifact could not be downloaded",
                    "temporarily unavailable",
                    "rate limiting",
                    "deadline expired",
                )) or _legacy_face_error_is_recoverable(recoverable_error, job.face_limit)
                    or _stale_high_quality_face_gate_is_recoverable(recoverable_error, job.face_limit))
            )
            if can_retry_download:
                job.state = "queued"
                job.phase = "resuming"
                job.message = "Retrying the existing remote artifact download after restart."
                job.progress = max(75, job.progress)
            if (job.provider == "hunyuan" and job.state == "failed" and has_paid_model_task
                    and latest_attempt.get("provider_error_retryable") is True
                    and latest_attempt.get("provider_error_ambiguous") is not True):
                job.state = "queued"
                job.phase = "resuming"
                job.message = "Resuming the existing Hunyuan task after a temporary query or download failure."
            if job.state in {"preprocessing", "recommending_palette"}:
                job.state = "failed"
                job.phase = "failed"
                job.message = "The sidecar restarted during a local AI step. Start that step again manually."
                job.progress = 0
            # ``stopping`` is written only for an explicit user stop.  Treat it as
            # durable intent: after a crash or forced app exit, never resurrect the
            # already-paid task and surprise the user with more local processing.
            if job.state == "stopping":
                job.state = "stopped"
                job.phase = "stopped"
                job.message = "Model generation stopped."
                job.progress = 0
                self.clear_job_artifact(job)
            elif job.state in {"queued", "running"}:
                generation_id = next(
                    (attempt.get("generation_task_id") for attempt in reversed(job.attempts)
                     if isinstance(attempt.get("generation_task_id"), str) and attempt.get("generation_task_id")),
                    "",
                )
                if generation_id:
                    job.state = "queued"
                    job.phase = "resuming"
                    job.message = "Resuming the existing paid model task after restart."
                    job.progress = max(20, job.progress)
                else:
                    job.state = "failed"
                    job.phase = "failed"
                    job.message = "The sidecar restarted before the paid task reference was saved. Start a new generation manually."
                    job.progress = 0
            restored.append(job)
        with self.ports.lock:
            for job in restored:
                self.ports.jobs[job.id] = job
                self.ports.persist_job(job, touch=False)
        if resume_jobs:
            self.resume_restored_jobs(restored)
        return restored

    def resume_restored_jobs(self, restored: list[Job]) -> None:
        for job in restored:
            if job.state == "queued" and job.phase == "resuming":
                latest_attempt = job.attempts[-1] if job.attempts else {}
                if latest_attempt.get("provider_operation") == "model_texture":
                    source_job_id = str(latest_attempt.get("source_job_id", ""))
                    source_task_id = str(latest_attempt.get("source_task_id", ""))
                    if source_job_id and source_task_id:
                        self.ports.submit(job, self.ports.retexture_job, source_job_id, source_task_id, True)
                    else:
                        self.fail_job(job, "The preserved geometry reference is unavailable; start a new task manually.")
                else:
                    self.ports.submit(job, self.ports.generate_job, job.prepared_prompt, True)

    def adopt_legacy_completed_job(self, job_id: str) -> Job | None:
        """Register a pre-manifest model library entry without accepting an arbitrary path."""
        try:
            if str(uuid.UUID(job_id)) != job_id.lower():
                return None
        except ValueError:
            return None
        output_root = self.ports.model_output_root().resolve()
        try:
            directory = (output_root / job_id).resolve(strict=True)
            directory.relative_to(output_root)
            if not directory.is_dir():
                return None
            artifact = (directory / "model-vertex-color.obj").resolve(strict=True)
            artifact.relative_to(directory)
            artifact_size = artifact.stat().st_size
            if not artifact.is_file() or artifact_size <= 0 or artifact_size > MAX_ARTIFACT_BYTES:
                return None
        except (OSError, ValueError):
            return None

        job = Job(id=job_id, source="image", directory=directory)
        job.state = "ready"
        job.phase = "ready"
        job.message = "Recovered historical model library entry."
        job.progress = 100
        job.artifact_path = artifact
        job.artifact_format = "obj"  # This adoption path is for legacy OBJ jobs.
        attempts_path = directory / "attempts.json"
        try:
            if attempts_path.is_file() and attempts_path.stat().st_size <= MAX_JOB_STATE_BYTES:
                attempts_payload = json.loads(attempts_path.read_text(encoding="utf-8"))
                attempts = attempts_payload.get("attempts", []) if isinstance(attempts_payload, dict) else []
                if isinstance(attempts, list) and all(isinstance(attempt, dict) for attempt in attempts):
                    job.attempts = attempts
        except (OSError, UnicodeError, json.JSONDecodeError):
            pass
        preview = directory / "preview.png"
        try:
            resolved_preview = preview.resolve(strict=True)
            resolved_preview.relative_to(directory)
            if resolved_preview.is_file() and resolved_preview.stat().st_size > 0:
                job.preview_path = resolved_preview
                job.preview_content_type = "image/png"
        except (OSError, ValueError):
            pass
        try:
            job.updated_at = artifact.stat().st_mtime
        except OSError:
            pass

        with self.ports.lock:
            existing = self.ports.jobs.get(job_id)
            if existing is not None:
                return existing
            self.ports.jobs[job_id] = job
        return job

    def latest_job_is_restorable(self, job: Job) -> bool:
        """Return whether the native UI can reconstruct a useful journey state.

        Prepared-only text fixtures and abandoned internal jobs have no user input
        that the panel can display.  Letting one of those win `/latest` clears a
        newer user's recoverable image journey after restart.
        """

        if job.state not in {
            "recommending_palette", "awaiting_palette_confirmation", "preprocessing",
            "awaiting_confirmation", "queued", "running", "stopping", "ready",
        }:
            return False
        if job.state == "ready":
            return _file_info(job.artifact_path)[0]
        if job.source == "image":
            return _file_info(job.input_path)[0]
        return bool(job.user_prompt.strip())

    def read_job_report(self, job: Job, filename: str) -> dict[str, Any]:
        path = job.directory / filename
        try:
            if path.is_file() and path.stat().st_size <= MAX_JOB_STATE_BYTES:
                candidate = json.loads(path.read_text(encoding="utf-8"))
                if isinstance(candidate, dict):
                    return candidate
        except (OSError, UnicodeError, json.JSONDecodeError):
            pass
        return {}

    def public_job(self, job: Job) -> dict[str, Any]:
        input_ready, input_size = _file_info(job.input_path)
        preview_ready, preview_size = _file_info(job.preview_path)
        raw_preview_ready, raw_preview_size = _file_info(job.raw_preview_path)
        strict_preview_ready, strict_preview_size = _file_info(job.strict_preview_path)
        submitted_reference = _geometry_generation_reference(job)
        model_reference_ready, model_reference_size = _file_info(submitted_reference)
        geometry_reference_ready, geometry_reference_size = _file_info(job.geometry_reference_path)
        heatmap_ready, heatmap_size = _file_info(job.heatmap_path)
        metadata_ready, metadata_size = _file_info(job.metadata_path)
        artifact_ready, artifact_size = _file_info(job.artifact_path)
        color_intent_ready, color_intent_size = _file_info(job.color_intent_path)
        model_quality = self.read_job_report(job, MODEL_QUALITY_FILENAME)
        visual_quality = self.read_job_report(job, VISUAL_QUALITY_FILENAME)
        refinement = build_model_refinement_advice(model_quality, visual_quality)
        model_view_sheet_ready, model_view_sheet_size = _file_info(job.directory / "model-view-sheet.png")
        artifact_filename = ""
        if artifact_ready:
            artifact_filename = f"orcaslicer-model-{job.id}.{job.artifact_format}"
        provider_failure: dict[str, Any] = {}
        latest_attempt = job.attempts[-1] if job.attempts else {}
        provider_attempt = next(
            (
                attempt for attempt in reversed(job.attempts)
                if isinstance(attempt.get("generation_task_id"), str)
                and bool(attempt.get("generation_task_id"))
            ),
            {},
        )
        provider_tasks = {
            "provider": job.provider,
            "generation_task_id": str(provider_attempt.get("generation_task_id", "")),
            "conversion_task_id": str(provider_attempt.get("conversion_task_id", "")),
        } if provider_attempt else {}
        code = latest_attempt.get("provider_error_code")
        if isinstance(code, str) and code:
            category = latest_attempt.get("provider_error_category")
            provider_failure = {
                "code": code,
                "category": category if isinstance(category, str) else "",
                "retryable": latest_attempt.get("provider_error_retryable") is True,
                "ambiguous": latest_attempt.get("provider_error_ambiguous") is True,
            }
        elif job.preprocess_failure:
            provider_failure = {
                "code": str(job.preprocess_failure.get("code", "")),
                "category": "image_preprocessing",
                "retryable": job.preprocess_failure.get("retryable") is True,
                "ambiguous": job.preprocess_failure.get("ambiguous") is True,
            }
        return {
            "id": job.id,
            "source": job.source,
            "style": job.style,
            "custom_style": job.custom_style,
            "face_limit": job.face_limit,
            "generation_profile": job.generation_profile,
            "geometry_quality": job.geometry_quality,
            "texture_quality": job.texture_quality,
            "output_format": job.output_format,
            "provider": job.provider,
            "state": job.state,
            "phase": job.phase,
            "message": job.message,
            "progress": job.progress,
            "attempt": len(job.attempts),
            "max_attempts": MAX_GENERATION_ATTEMPTS,
            "prepared_prompt": job.prepared_prompt if job.source == "text" else "",
            "user_prompt": "" if job.source == "image" and job.user_prompt == DEFAULT_IMAGE_INSTRUCTION else job.user_prompt,
            "palette": list(job.palette),
            "palette_roles": job.palette_roles,
            "palette_color_count": job.palette_color_count,
            "palette_recommendation": job.palette_recommendation,
            "palette_recommendation_confirmed": job.palette_recommendation_confirmed,
            "print": job.print_settings,
            "image_metrics": job.image_metrics,
            "model_quality": model_quality,
            "visual_quality": visual_quality,
            "refinement": refinement,
            "provider_failure": provider_failure,
            "provider_tasks": provider_tasks,
            "model_views": {
                "ready": model_view_sheet_ready,
                "size_bytes": model_view_sheet_size if model_view_sheet_ready else 0,
            },
            "updated_at": job.updated_at,
            "design_timing": _public_design_timing(job),
            "input": {
                "ready": input_ready,
                "content_type": _stored_image_type(job.input_path) if input_ready else "",
                "size_bytes": input_size if input_ready else 0,
            },
            "preview": {
                "ready": preview_ready,
                "content_type": job.preview_content_type if preview_ready else "",
                "size_bytes": preview_size if preview_ready else 0,
            },
            "image_outputs": {
                "raw_preview": {"ready": raw_preview_ready, "size_bytes": raw_preview_size},
                "strict_preview": {"ready": strict_preview_ready, "size_bytes": strict_preview_size},
                "clean_preview": {"ready": preview_ready, "size_bytes": preview_size},
                "model_reference": {"ready": model_reference_ready, "size_bytes": model_reference_size},
                "geometry_reference": {
                    "ready": geometry_reference_ready,
                    "size_bytes": geometry_reference_size,
                },
                "heatmap": {"ready": heatmap_ready, "size_bytes": heatmap_size},
                "metadata": {"ready": metadata_ready, "size_bytes": metadata_size},
                "masks": sorted(job.mask_paths),
            },
            "artifact": {
                "ready": artifact_ready,
                "format": job.artifact_format if artifact_ready else "",
                "color_encoding": ("textures_or_vertex_colors" if job.artifact_format == "glb" else "vertex_colors") if artifact_ready else "",
                "filename": artifact_filename,
                "size_bytes": artifact_size if artifact_ready else 0,
                "color_intent": {
                    "ready": color_intent_ready,
                    "schema": job.color_intent_schema if color_intent_ready else "",
                    "sha256": job.color_intent_sha256 if color_intent_ready else "",
                    "filename": f"orcaslicer-color-intent-{job.id}.json" if color_intent_ready else "",
                    "size_bytes": color_intent_size if color_intent_ready else 0,
                },
            },
        }

    def cleanup_job(self, job: Job) -> None:
        # Job removal only releases in-memory state. Generated inputs, previews, and
        # model resources are user artifacts and remain available on disk.
        return

    def remove_job_state(self, job: Job) -> None:
        for name in (JOB_STATE_FILENAME, f"{JOB_STATE_FILENAME}.part"):
            try:
                (job.directory / name).unlink(missing_ok=True)
            except OSError:
                pass

    def finish_deleted(self, job: Job) -> None:
        with self.ports.lock:
            if job.delete_requested:
                self.ports.jobs.pop(job.id, None)
                self.remove_job_state(job)
            else:
                self.ports.persist_job(job)

    def clear_job_artifact(self, job: Job) -> None:
        job.artifact_path, job.artifact_format = None, ""
        job.color_intent_path, job.color_intent_schema, job.color_intent_sha256 = None, "", ""

    def mark_stopped(self, job: Job) -> None:
        with self.ports.lock:
            job.state = "stopped"
            job.phase = "stopped"
            job.message = "Model generation stopped."
            job.progress = 0
            self.clear_job_artifact(job)
            self.ports.persist_job(job)

    def stop_boundary(self, job: Job) -> None:
        if job.stop_event.is_set():
            if self.ports.is_shutting_down():
                raise SidecarRestart()
            self.mark_stopped(job)
            raise JobStopped()

    def fail_job(self, job: Job, message: str) -> None:
        diagnostic_event("job.failed", level="ERROR", phase=job.phase, failure_message=message)
        with self.ports.lock:
            if job.stop_event.is_set():
                job.state = "stopped"
                job.phase = "stopped"
                job.message = "Model generation stopped."
                job.progress = 0
            else:
                job.state = "failed"
                job.phase = "failed"
                job.message = message
            self.clear_job_artifact(job)
            self.ports.persist_job(job)

    def return_to_portrait_multiview_retry(self, job: Job, message: str) -> None:
        """Keep the approved preview retryable when local four-view checks fail."""

        with self.ports.lock:
            if job.stop_event.is_set():
                job.state = "stopped"
                job.phase = "stopped"
                job.message = "Model generation stopped."
                job.progress = 0
            else:
                _mark_prepaid_multiview_retry(job, message, message)
            self.clear_job_artifact(job)
            self.ports.persist_job(job)

    def fail_preprocess_job(self, job: Job, error: OpenAIPreprocessorError) -> None:
        job.preprocess_failure = preprocess_failure_payload(error)
        self.fail_job(job, str(error))

    def can_manually_retry_hunyuan(self, job: Job) -> bool:
        """A confirmed rejection may be retried only by a new user confirmation."""
        return (
            job.provider == "hunyuan" and (job.state == "failed" or
                (job.state == "awaiting_confirmation" and job.phase == "model_retry")) and bool(job.attempts)
            and all(attempt.get("status") == "rejected"
                    and attempt.get("provider") == "hunyuan"
                    and attempt.get("provider_error_category") == "validation"
                    and attempt.get("provider_error_ambiguous") is False
                    and not attempt.get("generation_task_id")
                    and not attempt.get("conversion_task_id") for attempt in job.attempts)
            and _model_generation_reference(job) is not None
        )

    def latest_generation_task_id(self, job: Job) -> str:
        """Return the newest persisted provider task without creating a new one."""
        for attempt in reversed(job.attempts):
            task_id = attempt.get("generation_task_id")
            if isinstance(task_id, str) and task_id:
                return task_id
        return ""

    def can_retry_unsubmitted_model(self, job: Job) -> bool:
        """Allow an explicit retry only when no paid provider task was recorded."""
        if job.state != "failed" or not job.attempts or self.latest_generation_task_id(job):
            return False
        latest = job.attempts[-1]
        return latest.get("provider_error_ambiguous") is not True
