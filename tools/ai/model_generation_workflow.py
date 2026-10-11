"""ModelGenerationWorkflow: application orchestration over explicit ports, independent of HTTP and UI."""
from __future__ import annotations

import sys
import traceback
from dataclasses import dataclass
from model_contracts import (
    Job,
    JobStopped,
    MAX_GENERATION_ATTEMPTS,
    MIN_MODEL_REFERENCE_EDGE,
    PortraitMultiviewPreparationError,
    SidecarRestart,
)
from model_creation_policy import _use_unrestricted_creation
from model_job_support import validate_face_target as _validate_face_target
from model_provider_gateway import (
    ModelTaskRequest,
    PaidTaskAuthorization,
    ProviderGatewayError,
    TextureTaskRequest,
)
from pathlib import Path
from portrait_geometry_reference import (
    _geometry_generation_reference,
    _identity_preserving_portrait_geometry_enabled,
    _model_generation_reference,
)
from tripo_client import TripoError
from typing import Any, Callable, ContextManager

@dataclass(frozen=True)
class ModelGenerationWorkflowPorts:
    """Only the composition root chooses executors, storage and paid providers."""

    lock: ContextManager[Any]
    persist: Callable[..., None]
    stop_boundary: Callable[[Job], None]
    fail: Callable[[Job, str], None]
    mark_stopped: Callable[[Job], None]
    finish_deleted: Callable[[Job], None]
    gateway: Callable[[str], Any]
    download_artifact: Callable[..., Any]
    promote_artifact: Callable[..., None]
    analysis_artifact: Callable[..., Any]
    automatic_visual_review: Callable[..., None]
    ensure_multiview: Callable[[Job], Any]
    multiview_paths: Callable[[Job, str], dict[str, Path] | None]
    write_color_intent: Callable[[Job, Path], None]
    progress_callback: Callable[..., Any]
    record_attempt: Callable[..., None]
    return_to_multiview_retry: Callable[..., None]
    multiview_enabled: Callable[[Job], bool]
    is_shutting_down: Callable[[], bool]
    validate_image: Callable[..., Any]
    validate_topology: Callable[..., Any]


