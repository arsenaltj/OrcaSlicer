"""Four named results, published independently against frozen edit bases."""
from __future__ import annotations

from copy import deepcopy
from dataclasses import dataclass
import hashlib
import logging
from pathlib import Path
import time
from typing import Any, Callable

from PIL import Image, ImageOps

from ai_diagnostics import event as diagnostic_event, redact_text
from advanced_multiview_contracts import VERSION_SCHEMA, VIEWS, new_id, view_contract_snapshot
from advanced_multiview_repository import AdvancedMultiviewRepository
from model_contracts import MIN_MODEL_REFERENCE_EDGE, RequestError
from model_provider_gateway import ProviderGatewayError
from model_request import _validate_image_file


@dataclass(frozen=True)
class DesignImageRequest:
    view: str
    initial: bool
    prompt: str
    style: str
    custom_style: str
    provider: str
    model: str
    attempt_id: str
    # Reference order is an explicit authority order, frozen by the advanced workflow.
    references: tuple[tuple[str, Path], ...]
    view_contract: dict[str, Any] | None = None


@dataclass(frozen=True)
class DesignImageResult:
    provider_request_id: str = ""
    remote_task_id: str = ""


class AdvancedMultiviewDesignWorkflow:
    def __init__(self, repository: AdvancedMultiviewRepository,
                 generate_image: Callable[[DesignImageRequest, Path], DesignImageResult],
                 recover_image: Callable[[Path], DesignImageResult | None] | None = None):
        self.repository = repository
        self.generate_image = generate_image
        self.recover_image = recover_image

    @staticmethod
    def _request(operation: dict[str, Any], view: str, references=()) -> DesignImageRequest:
        draft = operation["draft"]
        contract = operation["frozen"].get("view_contract") or view_contract_snapshot()
        initial = operation.get("target_modes", {}).get(view, "initial" if operation["initial"] else "edit") == "initial"
        return DesignImageRequest(view, initial, draft["prompt"], draft["style"],
                                  draft["custom_style"], draft["design_provider"], draft["design_model"],
                                  operation["attempts"][view]["id"], references,
                                  dict(contract["views"][view]))

    def _operation(self, group_id: str, operation_id: str) -> dict[str, Any]:
        return self.repository.read_record(group_id, f"operations/{operation_id}/operation.json")

    def _save(self, group_id: str, operation: dict[str, Any]) -> None:
        self.repository.write_record(group_id, f"operations/{operation['id']}/operation.json", operation)

    def _references(self, group_id: str, group: dict[str, Any], operation: dict[str, Any],
                    view: str) -> tuple[tuple[str, Path], ...]:
        frozen = operation["frozen"]
        references: list[tuple[str, Path]] = []

        def append(role: str, path: Path) -> None:
            if len(references) < 3 and all(existing != path for _, existing in references):
                references.append((role, path))

        def upload(name: str) -> None:
            record = frozen["uploads"].get(name)
            if not record:
                return
            original = self.repository.path(group_id, record["path"])
            normalized = self.repository.path(group_id, record["normalized_path"])
            if (hashlib.sha256(original.read_bytes()).hexdigest() != record["sha256"]
                    or hashlib.sha256(normalized.read_bytes()).hexdigest() != record["normalized_sha256"]):
                raise RequestError("snapshot_unavailable", "An uploaded reference changed.", 409)
            append(name + "_upload", normalized)

        def design(name: str, record: dict[str, Any] | None, role: str | None = None) -> None:
            if not record:
                return
            verified = self.repository.version(group_id, name, record["id"])
            append(role or name + "_design", self.repository.path(group_id, verified["path"]))

        if self._request(operation, view).initial:
            if frozen["uploads"].get(view):
                upload(view)
            elif view != "front":
                upload("front")
                anchor = frozen.get("initial_front_version_id") or frozen["context"].get("front", {}).get("id")
                if not anchor and operation["initial"]:  # Legacy initial/retry records.
                    anchor = group["views"]["front"]["current_version_id"]
                if anchor:
                    record = self.repository.version(group_id, "front", anchor)
                    append("front_design", self.repository.path(group_id, record["path"]))
                if not references:
                    raise RequestError("anchor_unavailable", "The main-reference design must succeed before an inferred view.", 409)
        else:
            context = frozen["context"]
            changed_uploads = set(frozen.get("changed_upload_views", ()))
            if view in changed_uploads:
                upload(view)
                design(view, context.get(view))
            else:
                design(view, context.get(view))
                upload(view)
            if view != "front" and "front" in changed_uploads:
                upload("front")
        return tuple(references)

    def _publish(self, group_id: str, operation_id: str, view: str,
                 request: DesignImageRequest, output: Path, result: DesignImageResult) -> None:
        try:
            info = _validate_image_file(output, minimum_edge=MIN_MODEL_REFERENCE_EDGE)
            if min(info.width, info.height) < MIN_MODEL_REFERENCE_EDGE:
                raise ValueError("The design image is too small for model input.")
        except ValueError as error:
            raise RequestError("invalid_provider_result", str(error), 409) from None
        version_id = new_id()
        relative = f"designs/{view}/{version_id}/image.png"
        destination = self.repository.path(group_id, relative, must_exist=False)
        destination.parent.mkdir(parents=True, exist_ok=True)
        with Image.open(output) as image:
            ImageOps.exif_transpose(image).convert("RGBA").save(destination)
        _validate_image_file(destination, minimum_edge=MIN_MODEL_REFERENCE_EDGE)
        record = {"schema": VERSION_SCHEMA, "id": version_id, "view": view, "path": relative,
                  "sha256": hashlib.sha256(destination.read_bytes()).hexdigest(),
                  "width": info.width, "height": info.height, "operation_id": operation_id,
                  "provider": request.provider, "model": request.model, "attempt_id": request.attempt_id,
                  "provider_request_id": result.provider_request_id, "remote_task_id": result.remote_task_id,
                  "created_at": time.time()}
        with self.repository.transaction():
            group = self.repository.load(group_id)
            operation = self._operation(group_id, operation_id)
            attempt = operation["attempts"][view]
            record["base_version_id"] = attempt["base_version_id"]
            self.repository.write_record(group_id, f"designs/{view}/{version_id}/version.json", record)
            attempt.update(version_id=version_id, provider_request_id=result.provider_request_id,
                           remote_task_id=result.remote_task_id)
            self._commit_candidate(group, operation, view, request.attempt_id)

    def _commit_candidate(self, group: dict[str, Any], operation: dict[str, Any],
                          view: str, attempt_id: str) -> None:
        group_id, operation_id = group["id"], operation["id"]
        attempt = operation["attempts"][view]
        version_id = attempt["version_id"]
        publication_base = operation.get("publication_bases", {}).get(view, attempt["base_version_id"])
        self.repository.version(group_id, view, version_id)
        if (group["active_operation_id"] != operation_id
                or group["input_epoch"] != operation["frozen"]["input_epoch"]
                or attempt["id"] != attempt_id
                or group["views"][view]["current_version_id"] not in {publication_base, version_id}):
            attempt.update(state="stopped", error={"code": "stale_result"})
            self._save(group_id, operation)
            return
        if group["stop_intent"] or operation["stop_intent"]:
            attempt.update(state="stopped", error={"code": "stopped_with_result"})
            self._save(group_id, operation)
            return
        # Persist the obtained candidate before switching the current pointer.
        attempt["state"] = "result_saved"
        self._save(group_id, operation)
        group["views"][view] = {"current_version_id": version_id,
                                "latest_attempt": {"operation_id": operation_id, "state": "succeeded",
                                                   "attempt_id": attempt["id"]}}
        group["pending_views"] = [item for item in group["pending_views"] if item != view]
        group["changed_upload_views"] = [item for item in group["changed_upload_views"] if item != view]
        self.repository.save(group)
        attempt.update(state="succeeded", error=None)
        if view == "front":
            operation["frozen"]["initial_front_version_id"] = version_id
        self._save(group_id, operation)

    def run(self, group_id: str, operation_id: str) -> None:
        try:
            for view in VIEWS:
                with self.repository.transaction():
                    group = self.repository.load(group_id)
                    operation = self._operation(group_id, operation_id)
                    if view not in operation["targets"]:
                        continue
                    attempt = operation["attempts"][view]
                    if attempt["state"] != "queued":
                        continue  # Never re-submit a saved submitting or completed attempt.
                    if (group["active_operation_id"] != operation_id or group["stop_intent"]
                            or operation["stop_intent"] or operation.get("halt_remaining")):
                        attempt["state"] = "stopped"
                        self._save(group_id, operation)
                        continue
                    if attempt["version_id"]:
                        # Explicit retry may accept a result obtained before Stop.
                        # No generation is needed for this immutable candidate.
                        self._commit_candidate(group, operation, view, attempt["id"])
                        continue
                    try:
                        references = self._references(group_id, group, operation, view)
                    except RequestError as error:
                        attempt.update(state="blocked" if error.code == "anchor_unavailable" else "failed",
                                       error={"code": error.code, "message": error.message})
                        self._save(group_id, operation)
                        group["views"][view]["latest_attempt"] = {
                            "operation_id": operation_id, "attempt_id": attempt["id"], "state": attempt["state"],
                            "error": attempt["error"]}
                        self.repository.save(group)
                        continue
                    request = self._request(operation, view, references)
                    if operation["new_image_calls"] >= operation["max_new_image_calls"]:
                        raise RequestError("authorization_consumed", "The confirmed image-call limit was reached.", 409)
                    attempt["state"] = "submitting"
                    attempt["staging_name"] = "result.png"
                    operation["state"] = "running"
                    operation["new_image_calls"] += 1
                    self._save(group_id, operation)
                    group["views"][view]["latest_attempt"] = {
                        "operation_id": operation_id, "attempt_id": attempt["id"], "state": "submitting"}
                    self.repository.save(group)
                    output = self.repository.path(group_id, f"staging/{attempt['id']}/result.png", must_exist=False)
                    output.parent.mkdir(parents=True, exist_ok=True)
                try:
                    started = time.monotonic()
                    result = self.generate_image(request, output)
                    self._publish(group_id, operation_id, view, request, output, result)
                except Exception as error:
                    if isinstance(error, RequestError) and error.code == "persistence_failed":
                        raise  # Recover the obtained output/receipt; never label disk failure as a supplier failure.
                    ambiguous = isinstance(error, ProviderGatewayError) and error.ambiguous
                    # Unexpected transport failures may have incurred a charge. Do not guess safe retry.
                    if not isinstance(error, (RequestError, ProviderGatewayError)):
                        ambiguous = True
                    elapsed_ms = max(0, round((time.monotonic() - started) * 1000))
                    gateway_error = error if isinstance(error, ProviderGatewayError) else None
                    provider_message = redact_text(gateway_error.provider_message).strip() if gateway_error else ""
                    request_id = gateway_error.request_id if gateway_error else ""
                    error_code = getattr(error, "code", "provider_result_unknown")
                    error_record = {
                        "code": error_code,
                        "message": provider_message or (
                            redact_text(error.message) if isinstance(error, RequestError) else "Image generation failed."
                        ),
                        "provider": request.provider,
                        "model": request.model,
                        "retryable": bool(gateway_error and gateway_error.retryable),
                        "ambiguous": ambiguous,
                        "elapsed_ms": elapsed_ms,
                    }
                    if gateway_error and isinstance(gateway_error.status, int):
                        error_record["http_status"] = gateway_error.status
                    if provider_message:
                        error_record["provider_message"] = provider_message
                    if request_id:
                        error_record["request_id"] = request_id
                    diagnostic_event(
                        "advanced_multiview.image.failed", level="WARNING",
                        operation_id=operation_id, attempt_id=request.attempt_id, view=view,
                        provider=request.provider, model=request.model, elapsed_ms=elapsed_ms,
                        error_code=error_code, category=getattr(error, "category", "local"),
                        http_status=gateway_error.status if gateway_error else None,
                        provider_message=provider_message, request_id=request_id,
                        retryable=bool(gateway_error and gateway_error.retryable), ambiguous=ambiguous,
                    )
                    with self.repository.transaction():
                        operation = self._operation(group_id, operation_id)
                        attempt = operation["attempts"][view]
                        if attempt["state"] == "result_saved" or attempt.get("version_id"):
                            raise  # Preserve the recoverable local result, not a new failed generation.
                        known = output.exists() or output.with_suffix(".response.json").exists()
                        attempt.update(state="result_pending" if known else "ambiguous" if ambiguous else "failed",
                                       error=error_record)
                        if ambiguous or known:
                            operation["halt_remaining"] = True
                        self._save(group_id, operation)
                        group = self.repository.load(group_id)
                        if group["active_operation_id"] == operation_id:
                            group["views"][view]["latest_attempt"] = {
                                "operation_id": operation_id, "attempt_id": attempt["id"],
                                "state": attempt["state"], "error": attempt["error"]}
                            self.repository.save(group)
            self._finish(group_id, operation_id)
        except Exception as error:
            # Persist only a safe code, never prompts, response bodies or private paths.
            code = error.code if isinstance(error, RequestError) else "local_result_commit_failed"
            logging.getLogger(__name__).error("Advanced operation %s requires recovery (%s)", operation_id, code)
            try:
                with self.repository.transaction():
                    operation = self._operation(group_id, operation_id)
                    operation.update(state="recovery_required", recovery_error={"code": code})
                    for attempt in operation["attempts"].values():
                        if attempt["state"] == "queued":
                            attempt["state"] = "stopped"
                    self._save(group_id, operation)
                    self._finish(group_id, operation_id)
            except Exception:
                logging.getLogger(__name__).error("Advanced recovery receipt could not be committed (%s)", operation_id)

    def _finish(self, group_id: str, operation_id: str) -> None:
        with self.repository.transaction():
            operation = self._operation(group_id, operation_id)
            states = {a["state"] for a in operation["attempts"].values()}
            operation["state"] = "succeeded" if states == {"succeeded"} else \
                                 "recovery_required" if states & {"ambiguous", "submitting", "result_saved", "result_pending"} else \
                                 "stopped" if operation["stop_intent"] else "partial"
            self._save(group_id, operation)
            group = self.repository.load(group_id)
            if group["active_operation_id"] == operation_id:
                for view, attempt in operation["attempts"].items():
                    group["views"][view]["latest_attempt"] = {
                        "operation_id": operation_id, "attempt_id": attempt["id"],
                        "state": attempt["state"], "error": attempt["error"]}
                group["active_operation_id"] = None
                self.repository.save(group)

    def recover_interrupted(self, group_id: str) -> None:
        """Startup/open recovery only commits known local results; it never calls a supplier."""
        with self.repository.transaction():
            group = self.repository.load(group_id)
            operation_id = group["active_operation_id"]
            if not operation_id:
                # A terminal local failure releases 2D eligibility. Known receipts
                # remain recoverable until a newer intent supersedes that target.
                operation_ids = {state["latest_attempt"]["operation_id"] for state in group["views"].values()
                                 if state.get("latest_attempt") and state["latest_attempt"]["state"] in
                                 {"submitting", "result_saved", "result_pending"}}
                if len(operation_ids) == 1:
                    candidate_id = operation_ids.pop()
                    candidate = self._operation(group_id, candidate_id)
                    if (candidate["frozen"]["input_epoch"] == group["input_epoch"] and all(
                            (group["views"][view].get("latest_attempt") or {}).get("operation_id") == candidate_id
                            for view in candidate["targets"])):
                        operation_id = candidate_id
                        group["active_operation_id"] = operation_id
                        group = self.repository.save(group)
            if not operation_id:
                # A crash between acceptance receipt and group commit cannot start
                # paid work. Preserve its receipt as explicitly stopped for retry.
                operations = self.repository.directory(group_id) / "operations"
                for path in operations.glob("*/operation.json"):
                    orphan = self.repository.read_json(path)
                    if orphan.get("state") == "queued" and all(
                            a.get("state") == "queued" for a in orphan.get("attempts", {}).values()):
                        orphan["state"] = "stopped"
                        for attempt in orphan["attempts"].values():
                            attempt["state"] = "stopped"
                        self._save(group_id, orphan)
                        if orphan["expected_group_revision"] == group["revision"]:
                            for view, attempt in orphan["attempts"].items():
                                group["views"][view]["latest_attempt"] = {
                                    "operation_id": orphan["id"], "attempt_id": attempt["id"],
                                    "state": "stopped", "error": None}
                            group["published"] = True
                            group["saved_draft"] = deepcopy(orphan["draft"])
                            group["latest_submitted_draft"] = deepcopy(orphan["draft"])
                            if orphan["kind"] == "initial":
                                group["initial_input"] = deepcopy(orphan["draft"])
                            group = self.repository.save(group)
                return
            operation = self._operation(group_id, operation_id)
            for view in list(operation["attempts"]):
                attempt = operation["attempts"][view]
                if attempt["state"] in {"submitting", "result_pending"}:
                    # The version receipt might have committed just before the
                    # operation write failed; reuse it instead of generating again.
                    versions = self.repository.directory(group_id) / "designs" / view
                    for path in versions.glob("*/version.json"):
                        record = self.repository.read_json(path)
                        if record.get("operation_id") == operation_id and record.get("attempt_id") == attempt["id"]:
                            self.repository.version(group_id, view, record["id"])
                            attempt.update(state="result_saved", version_id=record["id"])
                            break
                    if attempt["state"] != "result_saved":
                        output = self.repository.path(
                            group_id, f"staging/{attempt['id']}/{attempt.get('staging_name', 'result.png')}",
                            must_exist=False)
                        result = None
                        if self.recover_image:
                            try:
                                result = self.recover_image(output)
                            except (RequestError, ProviderGatewayError):
                                attempt["state"] = "result_pending"
                        elif output.exists():
                            result = DesignImageResult()
                        if result is not None:
                            try:
                                self._publish(group_id, operation_id, view, self._request(operation, view), output, result)
                            except RequestError as error:
                                if error.code == "persistence_failed":
                                    raise
                                # A corrupt obtained image remains a failed result,
                                # never a reason to retain a phantom active worker.
                                attempt.update(state="result_pending", error={"code": error.code,
                                               "message": redact_text(error.message)})
                            else:
                                group = self.repository.load(group_id)
                                operation = self._operation(group_id, operation_id)
                                attempt = operation["attempts"][view]
                if attempt["state"] == "result_saved" and attempt["version_id"]:
                    record = self.repository.version(group_id, view, attempt["version_id"])
                    publication_base = operation.get("publication_bases", {}).get(view, attempt["base_version_id"])
                    if (group["views"][view]["current_version_id"] in {publication_base, record["id"]}
                            and not group["stop_intent"] and not operation["stop_intent"]):
                        group["views"][view] = {"current_version_id": record["id"], "latest_attempt": {
                            "operation_id": operation_id, "attempt_id": attempt["id"], "state": "succeeded"}}
                        attempt["state"] = "succeeded"
                        if view == "front":
                            operation["frozen"]["initial_front_version_id"] = record["id"]
                    else:
                        attempt.update(state="stopped", error={"code": "stopped_with_result"})
                elif attempt["state"] == "submitting":
                    attempt.update(state="ambiguous", error={"code": "provider_task_ambiguous"})
                    group["views"][view]["latest_attempt"] = {
                        "operation_id": operation_id, "attempt_id": attempt["id"], "state": "ambiguous"}
                elif attempt["state"] == "queued":
                    attempt["state"] = "stopped"
                if attempt["state"] == "result_pending":
                    group["views"][view]["latest_attempt"] = {
                        "operation_id": operation_id, "attempt_id": attempt["id"], "state": "result_pending"}
            self._save(group_id, operation)
            self.repository.save(group)
            self._finish(group_id, operation_id)
