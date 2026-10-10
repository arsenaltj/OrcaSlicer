"""Model job commands over explicit ports. HTTP, widgets and process globals stay outside."""
from __future__ import annotations

import json
import uuid
from dataclasses import dataclass
from model_contracts import (
    ApplicationResult,
    DEFAULT_MODEL_FACE_LIMIT,
    GENERATION_PROFILE_FACE_LIMITS,
    Job,
    MAX_IMAGE_BYTES,
    MAX_PROMPT_BYTES,
    MIN_MODEL_REFERENCE_EDGE,
    MIN_SOURCE_IMAGE_EDGE,
    MODEL_QUALITY_FILENAME,
    RequestError,
)
from model_creation_policy import _use_unrestricted_creation
from model_input_image_quality import ModelInputImageQualityError, recommend_printable_style
from model_job_repository import _copy_job_file
from model_job_support import file_info as _file_info, image_type as _image_type
from model_provider_gateway import PaidTaskAuthorization
from model_request import (
    _boolean_field,
    _generation_options,
    _generation_provider,
    _normalize_custom_style,
    _normalize_face_limit,
    _normalize_generation_profile,
    _normalize_image_instruction,
    _normalize_print_settings,
    _normalize_style,
    _text_field,
    _user_image_instruction,
    _validate_image_data,
)
from pathlib import Path
from portrait_geometry_reference import _model_generation_reference
from printable_model_quality import ModelQualityError
from tripo_client import TripoError
from typing import Any, Callable, ContextManager, MutableMapping

@dataclass(frozen=True)
class ModelJobApplicationPorts:
    jobs: MutableMapping[str, Job]
    lock: ContextManager[Any]
    new_job: Callable[..., Any]
    persist: Callable[..., None]
    cleanup: Callable[..., Any]
    submit: Callable[..., None]
    preprocess_text: Callable[..., Any]
    preprocess_image: Callable[..., Any]
    present: Callable[[Job], dict[str, Any]]
    fallback_enabled: Callable[[], bool]
    image_provider_status: Callable[[], dict[str, Any]]
    can_retry_hunyuan: Callable[..., Any]
    can_retry_unsubmitted: Callable[..., Any]
    clear_artifact: Callable[..., Any]
    latest_task_id: Callable[..., Any]
    assess_reference: Callable[..., Any]
    gateway: Callable[..., Any]
    generate: Callable[..., Any]
    retexture: Callable[..., Any]
    adopt_legacy: Callable[..., Any]
    reuse_design: Callable[..., Any]
    record_attempt: Callable[..., Any]
    remove_state: Callable[..., Any]
    analysis_artifact: Callable[..., Any]
    get_job: Callable[[str], Job | None]
    validate_image: Callable[..., Any]
    analyze_model: Callable[..., Any]
    write_quality: Callable[..., None]
    review_model: Callable[..., Any]