class ModelGenerationWorkflow:
    def __init__(self, ports: ModelGenerationWorkflowPorts):
        self.ports = ports


    def generate(self,
        job: Job,
        prepared_prompt: str,
        resume: bool = False,
        authorization: PaidTaskAuthorization | None = None,
    ) -> None:
        _use_unrestricted_creation(job)
        gateway = self.ports.gateway(job.provider)
        active_attempt = 0
        try:
            artifact: Path | None = None
            last_quality_error: TripoError | ProviderGatewayError | None = None
            first_attempt = 1
            if job.attempts:
                if resume:
                    # A provider task can be stored after an earlier quality
                    # attempt. Resume the attempt that owns the newest task ID,
                    # rather than assuming it is always attempt 1.
                    first_attempt = next(
                        (index for index, attempt in reversed(list(enumerate(job.attempts, start=1)))
                         if isinstance(attempt.get("generation_task_id"), str)
                         and attempt.get("generation_task_id")),
                        0,
                    )
                    if first_attempt == 0:
                        raise TripoError("The paid model task reference is unavailable; start a new generation manually.")
                else:
                    # Keep the local job and its prior attempt evidence. A retry
                    # without a provider task gets a new local attempt number but
                    # remains tied to the same job ID.
                    first_attempt = len(job.attempts) + 1
            last_attempt = first_attempt + MAX_GENERATION_ATTEMPTS - 1
            for attempt_number in range(first_attempt, last_attempt + 1):
                active_attempt = attempt_number
                self.ports.stop_boundary(job)
                with self.ports.lock:
                    job.state = "running"
                    job.phase = "generating"
                    job.message = f"Generating printable model (attempt {attempt_number} of {last_attempt})."
                    job.progress = 20
                    self.ports.persist(job)
                existing = job.attempts[attempt_number - 1] if resume and len(job.attempts) >= attempt_number else {}
                generation_id = existing.get("generation_task_id", "")
                if not isinstance(generation_id, str):
                    generation_id = ""
                if resume and not generation_id:
                    raise TripoError("The paid model task reference is unavailable; start a new generation manually.")
                identity_geometry = _identity_preserving_portrait_geometry_enabled(job)
                preview = _geometry_generation_reference(job)
                # Four generated portrait views can be individually attractive yet
                # geometrically inconsistent. In real Tripo validation this produced
                # a Janus mesh with a second face on the back. The source-faithful,
                # hard-alpha front preserved identity and yielded one coherent head.
                # Build the material turntable only after geometry exists, when every
                # view is rendered from that exact mesh and therefore cannot disagree.
                multiview_paths = None if identity_geometry else (
                    self.ports.multiview_paths(job, "generation_views")
                    if generation_id
                    else self.ports.ensure_multiview(job)
                )
                request_source = (
                    "multiview" if multiview_paths is not None
                    else "text" if job.source == "text" and preview is None
                    else "image"
                )
                if request_source == "image":
                    try:
                        self.ports.validate_image(
                            preview,
                            minimum_edge=MIN_MODEL_REFERENCE_EDGE,
                            require_visual_detail=True,
                        )
                    except ValueError as exc:
                        raise ProviderGatewayError(
                            f"The generated image is not suitable for 3D input: {exc}",
                            code="invalid_model_request",
                            category="validation",
                            provider=job.provider,
                            operation="model_generation",
                        ) from None
                with self.ports.lock:
                    job.phase = "generating"
                    job.message = (
                        f"Generating the high-quality portrait from four views (attempt {attempt_number} of "
                        f"{last_attempt})."
                        if request_source == "multiview"
                        else f"Generating identity-first portrait geometry from the approved front view (attempt {attempt_number} of "
                        f"{last_attempt})."
                        if identity_geometry
                        else f"Generating printable model (attempt {attempt_number} of {last_attempt})."
                    )
                    job.progress = 20
                    self.ports.persist(job)
                if not generation_id:
                    if authorization is None:
                        raise ProviderGatewayError(
                            "Explicit confirmation is required before creating a paid model task.",
                            code="authorization_required",
                            category="authorization",
                            provider=job.provider,
                            operation="model_generation",
                        )
                    self.ports.record_attempt(
                        job,
                        attempt_number,
                        provider=job.provider,
                        provider_operation="model_generation",
                        provider_request_id=authorization.request_id,
                        provider_model="hy-3d-3.1" if job.provider == "hunyuan" else "",
                        status="creating",
                        error="",
                    )
                    self.ports.persist(job, required=True)
                task_ref = gateway.start_or_reuse_model_task(
                    ModelTaskRequest(
                        source=request_source,
                        prompt=prepared_prompt,
                        image_path=preview,
                        image_paths=multiview_paths,
                        face_limit=job.face_limit,
                        generation_profile=job.generation_profile,
                        geometry_quality=job.geometry_quality,
                        texture_quality=job.texture_quality,
                        output_format=job.output_format,
                    ),
                    existing_task_id=generation_id,
                    authorization=authorization,
                )
                generation_id = task_ref.task_id
                if not task_ref.reused:
                    self.ports.record_attempt(job, attempt_number, generation_task_id=generation_id, status="running")
                else:
                    # Clear the previous failure presentation while retaining the
                    # same provider task identity and all immutable attempt data.
                    self.ports.record_attempt(
                        job,
                        attempt_number,
                        generation_task_id=generation_id,
                        status="running",
                        error="",
                        provider_error_code="",
                        provider_error_category="",
                        provider_error_retryable=False,
                        provider_error_ambiguous=False,
                    )
                self.ports.stop_boundary(job)
                gateway.wait_for_task(
                    generation_id,
                    stop_event=job.stop_event,
                    progress=self.ports.progress_callback(job, 20, 70),
                )
                self.ports.stop_boundary(job)
                try:
                    candidate = self.ports.download_artifact(job, generation_id, attempt_number, resume)
                    face_count, _, _ = self.ports.validate_topology(self.ports.analysis_artifact(candidate), quality_advisory=True)
                    warning = _validate_face_target(face_count, job.face_limit)
                    job.image_metrics["model_delivery_warnings"] = [warning] if warning else []
                    artifact = job.directory / ("model.glb" if candidate.suffix.lower() == ".glb" else "model-vertex-color.obj")
                    self.ports.promote_artifact(candidate, artifact)
                    self.ports.record_attempt(job, attempt_number, status="accepted", artifact=str(candidate.name), error="")
                    break
                except (TripoError, ProviderGatewayError) as exc:
                    if self.ports.is_shutting_down():
                        raise SidecarRestart() from None
                    message = str(exc)
                    retryable_quality_error = any(
                        marker in message.lower()
                        for marker in ("triangle limit", "non-watertight", "non-manifold", "degenerate triangle")
                    )
                    updates: dict[str, Any] = {"status": "rejected", "error": message}
                    if isinstance(exc, ProviderGatewayError):
                        updates.update(
                            provider_error_code=exc.code,
                            provider_error_category=exc.category,
                            provider_error_retryable=exc.retryable,
                            provider_error_ambiguous=exc.ambiguous,
                        )
                    self.ports.record_attempt(job, attempt_number, **updates)
                    if not retryable_quality_error or attempt_number == last_attempt:
                        raise
                    last_quality_error = exc
            if artifact is None:
                raise last_quality_error or TripoError("No printable model passed validation.")

            visual_quality = self.ports.automatic_visual_review(job, artifact)
            self.ports.write_color_intent(job, artifact)
            with self.ports.lock:
                if job.stop_event.is_set():
                    raise JobStopped()
                job.artifact_path = artifact
                job.artifact_format = artifact.suffix.lower().lstrip(".")
                job.state = "ready"
                job.phase = "ready"
                job.message = (
                    "Generated model is ready, but visual review found identity or material risks."
                    if visual_quality is not None and not visual_quality.get("import_recommended", True)
                    else "Generated model is ready. Quality checks are advisory; visual review is available on request."
                )
                job.progress = 100
                self.ports.persist(job)
        except SidecarRestart:
            with self.ports.lock:
                job.state = "queued"
                job.phase = "resuming"
                job.message = "The existing paid model task will resume when the sidecar restarts."
                self.ports.persist(job)
        except JobStopped:
            self.ports.mark_stopped(job)
        except PortraitMultiviewPreparationError as exc:
            self.ports.return_to_multiview_retry(job, str(exc))
        except ProviderGatewayError as exc:
            if self.ports.is_shutting_down():
                # ``wait_for_task`` uses the job stop event to interrupt its local
                # polling loop during shutdown.  That interruption does not cancel
                # the already-paid remote task, so do not persist it as a provider
                # rejection.  A restarted sidecar must see the attempt as running
                # and resume the same task id instead of presenting a contradictory
                # "rejected" attempt beside a live progress bar.
                with self.ports.lock:
                    job.state = "queued"
                    job.phase = "resuming"
                    job.message = "The existing paid model task will resume when the sidecar restarts."
                    self.ports.persist(job)
            else:
                if active_attempt:
                    self.ports.record_attempt(
                        job,
                        active_attempt,
                        status="rejected",
                        error=str(exc),
                        provider_error_code=exc.code,
                        provider_error_category=exc.category,
                        provider_error_retryable=exc.retryable,
                        provider_error_ambiguous=exc.ambiguous,
                    )
                self.ports.fail(job, str(exc))
        except TripoError as exc:
            if self.ports.is_shutting_down():
                with self.ports.lock:
                    job.state = "queued"
                    job.phase = "resuming"
                    job.message = "The existing paid model task will resume when the sidecar restarts."
                    self.ports.persist(job)
            else:
                self.ports.fail(job, str(exc))
        except Exception as exc:
            print(
                f"[orca-ai] unexpected model-generation error for job {job.id}: "
                f"{type(exc).__name__}: {exc}",
                file=sys.stderr,
                flush=True,
            )
            traceback.print_exc()
            if (
                not job.attempts
                and self.ports.multiview_enabled(job)
                and not _identity_preserving_portrait_geometry_enabled(job)
                and job.model_reference_path is not None
            ):
                self.ports.return_to_multiview_retry(
                    job,
                    "Four-view portrait preparation hit a local error before any paid Tripo task was created. Retry generation.",
                )
            else:
                self.ports.fail(job, "Model generation failed.")
        finally:
            self.ports.finish_deleted(job)

    def retexture(self,
        job: Job,
        source_job_id: str,
        source_task_id: str,
        resume: bool = False,
        authorization: PaidTaskAuthorization | None = None,
    ) -> None:
        _use_unrestricted_creation(job)
        try:
            self.ports.stop_boundary(job)
            with self.ports.lock:
                job.state = "running"
                job.phase = "texturing"
                job.message = "Preserving the selected geometry and applying the current portrait colors."
                job.progress = 20
                self.ports.persist(job)
            existing = job.attempts[0] if resume and job.attempts else {}
            generation_id = existing.get("generation_task_id", "")
            if not isinstance(generation_id, str):
                generation_id = ""
            if resume and not generation_id:
                raise TripoError("The paid texture task reference is unavailable; start a new task manually.")
            reference = _model_generation_reference(job)
            try:
                self.ports.validate_image(
                    reference,
                    minimum_edge=MIN_MODEL_REFERENCE_EDGE,
                    require_visual_detail=True,
                )
            except ValueError as exc:
                raise ProviderGatewayError(
                    f"The texture reference image is not suitable: {exc}",
                    code="invalid_texture_request",
                    category="validation",
                    provider="tripo",
                    operation="model_texture",
                ) from None
            if not generation_id:
                if authorization is None:
                    raise ProviderGatewayError(
                        "Explicit confirmation is required before creating a paid texture task.",
                        code="authorization_required",
                        category="authorization",
                        provider="tripo",
                        operation="model_texture",
                    )
                self.ports.record_attempt(
                    job,
                    1,
                    provider="tripo",
                    provider_operation="model_texture",
                    provider_request_id=authorization.request_id,
                    source_job_id=source_job_id,
                    source_task_id=source_task_id,
                    status="creating",
                    error="",
                )
            task_ref = self.ports.gateway("tripo").start_or_reuse_texture_task(
                TextureTaskRequest(
                    source_task_id=source_task_id,
                    image_path=reference,
                    texture_alignment="geometry",
                    texture_quality="standard",
                ),
                existing_task_id=generation_id,
                authorization=authorization,
            )
            generation_id = task_ref.task_id
            if not task_ref.reused:
                self.ports.record_attempt(job, 1, generation_task_id=generation_id, status="running")
            self.ports.stop_boundary(job)
            self.ports.gateway("tripo").wait_for_task(
                generation_id,
                stop_event=job.stop_event,
                progress=self.ports.progress_callback(job, 20, 70),
            )
            self.ports.stop_boundary(job)
            candidate = self.ports.download_artifact(job, generation_id, 1, resume)
            face_count, _, _ = self.ports.validate_topology(self.ports.analysis_artifact(candidate), quality_advisory=True)
            warning = _validate_face_target(face_count, job.face_limit)
            job.image_metrics["model_delivery_warnings"] = [warning] if warning else []
            artifact = job.directory / ("model.glb" if candidate.suffix.lower() == ".glb" else "model-vertex-color.obj")
            self.ports.promote_artifact(candidate, artifact)
            self.ports.record_attempt(job, 1, status="accepted", artifact=str(candidate.name), error="")
            visual_quality = self.ports.automatic_visual_review(job, artifact)
            self.ports.write_color_intent(job, artifact)
            with self.ports.lock:
                if job.stop_event.is_set():
                    raise JobStopped()
                job.artifact_path = artifact
                job.artifact_format = artifact.suffix.lower().lstrip(".")
                job.state = "ready"
                job.phase = "ready"
                job.message = (
                    "The preserved-geometry portrait is ready, but visual review found identity or material risks."
                    if visual_quality is not None and not visual_quality.get("import_recommended", True)
                    else "The preserved-geometry portrait model is ready."
                )
                job.progress = 100
                self.ports.persist(job)
        except SidecarRestart:
            with self.ports.lock:
                job.state = "queued"
                job.phase = "resuming"
                job.message = "The existing paid texture task will resume when the sidecar restarts."
                self.ports.persist(job)
        except JobStopped:
            self.ports.mark_stopped(job)
        except ProviderGatewayError as exc:
            self.ports.record_attempt(
                job,
                1,
                status="rejected",
                error=str(exc),
                provider_error_code=exc.code,
                provider_error_category=exc.category,
                provider_error_retryable=exc.retryable,
                provider_error_ambiguous=exc.ambiguous,
            )
            if self.ports.is_shutting_down():
                with self.ports.lock:
                    job.state = "queued"
                    job.phase = "resuming"
                    job.message = "The existing paid texture task will resume when the sidecar restarts."
                    self.ports.persist(job)
            else:
                self.ports.fail(job, str(exc))
        except TripoError as exc:
            if self.ports.is_shutting_down():
                with self.ports.lock:
                    job.state = "queued"
                    job.phase = "resuming"
                    job.message = "The existing paid texture task will resume when the sidecar restarts."
                    self.ports.persist(job)
            else:
                self.ports.record_attempt(job, 1, status="rejected", error=str(exc))
                self.ports.fail(job, str(exc))
        except Exception:
            self.ports.record_attempt(job, 1, status="rejected", error="Texture generation failed.")
            self.ports.fail(job, "Texture generation failed.")
        finally:
            self.ports.finish_deleted(job)
