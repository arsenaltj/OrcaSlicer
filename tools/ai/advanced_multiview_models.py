"""Advanced model plans and frozen Job-v2 inputs. No HTTP or GUI dependencies."""
from __future__ import annotations

from copy import deepcopy
from dataclasses import dataclass
import shutil
import time
from typing import Any, Callable

from advanced_multiview_contracts import VIEWS, CREATION_KIND, check_revision, draft_fields, fingerprint, identity
from advanced_multiview_model_contracts import SNAPSHOT_SCHEMA, approved_paths
from advanced_multiview_model_workflow import AdvancedMultiviewModelWorkflow
from model_contracts import Job, RequestError
from model_job_repository import _load_job
from model_provider_gateway import PaidTaskAuthorization
from model_request import _generation_options, _generation_provider, _geometry_model, _normalize_face_limit


@dataclass(frozen=True)
class AdvancedModelPorts:
    jobs: Any
    lock: Any
    workflow_ports: Any
    submit: Callable
    present: Callable
    capability: Callable[[str, str], dict[str, Any]]


class AdvancedMultiviewModels:
    def __init__(self, repository, ports: AdvancedModelPorts):
        self.repository, self.ports = repository, ports
        self.workflow = AdvancedMultiviewModelWorkflow(ports.workflow_ports)

    def busy(self, group_id: str) -> bool:
        with self.ports.lock:
            return any(job.state in {"queued", "running", "stopping"} for job in self._group_jobs(group_id))

    def _group_jobs(self, group_id: str):
        return [job for job in self.ports.jobs.values() if job.creation_kind == CREATION_KIND and
                job.approved_multiview_snapshot.get("group_id") == group_id]

    def _require_resolved(self, group_id: str) -> None:
        with self.ports.lock:
            for job in self._group_jobs(group_id):
                attempt = job.attempts[-1] if job.attempts else {}
                if (not attempt.get("generation_task_id") and
                        (attempt.get("status") == "creating" or attempt.get("provider_error_ambiguous"))):
                    raise RequestError("provider_task_ambiguous", "Resolve the original model submission before creating another task.", 409)

    def _link(self, group, job) -> None:
        if not any(link["job_id"] == job.id for link in group["model_links"]):
            group["model_links"].append({"job_id": job.id, "command_id": job.id,
                "fingerprint": job.approved_multiview_snapshot["fingerprint"]})
            self.repository.save(group)

    def prepare(self, group_id: str, request: dict[str, Any]) -> dict[str, Any]:
        command = identity(request.get("command_id"), "command_id")
        provider = _generation_provider(request)
        face_limit = _normalize_face_limit(request.get("face_limit", 1000000))
        geometry, texture, output = _generation_options(request, face_limit)
        if face_limit not in {300000, 1000000, 2000000}:
            raise RequestError("invalid_generation_options", "Choose a supported model generation profile.", 400)
        capability = self.ports.capability(provider, _geometry_model(request))
        if capability.get("named_four_view_model") != "supported" or not capability.get("available"):
            raise RequestError("unsupported_multiview", "The selected model's named four-view input is unavailable.", 503)
        options = {"provider": provider, "face_limit": face_limit, "geometry_quality": geometry,
                   "texture_quality": texture, "output_format": output, "geometry_model": capability["model"]}
        with self.repository.transaction():
            group = self.repository.snapshot(group_id)
            check_revision(group, request.get("expected_group_revision"))
            if group["active_operation_id"] or self.busy(group_id):
                raise RequestError("group_busy", "Finish the active operation before modeling.", 409)
            self._require_resolved(group_id)
            if group["pending_views"]:
                raise RequestError("design_regeneration_required",
                                   "Regenerate design views whose source image or style changed before modeling.", 409)
            if not group["all_views_available"]:
                raise RequestError("missing_current_view", "Four usable design views are required.", 409)
            if draft_fields(request.get("draft")) != group["latest_submitted_draft"]:
                raise RequestError("draft_changed", "Generate the changed description before modeling.", 409)
            for view in VIEWS:
                attempt = group["views"][view].get("latest_attempt") or {}
                if attempt.get("state") in {"ambiguous", "result_pending", "result_saved"}:
                    raise RequestError("provider_task_ambiguous", "Resolve the outstanding image request first.", 409)
            snapshot = {"schema": SNAPSHOT_SCHEMA, "group_id": group_id, "snapshot_id": command,
                        "views": {v: {"version_id": group["views"][v]["current"]["id"],
                                     "path": f"inputs/{v}.png", "sha256": group["views"][v]["current"]["sha256"]} for v in VIEWS}}
            snapshot["fingerprint"] = fingerprint(snapshot)
            plan = {"schema": "advanced-multiview-model-plan-v1", "id": command, "job_id": command,
                    "group_id": group_id, "expected_group_revision": group["revision"],
                    "draft": deepcopy(group["latest_submitted_draft"]), "options": options,
                    "approved_multiview_snapshot": snapshot, "max_new_model_tasks": 1,
                    "max_new_image_calls": 0, "max_new_conversion_tasks": int(output == "obj")}
            digest = fingerprint(plan)
            path = self.repository.path(group_id, f"model-plans/{command}.json", must_exist=False)
            if path.exists():
                old = self.repository.read_record(group_id, f"model-plans/{command}.json")
                if old["confirmation_fingerprint"] != digest:
                    raise RequestError("idempotency_conflict", "The command belongs to a different model plan.", 409)
                return old
            plan.update(confirmation_fingerprint=digest, expires_at=time.time() + 600)
            self.repository.write_record(group_id, f"model-plans/{command}.json", plan)
            return plan

    def execute(self, group_id: str, request: dict[str, Any]) -> Job:
        command = identity(request.get("command_id"), "command_id")
        with self.repository.transaction():
            plan = self.repository.read_record(group_id, f"model-plans/{command}.json")
            if (request.get("plan_id") != command or
                    request.get("confirmation_fingerprint") != plan["confirmation_fingerprint"]):
                raise RequestError("stale_confirmation", "Confirm the current four-view model plan.", 409)
            if self.repository.path(group_id, f"reconciled-submissions/model/{command}.json", must_exist=False).exists():
                raise RequestError("submission_not_accepted", "This request was reconciled as not accepted. Confirm a new plan.", 409)
            group = self.repository.load(group_id)
            directory = self.repository.root.parent / command
            with self.ports.lock:
                job = self.ports.jobs.get(command) or _load_job(directory)
                if job is None and (directory / "job.json").exists():
                    raise RequestError("snapshot_unavailable", "The original model receipt cannot be read. Preserve it for recovery.", 409)
                existed = job is not None
                if job and (job.creation_kind != CREATION_KIND or
                            job.approved_multiview_snapshot != plan["approved_multiview_snapshot"]):
                    raise RequestError("idempotency_conflict", "The job identity belongs to another source.", 409)
                if not job:
                    if time.time() > plan["expires_at"]:
                        raise RequestError("stale_confirmation", "The model plan expired.", 409)
                    check_revision(group, plan["expected_group_revision"])
                    if group["active_operation_id"] or self.busy(group_id):
                        raise RequestError("group_busy", "The group has an active operation.", 409)
                    self._require_resolved(group_id)
                    options = plan["options"]
                    capability = self.ports.capability(options["provider"], options["geometry_model"])
                    if capability.get("named_four_view_model") != "supported" or not capability.get("available"):
                        raise RequestError("unsupported_multiview", "The confirmed model is no longer available.", 503)
                    for view in VIEWS:
                        if group["views"][view]["current_version_id"] != plan["approved_multiview_snapshot"]["views"][view]["version_id"]:
                            raise RequestError("stale_confirmation", "An approved design changed.", 409)
                    directory.mkdir(exist_ok=True)
                    (directory / "inputs").mkdir(exist_ok=True)
                    for view in VIEWS:
                        record = self.repository.version(group_id, view, group["views"][view]["current_version_id"])
                        shutil.copyfile(self.repository.path(group_id, record["path"]), directory / f"inputs/{view}.png")
                    approved_paths(plan["approved_multiview_snapshot"], directory)
                    draft = plan["draft"]
                    job = Job(id=command, source="image", directory=directory, creation_kind=CREATION_KIND,
                              approved_multiview_snapshot=deepcopy(plan["approved_multiview_snapshot"]),
                              state="stopped", phase="accepted", progress=15,
                              message="Four-view model accepted; no supplier task has started.", style=draft["style"], custom_style=draft["custom_style"],
                              user_prompt=draft["prompt"], design_provider=draft["design_provider"], design_model=draft["design_model"],
                              provider=options["provider"], geometry_model=options["geometry_model"], face_limit=options["face_limit"],
                              generation_profile="quality" if options["face_limit"] >= 500000 else "performance",
                              geometry_quality=options["geometry_quality"], texture_quality=options["texture_quality"], output_format=options["output_format"])
                    job.preview_path = job.model_reference_path = directory / "inputs/front.png"
                    job.preview_content_type = "image/png"
                    self.ports.workflow_ports.persist(job, required=True)
                self.ports.jobs[job.id] = job
            self._link(group, job)
            if not existed:
                try:
                    job.state = job.phase = "queued"
                    self.ports.workflow_ports.persist(job, required=True)
                    self.ports.submit(job, self.workflow.generate, "", False,
                                      PaidTaskAuthorization.confirmed(command, job.provider))
                except Exception:
                    job.state = job.phase = "stopped"
                    self.ports.workflow_ports.persist(job, required=True)
                    raise
            return job

    def reconcile(self, group_id: str, job_id: str) -> dict[str, Any]:
        """Repair only the local group link. Never queue work or create supplier tasks."""
        identity(job_id, "job_id")
        with self.repository.transaction(), self.ports.lock:
            group = self.repository.load(group_id)
            directory = self.repository.root.parent / job_id
            job = self.ports.jobs.get(job_id) or _load_job(directory)
            if job is None:
                if (directory / "job.json").exists():
                    raise RequestError("snapshot_unavailable", "The original model receipt cannot be read. Preserve it for recovery.", 409)
                self.repository.write_record(group_id, f"reconciled-submissions/model/{job_id}.json",
                                             {"id": job_id, "accepted": False})
                return {"accepted": False}
            if (job.creation_kind != CREATION_KIND or
                    job.approved_multiview_snapshot.get("group_id") != group_id):
                raise RequestError("idempotency_conflict", "The model job belongs to another source.", 409)
            self.ports.jobs[job_id] = job
            self._link(group, job)
            return {"accepted": True, "job": self.ports.present(job)}

    def stop(self, group_id: str, job_id: str) -> Job:
        identity(job_id, "job_id")
        with self.repository.transaction(), self.ports.lock:
            self.repository.load(group_id)
            job = self.ports.jobs.get(job_id)
            if not job or job not in self._group_jobs(group_id):
                raise RequestError("job_not_found", "The group's model task was not found.", 404)
            if job.state in {"queued", "running", "stopping"}:
                job.stop_event.set()
                job.state = job.phase = "stopping"
                job.message = "Stopping the advanced model task."
                self.ports.workflow_ports.persist(job, required=True)
            return job
    def resume(self, group_id: str, job_id: str) -> Job:
        identity(job_id, "job_id")
        with self.repository.transaction(), self.ports.lock:
            group = self.repository.load(group_id)
            job = self.ports.jobs.get(job_id)
            if not job or not any(link["job_id"] == job_id for link in group["model_links"]):
                raise RequestError("job_not_found", "The group's model task was not found.", 404)
            if job.state not in {"stopped", "failed"} or not any(a.get("generation_task_id") for a in job.attempts):
                raise RequestError("provider_task_reference_missing", "Only a saved remote model task can be resumed.", 409)
            if group["active_operation_id"] or self.busy(group_id):
                raise RequestError("group_busy", "Finish the active operation before resuming.", 409)
            job.stop_event.clear(); job.state, job.phase = "queued", "resuming"
            self.ports.workflow_ports.persist(job, required=True)
            try:
                self.ports.submit(job, self.workflow.generate, "", True, None)
            except Exception:
                job.state = job.phase = "stopped"
                self.ports.workflow_ports.persist(job, required=True)
                raise
            return job


def configured_model_capability(provider: str, model: str) -> dict[str, Any]:
    if provider != "tripo":
        return {"available": False, "named_four_view_model": "unknown", "model": model,
                "reason": "named_four_view_protocol_not_verified"}
    import tripo_client
    try:
        configured = tripo_client._config()[2]
        return {"available": not model or model == configured, "named_four_view_model": "supported", "model": configured,
                "verification": "adapter_contract; real acceptance pending"}
    except tripo_client.TripoError:
        return {"available": False, "named_four_view_model": "unknown", "model": model,
                "reason": "provider_not_configured"}