class ModelJobApplication:
    def __init__(self, ports: ModelJobApplicationPorts):
        self.ports = ports

    def require_design_source(self, source: str) -> None:
        available = self.ports.image_provider_status()["available"]
        if not available and (source != "text" or not self.ports.fallback_enabled()):
            raise RequestError("feature_unavailable", "AI style preview generation is not configured.", 503)

    def execute_job(self, command: str, job_id: str, request: dict[str, Any]) -> ApplicationResult:
        """Compose only named commands; callers cannot execute arbitrary entrypoints."""
        commands = {
            "generate": self.generate,
            "retexture": self.retexture,
            "stop": self.stop,
            "recheck": self.recheck,
            "visual-review": self.visual_review,
            "confirm-palette": self.confirm_palette,
            "generation-options": self.set_generation_options,
            "reuse-design": self.reuse_design,
        }
        handler = commands.get(command)
        if handler is None:
            raise RequestError("not_found", "Model job command not found.", 404)
        return handler(job_id, request)

    def create_text_job(self, request: dict[str, Any]) -> ApplicationResult:
        if not self.ports.image_provider_status()["available"] and not self.ports.fallback_enabled():
            raise RequestError("feature_unavailable", "AI style preview generation is not configured.", 503)
        _text_field(request.get("request_id"), "request_id")
        prompt = _text_field(request.get("prompt"), "prompt")
        palette = ()
        palette_roles = {}
        style = _normalize_style(request.get("style"))
        custom_style = _normalize_custom_style(request.get("custom_style"), style)
        print_settings = _normalize_print_settings(request.get("print"))
        job = self.ports.new_job("text", palette, palette_roles, style, custom_style, print_settings,
                       provider=_generation_provider(request), generation_options=request)
        job.palette_recommendation_confirmed = _boolean_field(
            request.get("palette_recommendation_confirmed"), "palette_recommendation_confirmed"
        )
        job.user_prompt = prompt
        self.ports.persist(job)
        with self.ports.lock:
            self.ports.jobs[job.id] = job
        try:
            self.ports.submit(job, self.ports.preprocess_text, prompt)
        except RequestError:
            with self.ports.lock:
                self.ports.jobs.pop(job.id, None)
            self.ports.cleanup(job)
            raise
        with self.ports.lock:
            response = self.ports.present(job)
        return ApplicationResult(202, {"job": response})

    def recommend_model_style(self, fields: dict[str, str], image: bytes, declared_type: str) -> ApplicationResult:
        prompt = fields.get("instruction", "").strip()
        if len(image) > MAX_IMAGE_BYTES:
            raise RequestError("image_too_large", "Image exceeds the 20 MB limit.", 413)
        detected_type = _image_type(image)
        if detected_type is None:
            raise RequestError("unsupported_image", "Image must be PNG or JPEG.", 415)
        if declared_type not in {"application/octet-stream", detected_type}:
            raise RequestError("unsupported_image", "Image Content-Type does not match its data.", 415)
        try:
            _validate_image_data(image, minimum_edge=MIN_SOURCE_IMAGE_EDGE)
            recommendation = recommend_printable_style(image, prompt=prompt)
        except (ValueError, ModelInputImageQualityError) as exc:
            raise RequestError("invalid_image", str(exc), 415) from None
        return ApplicationResult(200, {"recommendation": recommendation})

    def create_image_job(self, fields: dict[str, str], image: bytes, declared_type: str) -> ApplicationResult:
        if not self.ports.image_provider_status()["available"]:
            raise RequestError("feature_unavailable", "AI style preview generation is not configured.", 503)
        _text_field(fields.get("request_id"), "request_id")
        user_instruction = _user_image_instruction(fields.get("instruction"))
        instruction = _normalize_image_instruction(user_instruction)
        palette = ()
        palette_roles = {}
        style = _normalize_style(fields.get("style"))
        custom_style = _normalize_custom_style(fields.get("custom_style"), style)
        palette_recommendation_confirmed = _boolean_field(
            fields.get("palette_recommendation_confirmed"), "palette_recommendation_confirmed"
        )
        try:
            print_payload = json.loads(fields.get("print", "{}"))
        except json.JSONDecodeError:
            raise RequestError("invalid_print_settings", "print settings must be valid JSON", 400) from None
        print_settings = _normalize_print_settings(print_payload)
        if len(image) > MAX_IMAGE_BYTES:
            raise RequestError("image_too_large", "Image exceeds the 20 MB limit.", 413)
        detected_type = _image_type(image)
        if detected_type is None:
            raise RequestError("unsupported_image", "Image must be PNG or JPEG.", 415)
        if declared_type not in {"application/octet-stream", detected_type}:
            raise RequestError("unsupported_image", "Image Content-Type does not match its data.", 415)
        try:
            _validate_image_data(image, minimum_edge=MIN_SOURCE_IMAGE_EDGE)
        except ValueError as exc:
            raise RequestError("invalid_image", str(exc), 415) from None
        job = self.ports.new_job("image", palette, palette_roles, style, custom_style, print_settings,
                       provider=_generation_provider(fields), generation_options=fields)
        job.palette_recommendation_confirmed = palette_recommendation_confirmed
        job.user_prompt = user_instruction
        job.image_metrics["source_filename"] = fields.get("_source_filename", "")
        suffix = ".png" if detected_type == "image/png" else ".jpg"
        input_path = job.directory / f"input-{uuid.uuid4().hex}{suffix}"
        try:
            input_path.write_bytes(image)
        except OSError:
            self.ports.cleanup(job)
            raise RequestError("service_unavailable", "The uploaded image could not be stored.", 503, True) from None
        job.input_path = input_path
        self.ports.persist(job)
        with self.ports.lock:
            self.ports.jobs[job.id] = job
        try:
            self.ports.submit(job, self.ports.preprocess_image, input_path, instruction)
        except RequestError:
            with self.ports.lock:
                self.ports.jobs.pop(job.id, None)
            self.ports.cleanup(job)
            raise
        with self.ports.lock:
            response = self.ports.present(job)
        return ApplicationResult(202, {"job": response})

    def confirm_palette(self, job_id: str, request: dict[str, Any]) -> ApplicationResult:
        job = self.ports.get_job(job_id)
        if job is None:
            raise RequestError("job_not_found", "Model job not found.", 404)
        with self.ports.lock:
            if job.state != "awaiting_palette_confirmation" or not job.palette_recommendation:
                raise RequestError("invalid_job_state", "Job is not awaiting palette confirmation.", 409)
            previous_recommendation = job.palette_recommendation
            _use_unrestricted_creation(job)
            job.state = "preprocessing"
            job.phase = "preprocessing"
            job.message = "An unrestricted design preview is being prepared."
            job.progress = 10
            self.ports.persist(job)
        try:
            if job.source == "text":
                self.ports.submit(job, self.ports.preprocess_text, job.user_prompt)
            elif job.input_path is not None:
                self.ports.submit(job, self.ports.preprocess_image, job.input_path, _normalize_image_instruction(job.user_prompt))
            else:
                raise RequestError("input_unavailable", "The stored reference image is unavailable.", 409)
        except RequestError:
            with self.ports.lock:
                job.palette_recommendation_confirmed = False
                job.palette_recommendation = previous_recommendation
                job.state = "awaiting_palette_confirmation"
                job.phase = "awaiting_palette_confirmation"
                job.message = "Review and confirm the recommended design colors."
                job.progress = 10
                self.ports.persist(job)
            raise
        with self.ports.lock:
            response = self.ports.present(job)
        return ApplicationResult(200, {"job": response})

    def set_generation_options(self, job_id: str, request: dict[str, Any]) -> ApplicationResult:
        if set(request) != {"provider", "face_limit", "geometry_quality", "texture_quality", "output_format"}:
            raise RequestError("invalid_request", "Complete generation options are required.", 400)
        provider = _generation_provider(request)
        face_limit = _normalize_face_limit(request["face_limit"])
        geometry, texture, output = _generation_options(request, face_limit)
        with self.ports.lock:
            job = self.ports.get_job(job_id)
            if job is None:
                raise RequestError("job_not_found", "Model job not found.", 404)
            retry_design = self.ports.can_retry_hunyuan(job)
            if (job.state != "awaiting_confirmation" or job.attempts) and not retry_design:
                raise RequestError("invalid_job_state", "Only an unsubmitted design can change generation options.", 409)
            if retry_design and provider != job.provider:
                raise RequestError("invalid_request", "A rejected Hunyuan design must keep its provider for retry.", 409)
            names = ("provider", "face_limit", "generation_profile", "geometry_quality", "texture_quality", "output_format")
            previous = tuple(getattr(job, name) for name in names)
            values = (provider, face_limit, "quality" if face_limit >= 500000 else "performance", geometry, texture, output)
            for name, value in zip(names, values):
                setattr(job, name, value)
            try:
                self.ports.persist(job, required=True)
            except TripoError:
                for name, value in zip(names, previous):
                    setattr(job, name, value)
                raise RequestError("state_save_failed", "Generation options could not be saved.", 503, True) from None
            response = self.ports.present(job)
        return ApplicationResult(200, {"job": response})

    def reuse_design(self, job_id: str, request: dict[str, Any]) -> ApplicationResult:
        if request:
            raise RequestError("invalid_request", "Reusing a design takes no provider or file arguments.", 400)
        source = self.ports.get_job(job_id)
        if source is None:
            raise RequestError("job_not_found", "Saved design not found.", 404)
        with self.ports.lock:
            child = self.ports.reuse_design(source)
            self.ports.jobs[child.id] = child
            response = self.ports.present(child)
        return ApplicationResult(200, {"job": response})

    def generate(self, job_id: str, request: dict[str, Any]) -> ApplicationResult:
        if "prepared_prompt" not in request:
            raise RequestError("invalid_request", "prepared_prompt is required.", 400)
        raw_prompt = request.get("prepared_prompt")
        if not isinstance(raw_prompt, str):
            raise RequestError("invalid_request", "prepared_prompt must be a string.", 400)
        if len(raw_prompt.strip().encode("utf-8")) > MAX_PROMPT_BYTES:
            raise RequestError("invalid_request", "prepared_prompt exceeds the 2000-byte limit.", 400)
        resume_existing = _boolean_field(request.get("resume_existing"), "resume_existing", default=False)
        job = self.ports.get_job(job_id)
        if job is None:
            raise RequestError("job_not_found", "Model job not found.", 404)
        if "generation_profile" in request and "face_limit" not in request:
            generation_profile = _normalize_generation_profile(request.get("generation_profile"))
            face_limit = GENERATION_PROFILE_FACE_LIMITS[generation_profile]
        else:
            face_limit = _normalize_face_limit(request.get("face_limit", DEFAULT_MODEL_FACE_LIMIT))
            generation_profile = "quality" if face_limit >= 500000 else "performance"
        geometry_quality, texture_quality, output_format = _generation_options(request, face_limit)
        provider = _generation_provider(request)
        with self.ports.lock:
            manual_retry = self.ports.can_retry_hunyuan(job)
            provider_task_id = self.ports.latest_task_id(job)
            unsubmitted_retry = self.ports.can_retry_unsubmitted(job)
            if resume_existing:
                if not provider_task_id:
                    raise RequestError(
                        "provider_task_reference_missing",
                        "The saved job has no provider task reference; it cannot be resumed safely.",
                        409,
                    )
                if job.state not in {"failed", "stopped", "queued", "running"}:
                    raise RequestError("invalid_job_state", "The saved provider task cannot be resumed in this state.", 409)
            elif provider_task_id and job.state == "failed":
                raise RequestError(
                    "resume_required",
                    "A provider task already exists. Resume the saved task instead of creating another one.",
                    409,
                )
            elif (job.state != "awaiting_confirmation" and not manual_retry and not unsubmitted_retry and
                  not (job.state == "failed" and not provider_task_id)):
                raise RequestError("invalid_job_state", "Job is not awaiting confirmation or an explicit safe retry.", 409)
            if job.state == "failed" and not provider_task_id and not resume_existing and not unsubmitted_retry and not manual_retry:
                raise RequestError(
                    "provider_task_ambiguous",
                    "The submission result is ambiguous and no provider task reference was saved. Confirm the provider task before retrying.",
                    409,
                )
            if manual_retry and provider != job.provider:
                raise RequestError("invalid_request", "A rejected Hunyuan design must keep its provider for retry.", 409)
            if resume_existing and provider != job.provider:
                raise RequestError("invalid_request", "A resumed provider task must keep its original provider.", 409)
            _use_unrestricted_creation(job)
            prepared_prompt = raw_prompt.strip()
            reference = _model_generation_reference(job)
            if job.source == "text" and reference is None and not prepared_prompt:
                raise RequestError("invalid_request", "prepared_prompt is required for text generation.", 400)
            if job.source == "image" or reference is not None:
                try:
                    self.ports.validate_image(
                        reference,
                        minimum_edge=MIN_MODEL_REFERENCE_EDGE,
                        require_visual_detail=True,
                    )
                except ValueError as exc:
                    raise RequestError(
                        "invalid_model_reference",
                        f"The generated image is not suitable for 3D input: {exc}",
                        409,
                    ) from None
                self.ports.assess_reference(job)
            if not self.ports.gateway(provider).model_generation_available():
                raise RequestError("feature_unavailable", "Model generation is not configured.", 503)
            next_attempt = len(job.attempts) + 1
            authorization = None if resume_existing else PaidTaskAuthorization.confirmed(
                f"{job.id}:model:{next_attempt}", provider
            )
            job.provider = provider
            job.prepared_prompt = prepared_prompt if job.source == "text" else ""
            job.face_limit = face_limit
            job.generation_profile = generation_profile
            job.geometry_quality = geometry_quality
            job.texture_quality = texture_quality
            job.output_format = output_format
            job.state = "queued"
            job.phase = "generating"
            job.message = "Generation queued."
            job.progress = 20
            self.ports.clear_artifact(job)
            # A previous stop is local intent. An explicit retry or resume
            # clears that intent before the worker reaches its first boundary.
            job.stop_event.clear()
            self.ports.persist(job)
        try:
            self.ports.submit(job, self.ports.generate, prepared_prompt, resume_existing, authorization)
        except RequestError:
            with self.ports.lock:
                job.state = "failed" if (manual_retry or unsubmitted_retry or resume_existing) else "awaiting_confirmation"
                job.phase = "failed" if job.state == "failed" else "awaiting_confirmation"
                job.message = (
                    "The saved provider task could not be resumed."
                    if resume_existing else "Review the prepared request before generation."
                )
                job.progress = 15
                self.ports.persist(job)
            raise
        with self.ports.lock:
            response = self.ports.present(job)
        return ApplicationResult(200, {"job": response})

    def retexture(self, reference_job_id: str, request: dict[str, Any]) -> ApplicationResult:
        if set(request) != {"geometry_job_id"}:
            raise RequestError(
                "invalid_request",
                "Retexture requests require only geometry_job_id.",
                400,
            )
        geometry_job_id = request.get("geometry_job_id")
        if not isinstance(geometry_job_id, str):
            raise RequestError("invalid_request", "geometry_job_id must be a UUID string.", 400)
        try:
            parsed_geometry_id = uuid.UUID(geometry_job_id)
        except ValueError:
            raise RequestError("invalid_request", "geometry_job_id must be a UUID string.", 400) from None
        if str(parsed_geometry_id) != geometry_job_id.lower():
            raise RequestError("invalid_request", "geometry_job_id must be a canonical UUID string.", 400)
        geometry_job_id = geometry_job_id.lower()

        reference_job = self.ports.get_job(reference_job_id)
        geometry_job = self.ports.get_job(geometry_job_id)
        if geometry_job is None:
            geometry_job = self.ports.adopt_legacy(geometry_job_id)
        if reference_job is None or geometry_job is None:
            raise RequestError("job_not_found", "The reference or geometry model job was not found.", 404)
        if geometry_job.provider != "tripo":
            raise RequestError("unsupported_provider_operation",
                               "Hunyuan history does not support preserved-geometry texturing.", 400)
        with self.ports.lock:
            if reference_job.state not in {"awaiting_confirmation", "ready"}:
                raise RequestError(
                    "invalid_job_state",
                    "The current image reference is not ready for preserved-geometry texturing.",
                    409,
                )
            if geometry_job.state != "ready" or not _file_info(geometry_job.artifact_path)[0]:
                raise RequestError(
                    "geometry_not_ready",
                    "The selected historical model geometry is not ready.",
                    409,
                )
            reference = _model_generation_reference(reference_job)
            try:
                self.ports.validate_image(
                    reference,
                    minimum_edge=MIN_MODEL_REFERENCE_EDGE,
                    require_visual_detail=True,
                )
            except ValueError as exc:
                raise RequestError("invalid_model_reference", str(exc), 409) from None
            source_attempt = next(
                (
                    attempt for attempt in reversed(geometry_job.attempts)
                    if isinstance(attempt.get("generation_task_id"), str)
                    and attempt.get("generation_task_id")
                    and attempt.get("status") in {"accepted", "running"}
                ),
                None,
            )
            source_task_id = str(source_attempt.get("generation_task_id", "")) if source_attempt else ""
            if not source_task_id:
                raise RequestError(
                    "geometry_source_unavailable",
                    "The selected historical model has no reusable provider geometry reference.",
                    409,
                )
            if not self.ports.gateway("tripo").model_generation_available():
                raise RequestError("feature_unavailable", "Model texturing is not configured.", 503)

        child = self.ports.new_job(
            reference_job.source,
            reference_job.palette,
            reference_job.palette_roles,
            reference_job.style,
            reference_job.custom_style,
            reference_job.print_settings,
            palette_color_count=reference_job.palette_color_count,
        )
        try:
            child.user_prompt = reference_job.user_prompt
            child.prepared_prompt = reference_job.prepared_prompt
            child.face_limit = geometry_job.face_limit
            child.generation_profile = geometry_job.generation_profile
            child.geometry_quality = geometry_job.geometry_quality
            child.texture_quality = geometry_job.texture_quality
            child.output_format = geometry_job.output_format
            child.palette_recommendation = json.loads(json.dumps(reference_job.palette_recommendation))
            child.palette_recommendation_confirmed = reference_job.palette_recommendation_confirmed
            child.image_metrics = json.loads(json.dumps(reference_job.image_metrics))
            child.preview_content_type = reference_job.preview_content_type
            child.input_path = _copy_job_file(reference_job.input_path, child, "input")
            child.raw_preview_path = _copy_job_file(reference_job.raw_preview_path, child, "style-preview-raw")
            _use_unrestricted_creation(child)
            child.strict_preview_path = _copy_job_file(reference_job.strict_preview_path, child, "four-color-preview")
            child.preview_path = _copy_job_file(reference_job.preview_path, child, "clean-preview")
            child.model_reference_path = _copy_job_file(reference, child, "model-reference")
            child.geometry_reference_path = _copy_job_file(
                reference_job.geometry_reference_path, child, "geometry-reference"
            )
            child.heatmap_path = _copy_job_file(reference_job.heatmap_path, child, "unprintable-heatmap")
            child.metadata_path = _copy_job_file(reference_job.metadata_path, child, "metadata")
            child.background_mask_path = _copy_job_file(reference_job.background_mask_path, child, "mask-background")
            child.subject_mask_path = _copy_job_file(reference_job.subject_mask_path, child, "mask-subject")
            child.mask_paths = {
                role: copied
                for role, path in reference_job.mask_paths.items()
                if (copied := _copy_job_file(path, child, f"mask-{role}")) is not None
            }
            child.state = "queued"
            child.phase = "texturing"
            child.message = "Preserved-geometry texture generation queued."
            child.progress = 20
            self.ports.record_attempt(
                child,
                1,
                provider="tripo",
                provider_operation="model_texture",
                provider_request_id=f"{child.id}:texture:1",
                source_job_id=geometry_job.id,
                source_task_id=source_task_id,
                status="creating",
                error="",
            )
            with self.ports.lock:
                self.ports.jobs[child.id] = child
                self.ports.persist(child)
            authorization = PaidTaskAuthorization.confirmed_texture(f"{child.id}:texture:1")
            self.ports.submit(child, self.ports.retexture, geometry_job.id, source_task_id, False, authorization)
        except RequestError:
            with self.ports.lock:
                self.ports.jobs.pop(child.id, None)
            self.ports.remove_state(child)
            raise
        with self.ports.lock:
            response = self.ports.present(child)
        return ApplicationResult(202, {"job": response})

    def stop(self, job_id: str, request: dict[str, Any]) -> ApplicationResult:
        job = self.ports.get_job(job_id)
        if job is None:
            raise RequestError("job_not_found", "Model job not found.", 404)
        with self.ports.lock:
            if job.state in {"recommending_palette", "preprocessing", "queued", "running", "stopping"}:
                job.stop_event.set()
                job.state = "stopping"
                job.phase = "stopping"
                job.message = "Stopping model generation."
                self.ports.persist(job)
            elif job.state in {"awaiting_palette_confirmation", "awaiting_confirmation"}:
                job.stop_event.set()
                job.state = "stopped"
                job.phase = "stopped"
                job.message = "Model generation stopped."
                job.progress = 0
                self.ports.persist(job)
            elif job.state != "stopped":
                raise RequestError("invalid_job_state", "Job cannot be stopped in its current state.", 409)
            response = self.ports.present(job)
        return ApplicationResult(200, {"job": response})

    def delete(self, job_id: str) -> ApplicationResult:
        with self.ports.lock:
            job = self.ports.jobs.get(job_id)
            if job is None:
                raise RequestError("job_not_found", "Model job not found.", 404)
            if job.state in {"recommending_palette", "preprocessing", "queued", "running", "stopping"}:
                job.delete_requested = True
                job.stop_event.set()
                job.state = "stopping"
                job.phase = "stopping"
                job.message = "Stopping model generation."
            else:
                self.ports.jobs.pop(job_id)
                self.ports.remove_state(job)
        return ApplicationResult(204, {})

    def recheck(self, job_id: str, request: dict[str, Any]) -> ApplicationResult:
        job = self.ports.get_job(job_id)
        if job is None:
            job = self.ports.adopt_legacy(job_id)
        if job is None:
            raise RequestError("job_not_found", "Model job not found.", 404)
        with self.ports.lock:
            if job.state in {"preprocessing", "queued", "running", "stopping"}:
                raise RequestError("invalid_job_state", "Model quality cannot be checked while the job is running.", 409)
            artifact = job.artifact_path
            artifact_format = job.artifact_format
        if artifact is None or artifact_format not in {"obj", "glb"}:
            raise RequestError("artifact_not_ready", "The model OBJ is not available for quality checking.", 409)
        try:
            resolved_artifact = artifact.resolve(strict=True)
            resolved_artifact.relative_to(job.directory.resolve(strict=True))
        except (OSError, ValueError):
            raise RequestError("artifact_not_ready", "The registered model OBJ is unavailable.", 409) from None
        quality = self.ports.analyze_model(
            self.ports.analysis_artifact(resolved_artifact),
            allow_repairable_topology=True,
            target_palette=job.palette,
        )
        try:
            self.ports.write_quality(quality, job.directory / MODEL_QUALITY_FILENAME)
        except ModelQualityError as exc:
            raise RequestError("quality_report_unavailable", str(exc), 503, True) from None
        with self.ports.lock:
            if self.ports.jobs.get(job_id) is not job:
                raise RequestError("job_not_found", "Model job is no longer available.", 404)
            self.ports.persist(job)
            response = self.ports.present(job)
        return ApplicationResult(200, {"job": response})

    def visual_review(self, job_id: str, request: dict[str, Any]) -> ApplicationResult:
        force = request.get("force", False)
        if not isinstance(force, bool):
            raise RequestError("invalid_request", "force must be a boolean.", 400)
        job = self.ports.get_job(job_id)
        if job is None:
            job = self.ports.adopt_legacy(job_id)
        if job is None:
            raise RequestError("job_not_found", "Model job not found.", 404)
        with self.ports.lock:
            if job.state in {"preprocessing", "queued", "running", "stopping"}:
                raise RequestError("job_busy", "Model generation is still running.", 409)
            artifact = job.artifact_path
        if artifact is None or job.artifact_format not in {"obj", "glb"}:
            raise RequestError("artifact_not_ready", "The model OBJ is not available for visual review.", 409)
        try:
            resolved_directory = job.directory.resolve(strict=True)
            resolved_artifact = artifact.resolve(strict=True)
            resolved_artifact.relative_to(resolved_directory)
        except (OSError, ValueError):
            raise RequestError("artifact_not_ready", "The registered model OBJ is unavailable.", 409) from None
        reference: Path | None = None
        if job.source == "image" and job.input_path is not None:
            try:
                candidate = job.input_path.resolve(strict=True)
                candidate.relative_to(resolved_directory)
                reference = candidate
            except (OSError, ValueError):
                reference = None
        self.ports.review_model(
            self.ports.analysis_artifact(resolved_artifact),
            resolved_directory,
            description=job.user_prompt,
            style=job.style,
            reference_path=reference,
            force=force,
        )
        with self.ports.lock:
            if self.ports.jobs.get(job_id) is not job:
                raise RequestError("job_not_found", "Model job is no longer available.", 404)
            self.ports.persist(job)
            response = self.ports.present(job)
        return ApplicationResult(200, {"job": response})
