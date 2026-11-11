"""Confirmed advanced 2D commands; no dependency on widgets, HTTP or a vendor."""
from __future__ import annotations

from copy import deepcopy
import time
from typing import Any, Callable

from advanced_multiview_contracts import (
    OPERATION_SCHEMA, PLAN_SCHEMA, VIEWS,
    check_revision, display_name, draft_fields, fingerprint, identity, new_id, targets, view_contract_snapshot,
)
from advanced_multiview_repository import AdvancedMultiviewRepository
from model_contracts import RequestError


class AdvancedMultiviewApplication:
    def __init__(self, repository: AdvancedMultiviewRepository, *,
                 capabilities: Callable[[str, str], dict[str, Any]],
                 submit: Callable[[Callable[[], None]], None],
                 run_design: Callable[[str, str], None]):
        self.repository = repository
        self.capabilities = capabilities
        self.submit = submit
        self.run_design = run_design
        self._scheduled: set[str] = set()
        self.model_busy: Callable[[str], bool] = lambda _group: False

    def operation_running(self, operation_id: str) -> bool:
        with self.repository.transaction():
            return operation_id in self._scheduled

    def _enqueue(self, group_id: str, operation_id: str) -> None:
        def work():
            try:
                self.run_design(group_id, operation_id)
            finally:
                with self.repository.transaction():
                    self._scheduled.discard(operation_id)

        self._scheduled.add(operation_id)
        try:
            self.submit(work)
        except Exception:
            self._scheduled.discard(operation_id)
            operation = self.operation(group_id, operation_id)
            operation["state"] = "recovery_required"
            self.repository.write_record(group_id, f"operations/{operation_id}/operation.json", operation)
            # Keep the accepted group attached: explicit recovery stops its queued
            # attempts without spending any image calls.
            raise RequestError("executor_unavailable", "The accepted operation could not start; recover it explicitly.", 503) from None

    def save_draft(self, group_id: str, request: dict[str, Any]) -> dict[str, Any]:
        draft = draft_fields(request.get("draft"))
        with self.repository.transaction():
            group = self.repository.load(group_id)
            check_revision(group, request.get("expected_group_revision"))
            if group["active_operation_id"] or self.model_busy(group_id):
                raise RequestError("group_busy", "Finish the active operation before changing its draft.", 409)
            style_changed = (draft["style"], draft["custom_style"]) != (
                group["saved_draft"]["style"], group["saved_draft"]["custom_style"])
            group["saved_draft"] = draft
            if style_changed:
                group["input_epoch"] = new_id()
                group["pending_views"] = list(VIEWS)
            return self.repository.save(group)

    def update_inputs(self, group_id: str, request: dict[str, Any],
                      images: dict[str, bytes]) -> dict[str, Any]:
        draft = draft_fields(request)
        with self.repository.transaction():
            group = self.repository.load(group_id)
            check_revision(group, request.get("expected_group_revision"))
            if group["active_operation_id"] or self.model_busy(group_id):
                raise RequestError("group_busy", "Finish the active operation before changing its inputs.", 409)
            return self.repository.replace_inputs(group, draft, images)

    def rename_group(self, group_id: str, request: dict[str, Any]) -> dict[str, Any]:
        name = display_name(request.get("display_name", ""))
        with self.repository.transaction():
            group = self.repository.load(group_id)
            check_revision(group, request.get("expected_group_revision"))
            if group["active_operation_id"] or self.model_busy(group_id):
                raise RequestError("group_busy", "Finish the active operation before renaming the group.", 409)
            group["display_name"] = name
            return self.repository.save(group)

    def prepare(self, group_id: str, request: dict[str, Any]) -> dict[str, Any]:
        command_id = identity(request.get("command_id"), "command_id")
        kind = request.get("kind")
        if kind not in {"initial", "edit", "retry"}:
            raise RequestError("invalid_request", "An initial, edit or retry operation is required.", 400)
        with self.repository.transaction():
            group = self.repository.load(group_id)
            check_revision(group, request.get("expected_group_revision"))
            if group["active_operation_id"] or self.model_busy(group_id):
                raise RequestError("group_busy", "The group already has an active operation.", 409)
            retry_uses_current_input = False
            if kind == "initial":
                if group["published"]:
                    raise RequestError("invalid_group_state", "This group already has an initial operation.", 409)
                draft = deepcopy(group["saved_draft"])
                selected = VIEWS
                bases = dict.fromkeys(VIEWS)
                retry_of = None
                initial = True
                reusable = {}
            elif kind == "edit":
                draft = draft_fields(request)
                requested = targets(request.get("target_views"))
                selected = tuple(view for view in VIEWS
                                 if view in requested or view in group["pending_views"])
                supplied_bases = request.get("base_versions")
                if (not isinstance(supplied_bases, dict)
                        or any(view not in supplied_bases for view in requested)
                        or any(view not in VIEWS for view in supplied_bases)):
                    raise RequestError("revision_conflict", "Explicit visible base versions are required.", 409)
                # A desktop cache failure may leave an older, verified image visible.
                # Pin that immutable base; publication compares the canonical pointer
                # captured separately below, never an unseen image chosen by the server.
                bases = {v: supplied_bases.get(v, group["views"][v]["current_version_id"]) or None
                         for v in selected}
                for view, version_id in supplied_bases.items():
                    if version_id:
                        self.repository.version(group_id, view, version_id)
                retry_of = None
                initial = False
                reusable = {}
            else:
                retry_of = identity(request.get("operation_id"), "operation_id")
                old = self.operation(group_id, retry_of)
                selected = targets(request.get("target_views"), default_all=False)
                if not selected or any(v not in old["attempts"] for v in selected):
                    raise RequestError("invalid_request", "Explicit failed targets are required for retry.", 400)
                for view in selected:
                    attempt = old["attempts"][view]
                    if attempt["state"] in {"ambiguous", "result_pending", "result_saved"}:
                        raise RequestError("provider_task_ambiguous", "Resolve the original submission before retrying.", 409)
                    if attempt["state"] not in {"failed", "stopped", "blocked", "queued"}:
                        raise RequestError("invalid_request", "Only unsuccessful targets can be retried.", 409)
                initial = old["initial"]
                retry_uses_current_input = initial and group["input_epoch"] != old["frozen"]["input_epoch"]
                draft = deepcopy(group["saved_draft"] if retry_uses_current_input else old["draft"])
                if retry_uses_current_input:
                    # A failed-card retry may use newly supplied source content,
                    # but it remains bound to the original paid provider/model.
                    draft["design_provider"] = old["draft"]["design_provider"]
                    draft["design_model"] = old["draft"]["design_model"]
                if initial and "front" in selected and not group["views"]["front"]["current_version_id"]:
                    selected = tuple(v for v in VIEWS if not group["views"][v]["current_version_id"])
                bases = {v: old["attempts"][v]["base_version_id"] for v in selected}
                if any(group["views"][v]["current_version_id"] != bases[v] for v in selected):
                    raise RequestError("revision_conflict", "A retry base changed; prepare a new edit instead.", 409)
                reusable = {}
                for view in selected:
                    candidate = None if retry_uses_current_input else old["attempts"][view].get("version_id")
                    if candidate:
                        record = self.repository.version(group_id, view, candidate)
                        if record["base_version_id"] != bases[view]:
                            raise RequestError("snapshot_unavailable", "The saved candidate has a different edit base.", 409)
                        reusable[view] = candidate
            context = {}
            for view in VIEWS:
                version_id = group["views"][view]["current_version_id"]
                if kind == "edit":
                    version_id = supplied_bases.get(view, version_id) or None
                if version_id:
                    context[view] = self.repository.version(group_id, view, version_id)
            dependencies = []
            if kind == "edit" and not group["uploads"].get("front") and not context.get("front"):
                if any(not bases[v] and v != "front" and not group["uploads"].get(v) for v in selected):
                    if "front" not in selected:
                        dependencies.append("front")
                    selected = tuple(v for v in VIEWS if v == "front" or v in selected)
                    bases["front"] = None
            modes = {v: "initial" if initial or not bases[v] else "edit" for v in selected}
            capability = self.capabilities(draft["design_provider"], draft["design_model"])
            self._check_capability(capability, modes.values(), group["source"])
            if capability.get("model"):
                draft["design_model"] = capability["model"]
            frozen = {"input_epoch": group["input_epoch"], "uploads": deepcopy(group["uploads"]),
                      "source": group["source"], "context": context,
                      "pending_views": list(group["pending_views"]),
                      "changed_upload_views": list(group["changed_upload_views"]),
                      "view_contract": view_contract_snapshot()}
            if kind == "retry" and not retry_uses_current_input:
                frozen = deepcopy(old["frozen"])
                if initial:
                    frozen["initial_front_version_id"] = old["attempts"].get("front", {}).get("version_id") or \
                        frozen.get("initial_front_version_id")
            plan = {"schema": PLAN_SCHEMA, "id": command_id, "group_id": group_id,
                    "kind": kind, "initial": initial, "retry_of": retry_of,
                    "expected_group_revision": group["revision"], "targets": list(selected),
                    "base_versions": bases, "draft": draft, "frozen": frozen,
                    "target_modes": modes, "dependency_views": dependencies,
                    "publication_bases": {v: group["views"][v]["current_version_id"] for v in selected},
                    "reusable_versions": reusable,
                    "max_new_image_calls": len(selected) - len(reusable), "max_new_model_tasks": 0}
            content_hash = fingerprint(plan)
            relative = f"plans/{command_id}.json"
            path = self.repository.path(group_id, relative, must_exist=False)
            if path.exists():
                previous = self.repository.read_record(group_id, relative)
                if previous["confirmation_fingerprint"] != content_hash:
                    raise RequestError("idempotency_conflict", "The command ID belongs to a different plan.", 409)
                return previous
            plan["confirmation_fingerprint"] = content_hash
            plan["expires_at"] = time.time() + 600
            self.repository.write_record(group_id, relative, plan)
            return plan

    @staticmethod
    def _check_capability(capability, modes, source):
        required = {"edit_current_with_context" if mode == "edit" else
                    "initial_text" if source == "text" else "initial_named_references" for mode in modes}
        if any(capability.get(key) != "supported" for key in required):
            raise RequestError("unsupported_multiview", "The selected image model does not support this operation.", 503)

    def operation(self, group_id: str, operation_id: str) -> dict[str, Any]:
        identity(operation_id, "operation_id")
        result = self.repository.read_record(group_id, f"operations/{operation_id}/operation.json")
        if result.get("schema") != OPERATION_SCHEMA or result.get("id") != operation_id or result.get("group_id") != group_id:
            raise RequestError("snapshot_unavailable", "The operation identity is invalid.", 409)
        return result

    def execute(self, group_id: str, request: dict[str, Any]) -> dict[str, Any]:
        command_id = identity(request.get("command_id"), "command_id")
        if request.get("plan_id") != command_id:
            raise RequestError("invalid_request", "The command and plan IDs must match.", 400)
        with self.repository.transaction():
            plan = self.repository.read_record(group_id, f"plans/{command_id}.json")
            if plan.get("confirmation_fingerprint") != request.get("confirmation_fingerprint"):
                raise RequestError("stale_confirmation", "Confirm the current plan before generating.", 409)
            if self.repository.path(group_id, f"reconciled-submissions/design/{command_id}.json", must_exist=False).exists():
                raise RequestError("submission_not_accepted", "This request was reconciled as not accepted. Confirm a new plan.", 409)
            relative = f"operations/{command_id}/operation.json"
            group = self.repository.load(group_id)
            stopped = self.repository.path(group_id, f"stopped-submissions/{command_id}.json", must_exist=False).exists()
            existing = self.repository.path(group_id, relative, must_exist=False).exists()
            if existing:
                operation = self.operation(group_id, command_id)
                if operation["state"] != "queued" or command_id in self._scheduled:
                    return operation
                if group["active_operation_id"] == command_id:
                    self._enqueue(group_id, command_id)
                    return operation
                # The operation is the durable acceptance receipt. Reattach only when
                # the group commit failed before changing the confirmed revision.
                if group["revision"] != plan["expected_group_revision"] or group["active_operation_id"]:
                    raise RequestError("revision_conflict", "Recover the interrupted operation before generating.", 409)
            if time.time() > plan["expires_at"]:
                raise RequestError("stale_confirmation", "The generation plan expired.", 409)
            check_revision(group, plan["expected_group_revision"])
            if group["active_operation_id"] or self.model_busy(group_id):
                raise RequestError("group_busy", "The group already has an active operation.", 409)
            if group["input_epoch"] != plan["frozen"]["input_epoch"]:
                raise RequestError("stale_confirmation", "The creation input changed.", 409)
            capability = self.capabilities(plan["draft"]["design_provider"], plan["draft"]["design_model"])
            self._check_capability(capability, plan.get("target_modes", {v: "initial" if plan["initial"] else "edit"
                                                                       for v in plan["targets"]}).values(), group["source"])
            operation = {**deepcopy(plan), "schema": OPERATION_SCHEMA, "state": "stopped" if stopped else "queued",
                         "new_image_calls": 0, "stop_intent": stopped, "created_at": time.time(),
                         "attempts": {v: {"id": new_id(), "view": v, "base_version_id": plan["base_versions"][v],
                                          "state": "stopped" if stopped else "queued", "error": None,
                                          "version_id": plan.get("reusable_versions", {}).get(v),
                                          "remote_task_id": "", "provider_request_id": ""} for v in plan["targets"]}}
            self.repository.write_record(group_id, relative, operation)
            if stopped:
                return operation  # Stop won the acceptance race: retain receipt, spend zero calls.
            group["active_operation_id"] = command_id
            group["published"] = True
            group["stop_intent"] = False
            group["saved_draft"] = deepcopy(plan["draft"])
            group["latest_submitted_draft"] = deepcopy(plan["draft"])
            for view, attempt in operation["attempts"].items():
                group["views"][view]["latest_attempt"] = {"operation_id": command_id,
                    "attempt_id": attempt["id"], "state": "queued", "error": None}
            if plan["kind"] == "initial":
                group["initial_input"] = deepcopy(plan["draft"])
            self.repository.save(group)
            self._enqueue(group_id, command_id)
        return operation

    def stop(self, group_id: str, operation_id: str) -> dict[str, Any]:
        identity(operation_id, "operation_id")
        with self.repository.transaction():
            group = self.repository.load(group_id)
            # Durable even when the original POST has not reached execute yet.
            self.repository.write_record(group_id, f"stopped-submissions/{operation_id}.json",
                                         {"id": operation_id, "stop_intent": True})
            if not self.repository.path(group_id, f"operations/{operation_id}/operation.json", must_exist=False).exists():
                return {"id": operation_id, "state": "stopped", "stop_intent": True}
            operation = self.operation(group_id, operation_id)
            if group["active_operation_id"] != operation_id:
                return operation
            operation["stop_intent"] = True
            group["stop_intent"] = True
            self.repository.write_record(group_id, f"operations/{operation_id}/operation.json", operation)
            self.repository.save(group)
            return operation
