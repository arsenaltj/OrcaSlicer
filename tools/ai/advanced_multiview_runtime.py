"""One advanced composition per model root; shared sidecar, isolated executor/state."""
from __future__ import annotations

from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import threading
from typing import Any

from advanced_multiview_application import AdvancedMultiviewApplication
from advanced_multiview_design_workflow import AdvancedMultiviewDesignWorkflow
from advanced_multiview_image_gateway import AdvancedMultiviewImageGateway
from advanced_multiview_repository import AdvancedMultiviewRepository
from advanced_multiview_transport import dispatch
from advanced_multiview_models import AdvancedMultiviewModels, AdvancedModelPorts, configured_model_capability
from model_contracts import RequestError


class AdvancedMultiviewRuntime:
    def __init__(self, root: Path, model_ports=None, image_provider_gateway=None):
        self.repository = AdvancedMultiviewRepository(root)
        self.gateway = AdvancedMultiviewImageGateway(image_provider_gateway)
        self.workflow = AdvancedMultiviewDesignWorkflow(
            self.repository, self.gateway.generate, self.gateway.recover)
        self.executor = ThreadPoolExecutor(max_workers=1, thread_name_prefix="advanced-design")
        self.closed = False
        self.application = AdvancedMultiviewApplication(self.repository, capabilities=self.gateway.capabilities,
                                                        submit=self.submit, run_design=self.workflow.run)
        self.models = AdvancedMultiviewModels(self.repository, model_ports) if model_ports else None
        if self.models:
            self.application.model_busy = self.models.busy
        # Only process-start composition recovers interrupted records; opening a live group never does.
        if self.repository.root.exists():
            for directory in self.repository.root.iterdir():
                if directory.is_dir():
                    try:
                        self.workflow.recover_interrupted(directory.name)
                    except RequestError:
                        pass  # Preserve corrupt/foreign records for explicit diagnosis.

    def submit(self, work) -> None:
        if self.closed:
            raise RequestError("executor_unavailable", "The advanced executor is shutting down.", 503)
        self.executor.submit(work)

    def capabilities(self) -> dict[str, Any]:
        return {"schema": "advanced-multiview-capabilities-v1", "protocol_version": 1,
                "model_plans": self.models is not None,
                "image_models": self.gateway.catalog_capabilities(),
                "model_providers": {p: self.models.ports.capability(p, "") if self.models else
                                    {"named_four_view_model": "unknown", "available": False}
                                    for p in ("tripo", "yoyoclaw")}}

    def snapshot(self, group_id: str) -> dict[str, Any]:
        result = self.repository.snapshot(group_id)
        result["execution_state"] = ""
        if result["active_operation_id"]:
            operation = self.application.operation(group_id, result["active_operation_id"])
            result["execution_state"] = operation["state"]
        result["model_jobs"] = []
        result["model_busy"] = False
        if self.models:
            with self.models.ports.lock:
                for link in result["model_links"]:
                    job = self.models.ports.jobs.get(link["job_id"])
                    if job:
                        result["model_jobs"].append(self.models.ports.present(job))
                        result["model_busy"] |= job.state in {"queued", "running", "stopping"}
        return result

    def recover(self, group_id: str, operation_id: str) -> dict[str, Any]:
        with self.repository.transaction():
            group = self.repository.load(group_id)
            operation = self.application.operation(group_id, operation_id)
            if (group["active_operation_id"] not in {None, operation_id}
                    or self.application.operation_running(operation_id)):
                raise RequestError("group_busy", "Wait for the active operation before recovering saved results.", 409)
            if operation["state"] != "recovery_required":
                return operation
            if any((group["views"][view].get("latest_attempt") or {}).get("operation_id") != operation_id
                   for view in operation["targets"]):
                raise RequestError("stale_result", "A newer generation superseded this operation; its receipts are preserved.", 409)
            group["active_operation_id"] = operation_id
            self.repository.save(group)
        # Known output download/local commit only, never a supplier creation request.
        self.workflow.recover_interrupted(group_id)
        return self.application.operation(group_id, operation_id)

    def reconcile(self, group_id: str, operation_id: str) -> dict[str, Any]:
        """Explicit desktop reconciliation never submits new supplier work."""
        from advanced_multiview_contracts import identity
        identity(operation_id, "operation_id")
        with self.repository.transaction():
            self.repository.load(group_id)
            receipt = self.repository.path(group_id, f"operations/{operation_id}/operation.json", must_exist=False)
            if not receipt.exists():
                # A delayed original POST cannot be accepted after this negative answer.
                self.repository.write_record(group_id, f"reconciled-submissions/design/{operation_id}.json",
                                             {"id": operation_id, "accepted": False})
                return {"accepted": False, "group": self.snapshot(group_id)}
            if not self.application.operation_running(operation_id):
                self.workflow.recover_interrupted(group_id)
            return {"accepted": True, "operation": self.application.operation(group_id, operation_id),
                    "group": self.snapshot(group_id)}

    def shutdown(self) -> None:
        if self.closed:
            return
        self.closed = True
        if self.repository.root.exists():
            for directory in self.repository.root.iterdir():
                if directory.is_dir():
                    try:
                        group = self.repository.load(directory.name)
                        if group["active_operation_id"]:
                            self.application.stop(group["id"], group["active_operation_id"])
                    except RequestError:
                        pass
        self.executor.shutdown(wait=True, cancel_futures=False)


_RUNTIMES: dict[Path, AdvancedMultiviewRuntime] = {}
_LOCK = threading.RLock()


def get_runtime(root: Path, model_ports=None, image_provider_gateway=None) -> AdvancedMultiviewRuntime:
    root = Path(root).resolve()
    with _LOCK:
        if root not in _RUNTIMES:
            _RUNTIMES[root] = AdvancedMultiviewRuntime(
                root, model_ports() if model_ports else None, image_provider_gateway)
        return _RUNTIMES[root]


def handle(handler, root, method: str, model_ports=None, image_provider_gateway=None) -> bool:
    return dispatch(handler, lambda: get_runtime(root(), model_ports, image_provider_gateway), method)


def compose_handler(root, jobs, lock, workflow, submit, present, image_provider_gateway=None):
    """Bind the advanced composition once; legacy generation retains its own worker."""
    def ports():
        return AdvancedModelPorts(jobs, lock, workflow().ports, submit, present, configured_model_capability)
    class AdvancedHandler:
        def handle_advanced(self, method):
            return handle(self, root, method, ports, image_provider_gateway)

        def do_PUT(self):
            if not self.handle_advanced("PUT"):
                self.send_json(404, {"error": "not found"})
    return AdvancedHandler


def shutdown() -> None:
    with _LOCK:
        for runtime in _RUNTIMES.values():
            runtime.shutdown()
