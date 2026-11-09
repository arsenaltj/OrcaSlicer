"""One explicitly approved four-image model task over existing artifact ports."""
from __future__ import annotations

from advanced_multiview_model_contracts import CREATION_KIND, approved_paths
from model_contracts import Job, JobStopped, RequestError, SidecarRestart
from model_job_support import validate_face_target
from model_provider_gateway import ModelTaskRequest, PaidTaskAuthorization, ProviderGatewayError
from tripo_client import TripoError


class AdvancedMultiviewModelWorkflow:
    def __init__(self, ports):
        self.ports = ports

    def generate(self, job: Job, prepared_prompt: str = "", resume: bool = False,
                 authorization: PaidTaskAuthorization | None = None) -> None:
        try:
            if job.creation_kind != CREATION_KIND:
                raise RequestError("invalid_multiview_snapshot", "An advanced model source is required.", 409)
            paths = approved_paths(job.approved_multiview_snapshot, job.directory)
            self.ports.stop_boundary(job)
            existing = job.attempts[-1] if job.attempts else {}
            task_id = existing.get("generation_task_id", "")
            if resume and not task_id:
                raise RequestError("provider_task_ambiguous", "No saved provider task exists; no new task will be submitted.", 409)
            if not resume and job.attempts:
                raise RequestError("advanced_plan_required", "A new paid task requires a new advanced plan.", 409)
            if not task_id:
                if authorization is None:
                    raise RequestError("authorization_required", "Confirm the four-view model plan before generating.", 409)
                self.ports.record_attempt(job, 1, provider=job.provider, provider_operation="model_generation",
                    provider_request_id=authorization.request_id, provider_model=job.geometry_model, status="creating",
                    approved_snapshot=job.approved_multiview_snapshot["fingerprint"],
                    submitted_views={v: r["sha256"] for v, r in job.approved_multiview_snapshot["views"].items()})
                self.ports.persist(job, required=True)
            with self.ports.lock:
                job.state, job.phase, job.progress = "running", "generating", 20
                job.message = "Generating one model from the four approved design views."
                self.ports.persist(job, required=True)
            gateway = self.ports.gateway(job.provider)
            task = gateway.start_or_reuse_model_task(ModelTaskRequest(
                source="multiview", image_paths=paths, face_limit=job.face_limit,
                generation_profile=job.generation_profile, geometry_quality=job.geometry_quality,
                texture_quality=job.texture_quality, output_format=job.output_format, model_id=job.geometry_model),
                existing_task_id=task_id, authorization=authorization)
            self.ports.record_attempt(job, 1, generation_task_id=task.task_id, status="running", error="")
            self.ports.persist(job, required=True)
            self.ports.stop_boundary(job)
            gateway.wait_for_task(task.task_id, stop_event=job.stop_event,
                                  progress=self.ports.progress_callback(job, 20, 70), model_id=job.geometry_model)
            self.ports.stop_boundary(job)
            candidate = self.ports.download_artifact(job, task.task_id, 1, resume)
            faces, _, _ = self.ports.validate_topology(self.ports.analysis_artifact(candidate), quality_advisory=True)
            warning = validate_face_target(faces, job.face_limit)
            job.image_metrics["model_delivery_warnings"] = [warning] if warning else []
            artifact = job.directory / ("model.glb" if candidate.suffix.lower() == ".glb" else "model-vertex-color.obj")
            self.ports.promote_artifact(candidate, artifact)
            self.ports.write_color_intent(job, artifact)
            self.ports.stop_boundary(job)
            with self.ports.lock:
                job.artifact_path, job.artifact_format = artifact, artifact.suffix.lower().lstrip(".")
                job.state = job.phase = "ready"
                job.progress, job.message = 100, "The four-view model is ready for the advanced workbench."
                self.ports.record_attempt(job, 1, status="accepted", error="")
                self.ports.persist(job, required=True)
        except SidecarRestart:
            with self.ports.lock:
                job.state = job.phase = "stopped"
                job.message = "Resume the saved advanced task explicitly after restart."
                self.ports.persist(job)
        except JobStopped:
            self.ports.mark_stopped(job)
        except (RequestError, ProviderGatewayError, TripoError, OSError, ValueError) as error:
            if self.ports.is_shutting_down():
                job.state = job.phase = "stopped"
                self.ports.persist(job)
            else:
                if job.attempts:
                    self.ports.record_attempt(job, 1, status="rejected", error=str(error),
                        provider_error_code=getattr(error, "code", "advanced_model_failed"),
                        provider_error_category=getattr(error, "category", "advanced_model"),
                        provider_error_ambiguous=getattr(error, "ambiguous", False),
                        provider_error_retryable=getattr(error, "retryable", False))
                self.ports.fail(job, str(error))
        except Exception:
            self.ports.fail(job, "Advanced model processing failed; its original task record is preserved.")
        finally:
            self.ports.finish_deleted(job)
