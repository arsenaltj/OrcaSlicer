"""Offline contract and failure tests. Supplier ports are controlled; no real generation."""
from copy import deepcopy
from concurrent.futures import ThreadPoolExecutor
import hashlib
from io import BytesIO, StringIO
from itertools import combinations
import json
from pathlib import Path
import tempfile
import threading
import unittest
from unittest import mock

from PIL import Image

from advanced_multiview_application import AdvancedMultiviewApplication
from advanced_multiview_contracts import VIEWS, new_id, targets
from advanced_multiview_design_workflow import AdvancedMultiviewDesignWorkflow, DesignImageResult
from advanced_multiview_repository import AdvancedMultiviewRepository
from advanced_multiview_transport import parse_upload, dispatch, PREFIX
from model_contracts import RequestError
from model_provider_gateway import ProviderGatewayError


QWEN_IMAGE = "image-model-library/Qwen-Image-3.0-PC-YOYO"
HY_IMAGE = "image-model-library/Hy-Image-3.0-PC-YOYO"
DOUBAO_IMAGE = "image-model-library/Doubao-Seedream-5.0-Pro-PC-YOYO"


class _FakeYoYoImageGateway:
    def __init__(self):
        self.models = [QWEN_IMAGE, HY_IMAGE, DOUBAO_IMAGE]
        self.generate_calls = []
        self.download_calls = []
        self.download_error = None

    def catalog(self, role, *, fresh=False):
        return {"role": role, "provider": "yoyoclaw", "state": "ready",
                "default_model": QWEN_IMAGE,
                "models": [{"id": model, "name": model.rsplit("/", 1)[-1], "available": True}
                           for model in self.models], "error": None}

    def generate_design_response(self, model, prompt, image_paths=()):
        self.generate_calls.append((model, prompt, tuple(image_paths)))
        return {"requestId": "yoyo-request-1", "data": [{"url": "https://example.invalid/image.png"}]}

    def download_design_response(self, response, output_path):
        self.download_calls.append((response, Path(output_path)))
        if self.download_error:
            raise self.download_error
        Path(output_path).write_bytes(png("teal"))
        return Path(output_path)


def png(color="navy", size=256):
    output = BytesIO()
    Image.new("RGB", (size, size), color).save(output, format="PNG")
    return output.getvalue()


class MultiviewTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.repository = AdvancedMultiviewRepository(Path(self.temporary.name))
        self.requests = []
        self.queue = []
        self.failure = {}
        self.after_image = None

        def generate(request, destination):
            self.requests.append(request)
            if request.view in self.failure:
                raise self.failure[request.view]
            destination.write_bytes(png(["red", "green", "blue", "yellow"][VIEWS.index(request.view)]))
            if self.after_image:
                self.after_image(request)
            return DesignImageResult(provider_request_id=request.attempt_id)

        self.workflow = AdvancedMultiviewDesignWorkflow(self.repository, generate)
        self.capability = {"initial_text": "supported", "initial_named_references": "supported",
                           "edit_current_with_context": "supported"}
        self.application = AdvancedMultiviewApplication(
            self.repository, capabilities=lambda *_: self.capability,
            submit=self.queue.append, run_design=self.workflow.run)

    def create(self, images=None, **fields):
        return self.repository.create(new_id(), {"prompt": "One object", **fields}, images or {})

    def plan(self, group, kind="initial", **fields):
        return self.application.prepare(group["id"], {"command_id": new_id(), "kind": kind,
                                        "expected_group_revision": group["revision"], **fields})

    def execute(self, plan):
        return self.application.execute(plan["group_id"], {"command_id": plan["id"], "plan_id": plan["id"],
                                         "confirmation_fingerprint": plan["confirmation_fingerprint"]})

    def drain(self):
        while self.queue:
            self.queue.pop(0)()

    def initial(self, images=None):
        group = self.create(images)
        plan = self.plan(group)
        self.execute(plan)
        self.drain()
        return self.repository.load(group["id"]), plan

    def scoped_plan(self, group, view=None, local_prompt="", **fields):
        intent = {"version": 1, "scope": "all_inputs"} if view is None else {
            "version": 1, "scope": "single_view", "target_view": view, "local_prompt": local_prompt}
        return self.plan(group, "edit", **{**group["saved_draft"],
            "design_request": intent, "target_views": [view] if view else ["front"],
            "base_versions": {v: group["views"][v]["current_version_id"] for v in VIEWS}, **fields})

    def test_full_input_scope_ignores_old_repaint_selection_for_text_and_image_subsets(self):
        subsets = [None] + [
            {view: png(("navy", "red", "green", "yellow")[i]) for i, view in enumerate(VIEWS)
             if i == 0 or mask & (1 << (i - 1))} for mask in range(8)]
        for provider in ("gpt", "yoyoclaw"):
            for images in subsets:
                with self.subTest(provider=provider, images=list(images or {})):
                    group = self.create(images, design_provider=provider,
                                        design_model=QWEN_IMAGE if provider == "yoyoclaw" else "")
                    self.execute(self.plan(group)); self.drain()
                    group = self.repository.load(group["id"])
                    plan = self.scoped_plan(group)
                    expected = list(images) if images else list(VIEWS)
                    self.assertEqual(plan["targets"], expected)
                    self.requests.clear()
                    self.execute(plan); self.drain()
                    self.assertEqual([r.view for r in self.requests], expected)

    def test_local_edits_preserve_global_baseline_other_views_and_pending_work_for_both_image_providers(self):
        for provider in ("gpt", "yoyoclaw"):
            for view in VIEWS:
                with self.subTest(provider=provider, view=view):
                    group = self.create(design_provider=provider,
                                        design_model=QWEN_IMAGE if provider == "yoyoclaw" else "")
                    self.execute(self.plan(group)); self.drain()
                    group = self.repository.load(group["id"])
                    group = self.application.save_draft(group["id"], {
                        "expected_group_revision": group["revision"],
                        "draft": {**group["saved_draft"], "style": "cartoon", "prompt": "global blue"}})
                    before = deepcopy(group)
                    plan = self.scoped_plan(group, view, "local red")
                    self.assertEqual(plan["targets"], [view])
                    self.assertEqual(plan["dependency_views"], [])
                    self.assertEqual(plan["max_new_image_calls"], 1)
                    self.requests.clear()
                    self.execute(plan); self.drain()
                    current = self.repository.load(group["id"])
                    self.assertEqual(len(self.requests), 1)
                    request = self.requests[0]
                    self.assertEqual(request.view, view)
                    self.assertEqual(request.prompt, "global blue")
                    self.assertEqual(request.local_prompt, "local red")
                    self.assertEqual(request.references[0][0], view + "_design")
                    self.assertEqual(request.style, "cartoon")
                    for field in ("saved_draft", "latest_submitted_draft", "initial_input"):
                        self.assertEqual(current[field], before[field])
                    self.assertEqual(current["pending_views"], [v for v in VIEWS if v != view])
                    for other in VIEWS:
                        if other != view:
                            self.assertEqual(current["views"][other], before["views"][other])
                    operation = self.application.operation(group["id"], plan["id"])
                    self.assertEqual(operation["design_request"], plan["design_request"])
                    version = self.repository.version(group["id"], view, current["views"][view]["current_version_id"])
                    self.assertEqual(version["operation_id"], operation["id"])

    def test_empty_local_description_is_a_single_regeneration_and_does_not_rewrite_global_draft(self):
        group, _ = self.initial()
        before = deepcopy(group)
        plan = self.scoped_plan(group, "right", "")
        self.requests.clear(); self.execute(plan); self.drain()
        self.assertEqual([r.view for r in self.requests], ["right"])
        self.assertEqual(self.requests[0].local_prompt, "")
        self.assertEqual(self.repository.load(group["id"])["latest_submitted_draft"], before["latest_submitted_draft"])

    def test_local_plan_echo_preserves_whitespace_while_blank_requirements_remain_a_regeneration(self):
        group, _ = self.initial()
        for local in ("  a local change\n", " \n "):
            with self.subTest(local=local):
                plan = self.scoped_plan(group, "front", local)
                self.assertEqual(plan["design_request"]["local_prompt"], local)
                self.requests.clear(); self.execute(plan); self.drain()
                self.assertEqual(len(self.requests), 1)
                self.assertEqual(self.requests[0].local_prompt, local)
                group = self.repository.load(group["id"])
        with self.assertRaises(RequestError):
            self.scoped_plan(group, "front", " " * 2001)

    def test_local_description_has_its_own_utf8_byte_limit(self):
        group, _ = self.initial()
        for text in ("a" * 2000, "字" * 666 + "ab", "😀" * 500):
            with self.subTest(bytes=len(text.encode("utf-8"))):
                self.assertEqual(self.scoped_plan(group, "front", text)["design_request"]["local_prompt"], text)
                with self.assertRaises(RequestError):
                    self.scoped_plan(group, "front", text + "a")
        self.assertEqual(self.queue, [])

    def test_orphan_local_acceptance_recovery_preserves_global_baseline_without_buying_a_new_image(self):
        for provider in ("gpt", "yoyoclaw"):
            with self.subTest(provider=provider):
                group = self.create(design_provider=provider,
                                    design_model=QWEN_IMAGE if provider == "yoyoclaw" else "")
                self.execute(self.plan(group)); self.drain()
                group = self.repository.load(group["id"])
                group = self.application.save_draft(group["id"], {
                    "expected_group_revision": group["revision"],
                    "draft": {**group["saved_draft"], "prompt": "unsubmitted global description"}})
                before = deepcopy(group)
                plan = self.scoped_plan(group, "right", "only this local description")
                self.requests.clear()
                with mock.patch.object(self.repository, "save", side_effect=RequestError("persistence_failed", "full", 503)):
                    with self.assertRaises(RequestError):
                        self.execute(plan)
                self.assertEqual(self.queue, [])
                self.workflow.recover_interrupted(group["id"], expected_operation=plan["id"])
                restored = self.repository.load(group["id"])
                for field in ("saved_draft", "latest_submitted_draft", "initial_input", "pending_views"):
                    self.assertEqual(restored[field], before[field])
                self.assertEqual(self.requests, [])
                self.assertIsNone(restored["active_operation_id"])
                operation = self.application.operation(group["id"], plan["id"])
                self.assertEqual(operation["state"], "stopped")
                self.assertEqual(list(operation["attempts"]), ["right"])
                self.assertEqual(operation["design_request"], plan["design_request"])
                for view in VIEWS:
                    self.assertEqual(restored["views"][view]["current_version_id"], before["views"][view]["current_version_id"])

    def test_invalid_design_scopes_and_local_target_mismatches_never_schedule_generation(self):
        group, _ = self.initial()
        for intent in (None, {}, {"version": True, "scope": "all_inputs"},
                       {"version": 2, "scope": "all_inputs"}, {"version": 1, "scope": []},
                       {"version": 1, "scope": "all_inputs", "local_prompt": ""},
                       {"version": 1, "scope": "single_view", "target_view": "top", "local_prompt": ""},
                       {"version": 1, "scope": "single_view", "target_view": "left", "local_prompt": 1}):
            with self.subTest(intent=intent), self.assertRaises(RequestError):
                self.scoped_plan(group, "left", design_request=intent)
        for selected in ([], ["front"], ["left", "back"], ["left", "left"]):
            with self.subTest(selected=selected), self.assertRaises(RequestError):
                self.scoped_plan(group, "left", target_views=selected)
        self.assertEqual(self.queue, [])

    def test_local_generation_requires_initial_acceptance_card_state_and_resolved_receipts(self):
        group = self.create()
        with self.assertRaises(RequestError):
            self.scoped_plan(group, "front")
        group, _ = self.initial({"front": png(), "right": png("red")})
        with self.assertRaises(RequestError) as error:
            self.scoped_plan(group, "left")
        self.assertEqual(error.exception.code, "missing_current_view")
        def preserve_obtained_result(_request, output):
            output.write_bytes(png("navy"))
            raise ProviderGatewayError("interrupted", code="timeout", category="availability", ambiguous=True)

        self.workflow.generate_image = preserve_obtained_result
        self.execute(self.scoped_plan(group, "right")); self.drain()
        group = self.repository.load(group["id"])
        self.assertEqual(group["views"]["right"]["latest_attempt"]["state"], "result_pending")
        with self.assertRaises(RequestError) as error:
            self.scoped_plan(group, "front")
        self.assertEqual(error.exception.code, "provider_task_ambiguous")
        with self.assertRaises(RequestError) as error:
            self.scoped_plan(group)
        self.assertEqual(error.exception.code, "provider_task_ambiguous")

    def test_local_edits_and_failed_cards_can_use_a_reference_outside_the_bulk_target_set(self):
        self.failure["left"] = ProviderGatewayError("rejected", code="rejected", category="request")
        group, _ = self.initial()
        self.failure.clear()
        group = self.application.update_inputs(group["id"], {
            **group["saved_draft"], "expected_group_revision": group["revision"],
        }, {"front": png("purple")})
        for target, role in (("right", "right_design"), ("left", "front_upload")):
            with self.subTest(target=target):
                self.assertIsNone(group["uploads"][target])
                self.assertEqual(self.scoped_plan(group)["targets"], ["front"])
                plan = self.scoped_plan(group, target, "Improve only this direction")
                self.assertEqual(plan["targets"], [target])
                self.assertEqual(plan["dependency_views"], [])
                self.assertEqual(plan["max_new_image_calls"], 1)
                before = group
                self.requests.clear()
                self.execute(plan); self.drain()
                self.assertEqual([request.view for request in self.requests], [target])
                self.assertEqual(self.requests[0].references[0][0], role)
                group = self.repository.load(group["id"])
                self.assertNotEqual(group["views"][target]["current_version_id"], before["views"][target]["current_version_id"])
                for view in VIEWS:
                    if view != target:
                        self.assertEqual(group["views"][view], before["views"][view])
                self.assertEqual(group["pending_views"], before["pending_views"])
                self.assertEqual(group["latest_submitted_draft"], before["latest_submitted_draft"])

    def test_failed_text_card_can_retry_alone_with_no_main_anchor(self):
        self.failure["front"] = ProviderGatewayError("rejected", code="rejected", category="request")
        group, _ = self.initial()
        self.failure.clear()
        self.assertEqual(self.scoped_plan(group, "left")["targets"], ["left"])
        group = self.application.save_draft(group["id"], {
            "expected_group_revision": group["revision"], "draft": {**group["saved_draft"], "prompt": ""},
        })
        with self.assertRaises(RequestError) as error:
            self.scoped_plan(group, "left")
        self.assertEqual(error.exception.code, "empty_input")
        plan = self.scoped_plan(group, "left", "Render the left profile")
        self.assertEqual(plan["dependency_views"], [])
        self.requests.clear(); self.execute(plan); self.drain()
        self.assertEqual([r.view for r in self.requests], ["left"])
        self.assertTrue(self.requests[0].initial)
        self.assertEqual(self.requests[0].references, ())
        group = self.repository.load(group["id"])
        self.assertIsNone(group["views"]["front"]["current_version_id"])
        self.assertIsNone(group["views"]["back"]["current_version_id"])
        # Once a design exists, its base alone is sufficient for a one-view edit.
        plan = self.scoped_plan(group, "left")
        self.requests.clear(); self.execute(plan); self.drain()
        self.assertEqual([r.view for r in self.requests], ["left"])
        self.assertEqual(self.requests[0].references[0][0], "left_design")

    def test_local_retry_inherits_scope_and_text_and_never_merges_other_pending_targets(self):
        group, _ = self.initial()
        group = self.application.save_draft(group["id"], {"expected_group_revision": group["revision"],
            "draft": {**group["saved_draft"], "style": "cartoon"}})
        self.failure["front"] = ProviderGatewayError("rejected", code="rejected", category="request")
        plan = self.scoped_plan(group, "front", "only this")
        self.execute(plan); self.drain()
        self.failure.clear()
        current = self.repository.load(group["id"])
        retry = self.plan(current, "retry", operation_id=plan["id"], target_views=["front"])
        self.assertEqual(retry["targets"], ["front"])
        self.assertEqual(retry["design_request"], plan["design_request"])
        self.requests.clear(); self.execute(retry); self.drain()
        self.assertEqual([r.view for r in self.requests], ["front"])
        self.assertEqual(self.requests[0].local_prompt, "only this")
        self.assertEqual(self.repository.load(group["id"])["pending_views"], ["left", "back", "right"])

    def test_local_prompt_precedence_is_explicit_without_changing_legacy_image_prompts(self):
        from dataclasses import replace
        from advanced_multiview_image_gateway import design_prompt
        group, _ = self.initial()
        self.execute(self.scoped_plan(group, "left", "make the blue coat red")); self.drain()
        request = self.requests[-1]
        local = design_prompt(request)
        self.assertIn("make the blue coat red", local)
        self.assertIn("take precedence", local)
        self.assertIn("current left design", local)
        legacy = design_prompt(replace(request, local_prompt=None))
        self.assertNotIn("Local requirements:", legacy)
        self.assertEqual(design_prompt(replace(request, local_prompt="")), legacy)

    def test_local_plan_cannot_change_its_text_or_scope_after_confirmation(self):
        group, _ = self.initial()
        plan = self.scoped_plan(group, "left", "the confirmed local text")
        changed = deepcopy(plan)
        changed["design_request"]["local_prompt"] = "a different edit"
        self.repository.write_record(group["id"], f"plans/{plan['id']}.json", changed)
        with self.assertRaises(RequestError) as caught:
            self.execute(plan)
        self.assertEqual(caught.exception.code, "stale_confirmation")
        self.assertEqual(self.queue, [])

    def test_local_commands_replay_once_and_reject_reused_ids_with_different_intent(self):
        for provider in ("gpt", "yoyoclaw"):
            with self.subTest(provider=provider):
                group = self.create(design_provider=provider,
                                    design_model=QWEN_IMAGE if provider == "yoyoclaw" else "")
                self.execute(self.plan(group)); self.drain()
                group = self.repository.load(group["id"])
                self.requests.clear()
                command = new_id()
                plan = self.scoped_plan(group, "left", "only the left detail", command_id=command)
                self.assertEqual(self.scoped_plan(group, "left", "only the left detail",
                                                  command_id=command), plan)
                for view, text in (("left", "different text"), ("right", "only the left detail"),
                                   (None, "")):
                    with self.subTest(view=view, text=text), self.assertRaises(RequestError) as caught:
                        self.scoped_plan(group, view, text, command_id=command)
                    self.assertEqual(caught.exception.code, "idempotency_conflict")
                first = self.execute(plan)
                self.assertEqual(self.execute(plan), first)
                self.assertEqual(len(self.queue), 1)
                self.drain()
                finished = self.application.operation(group["id"], command)
                self.assertEqual(finished["state"], "succeeded")
                self.assertEqual(finished["design_request"], plan["design_request"])
                self.assertEqual(list(finished["attempts"]), ["left"])
                self.assertEqual(finished["new_image_calls"], 1)
                self.assertEqual(self.execute(plan), finished)
                self.drain()
                self.assertEqual([request.view for request in self.requests], ["left"])
                self.assertEqual(self.requests[0].local_prompt, "only the left detail")

    def test_local_stop_intent_blocks_late_acceptance_and_late_result_publication(self):
        for provider in ("gpt", "yoyoclaw"):
            for stage in ("before_acceptance", "queued", "result_obtained"):
                with self.subTest(provider=provider, stage=stage):
                    group = self.create(design_provider=provider,
                                        design_model=QWEN_IMAGE if provider == "yoyoclaw" else "")
                    self.execute(self.plan(group)); self.drain()
                    group = self.repository.load(group["id"])
                    before = deepcopy(group)
                    self.requests.clear()
                    plan = self.scoped_plan(group, "back", "stop this local edit")
                    if stage == "before_acceptance":
                        self.application.stop(group["id"], plan["id"])
                        self.execute(plan)
                    else:
                        if stage == "result_obtained":
                            self.after_image = lambda _: self.application.stop(group["id"], plan["id"])
                        self.execute(plan)
                        if stage == "queued":
                            self.application.stop(group["id"], plan["id"])
                    self.drain()
                    self.after_image = None
                    operation = self.application.operation(group["id"], plan["id"])
                    current = self.repository.load(group["id"])
                    self.assertEqual(operation["state"], "stopped")
                    self.assertEqual(operation["design_request"], plan["design_request"])
                    self.assertEqual(list(operation["attempts"]), ["back"])
                    expected_calls = int(stage == "result_obtained")
                    self.assertEqual(operation["new_image_calls"], expected_calls)
                    self.assertEqual(len(self.requests), expected_calls)
                    self.assertIsNone(current["active_operation_id"])
                    for field in ("saved_draft", "latest_submitted_draft", "initial_input", "pending_views"):
                        self.assertEqual(current[field], before[field])
                    for view in VIEWS:
                        self.assertEqual(current["views"][view]["current_version_id"],
                                         before["views"][view]["current_version_id"])
                        if view != "back":
                            self.assertEqual(current["views"][view], before["views"][view])
                    if stage == "result_obtained":
                        candidate = operation["attempts"]["back"]["version_id"]
                        self.assertTrue(candidate)
                        self.assertIsNotNone(self.repository.version(group["id"], "back", candidate))
                    self.assertEqual(self.execute(plan), operation)
                    self.drain()
                    self.assertEqual(len(self.requests), expected_calls)

    def test_negative_local_reconciliation_rejects_the_delayed_original_post(self):
        from advanced_multiview_runtime import AdvancedMultiviewRuntime
        for provider in ("gpt", "yoyoclaw"):
            with self.subTest(provider=provider):
                group = self.create(design_provider=provider,
                                    design_model=QWEN_IMAGE if provider == "yoyoclaw" else "")
                self.execute(self.plan(group)); self.drain()
                group = self.repository.load(group["id"])
                self.requests.clear()
                plan = self.scoped_plan(group, "right", "this exact local request")
                runtime = AdvancedMultiviewRuntime.__new__(AdvancedMultiviewRuntime)
                runtime.repository, runtime.workflow, runtime.application = self.repository, self.workflow, self.application
                runtime.models = None
                self.assertFalse(runtime.reconcile(group["id"], plan["id"])["accepted"])
                with self.assertRaises(RequestError) as caught:
                    self.execute(plan)
                self.assertEqual(caught.exception.code, "submission_not_accepted")
                self.assertEqual(self.repository.load(group["id"]), group)
                self.assertEqual(self.queue, [])
                self.assertEqual(self.requests, [])

    def test_local_saved_result_recovers_only_its_target_without_resubmitting_or_advancing_global_baseline(self):
        for provider in ("gpt", "yoyoclaw"):
            with self.subTest(provider=provider):
                group = self.create(design_provider=provider,
                                    design_model=QWEN_IMAGE if provider == "yoyoclaw" else "")
                self.execute(self.plan(group)); self.drain()
                group = self.repository.load(group["id"])
                group = self.application.save_draft(group["id"], {
                    "expected_group_revision": group["revision"],
                    "draft": {**group["saved_draft"], "style": "cartoon", "prompt": "unsubmitted global change"}})
                before = deepcopy(group)
                self.requests.clear()
                plan = self.scoped_plan(group, "left", "recover this left detail")
                self.execute(plan)
                original_save = self.repository.save

                def fail_pointer(value):
                    if value["views"]["left"]["current_version_id"] != before["views"]["left"]["current_version_id"]:
                        raise RequestError("persistence_failed", "full", 503)
                    return original_save(value)

                with mock.patch.object(self.repository, "save", side_effect=fail_pointer):
                    self.drain()
                operation = self.application.operation(group["id"], plan["id"])
                self.assertEqual(operation["state"], "recovery_required")
                self.assertEqual(operation["attempts"]["left"]["state"], "result_saved")
                self.workflow.recover_interrupted(group["id"])
                restored = AdvancedMultiviewRepository(self.repository.root.parent).load(group["id"])
                operation = self.application.operation(group["id"], plan["id"])
                self.assertEqual(operation["state"], "succeeded")
                self.assertEqual(operation["design_request"], plan["design_request"])
                self.assertEqual(operation["new_image_calls"], 1)
                self.assertEqual([request.view for request in self.requests], ["left"])
                self.assertEqual(restored["views"]["left"]["current_version_id"],
                                 operation["attempts"]["left"]["version_id"])
                self.assertEqual(restored["pending_views"], ["front", "back", "right"])
                for field in ("saved_draft", "latest_submitted_draft", "initial_input"):
                    self.assertEqual(restored[field], before[field])
                for view in ("front", "back", "right"):
                    self.assertEqual(restored["views"][view], before["views"][view])
                self.assertEqual(self.queue, [])

    def test_local_admission_failures_preserve_intent_and_require_explicit_recovery_or_retry(self):
        for provider in ("gpt", "yoyoclaw"):
            for boundary in ("plan", "operation", "attachment", "queue"):
                with self.subTest(provider=provider, boundary=boundary):
                    group = self.create(design_provider=provider,
                                        design_model=QWEN_IMAGE if provider == "yoyoclaw" else "")
                    self.execute(self.plan(group)); self.drain()
                    group = self.repository.load(group["id"])
                    before = deepcopy(group)
                    self.requests.clear()
                    command_id = new_id()
                    intent = "preserve this left request"
                    plan_path = f"plans/{command_id}.json"
                    operation_path = f"operations/{command_id}/operation.json"
                    original_write = self.repository.write_record
                    original_save = self.repository.save
                    hits = []

                    def write_record(group_id, relative, value):
                        if relative == (plan_path if boundary == "plan" else operation_path):
                            if boundary in {"plan", "operation"}:
                                hits.append(relative)
                                raise RequestError("persistence_failed", "controlled write failure", 503)
                        return original_write(group_id, relative, value)

                    def save(value):
                        if boundary == "attachment" and value["active_operation_id"] == command_id:
                            hits.append("attachment")
                            raise RequestError("persistence_failed", "controlled attachment failure", 503)
                        return original_save(value)

                    if boundary == "plan":
                        with mock.patch.object(self.repository, "write_record", side_effect=write_record):
                            with self.assertRaises(RequestError) as caught:
                                self.scoped_plan(group, "left", intent, command_id=command_id)
                        self.assertEqual(caught.exception.code, "persistence_failed")
                        self.assertFalse(self.repository.path(group["id"], plan_path, must_exist=False).exists())
                    plan = self.scoped_plan(group, "left", intent, command_id=command_id)
                    if boundary in {"operation", "attachment"}:
                        with mock.patch.object(self.repository, "write_record", side_effect=write_record), \
                             mock.patch.object(self.repository, "save", side_effect=save):
                            with self.assertRaises(RequestError) as caught:
                                self.execute(plan)
                        self.assertEqual(caught.exception.code, "persistence_failed")
                        self.assertEqual(self.repository.load(group["id"]), before)
                    elif boundary == "queue":
                        with mock.patch.object(self.application, "submit", side_effect=RuntimeError("queue closed")):
                            with self.assertRaises(RequestError) as caught:
                                self.execute(plan)
                        hits.append("queue")
                        self.assertEqual(caught.exception.code, "executor_unavailable")
                        accepted = self.application.operation(group["id"], command_id)
                        self.assertEqual(accepted["state"], "recovery_required")
                        self.assertEqual(accepted["new_image_calls"], 0)
                        self.assertFalse(self.application.operation_running(command_id))
                        self.workflow.recover_interrupted(group["id"])
                        stopped = self.application.operation(group["id"], command_id)
                        self.assertEqual(stopped["attempts"]["left"]["state"], "stopped")
                        self.assertEqual(self.execute(plan), stopped)
                        group = self.repository.load(group["id"])
                        self.assertIsNone(group["active_operation_id"])
                        plan = self.scoped_plan(group, "left", intent)
                    self.assertTrue(hits)
                    self.assertEqual(self.requests, [])
                    self.assertEqual(self.queue, [])
                    self.execute(plan); self.drain()
                    operation = self.application.operation(group["id"], plan["id"])
                    restored = AdvancedMultiviewRepository(self.repository.root.parent).load(group["id"])
                    self.assertEqual(operation["state"], "succeeded")
                    self.assertEqual(operation["targets"], ["left"])
                    self.assertEqual(operation["design_request"]["local_prompt"], intent)
                    self.assertEqual(operation["new_image_calls"], 1)
                    self.assertEqual([request.view for request in self.requests], ["left"])
                    for field in ("saved_draft", "latest_submitted_draft", "initial_input"):
                        self.assertEqual(restored[field], before[field])
                    for view in ("front", "back", "right"):
                        self.assertEqual(restored["views"][view], before["views"][view])
                    self.assertEqual(self.queue, [])

    def test_local_publication_recovers_every_durable_boundary_without_another_image_call(self):
        for provider in ("gpt", "yoyoclaw"):
            for boundary in ("version", "candidate_receipt", "current", "completed_receipt"):
                with self.subTest(provider=provider, boundary=boundary):
                    group = self.create(design_provider=provider,
                                        design_model=QWEN_IMAGE if provider == "yoyoclaw" else "")
                    self.execute(self.plan(group)); self.drain()
                    group = self.repository.load(group["id"])
                    group = self.application.save_draft(group["id"], {
                        "expected_group_revision": group["revision"],
                        "draft": {**group["saved_draft"], "style": "cartoon", "prompt": "unsubmitted global change"}})
                    before = deepcopy(group)
                    self.requests.clear()
                    plan = self.scoped_plan(group, "left", "recover only this left result")
                    self.execute(plan)
                    original_write = self.repository.write_record
                    original_save = self.repository.save
                    hits = []

                    def write_record(group_id, relative, value):
                        fail = boundary == "version" and relative.startswith("designs/left/")
                        if relative == f"operations/{plan['id']}/operation.json":
                            state = value["attempts"]["left"]["state"]
                            fail = fail or (boundary == "candidate_receipt" and state == "result_saved")
                            fail = fail or (boundary == "completed_receipt" and state == "succeeded")
                        if fail:
                            hits.append(relative)
                            raise RequestError("persistence_failed", "controlled publication failure", 503)
                        return original_write(group_id, relative, value)

                    def save(value):
                        if (boundary == "current" and value["views"]["left"]["current_version_id"] !=
                                before["views"]["left"]["current_version_id"]):
                            hits.append("current")
                            raise RequestError("persistence_failed", "controlled current failure", 503)
                        return original_save(value)

                    with mock.patch.object(self.repository, "write_record", side_effect=write_record), \
                         mock.patch.object(self.repository, "save", side_effect=save):
                        self.drain()
                    self.assertTrue(hits)
                    interrupted = self.application.operation(group["id"], plan["id"])
                    self.assertEqual(interrupted["state"], "recovery_required")
                    self.assertEqual(interrupted["new_image_calls"], 1)
                    self.assertEqual([request.view for request in self.requests], ["left"])
                    durable = self.repository.load(group["id"])
                    if boundary != "completed_receipt":
                        self.assertEqual(durable["views"]["left"]["current_version_id"],
                                         before["views"]["left"]["current_version_id"])
                    self.workflow.recover_interrupted(group["id"])
                    self.workflow.recover_interrupted(group["id"])
                    restored = AdvancedMultiviewRepository(self.repository.root.parent).load(group["id"])
                    operation = self.application.operation(group["id"], plan["id"])
                    self.assertEqual(operation["state"], "succeeded")
                    self.assertEqual(operation["targets"], ["left"])
                    self.assertEqual(operation["design_request"], plan["design_request"])
                    self.assertEqual(operation["new_image_calls"], 1)
                    self.assertEqual([request.view for request in self.requests], ["left"])
                    current = self.repository.version(group["id"], "left", restored["views"]["left"]["current_version_id"])
                    self.assertEqual(current["operation_id"], plan["id"])
                    self.assertEqual(current["attempt_id"], operation["attempts"]["left"]["id"])
                    self.assertEqual(restored["pending_views"], ["front", "back", "right"])
                    self.assertIsNone(restored["active_operation_id"])
                    for field in ("saved_draft", "latest_submitted_draft", "initial_input"):
                        self.assertEqual(restored[field], before[field])
                    for view in ("front", "back", "right"):
                        self.assertEqual(restored["views"][view], before["views"][view])
                    self.assertEqual(self.queue, [])

    def test_local_entry_does_not_create_an_unattempted_direction_after_upload_replacement(self):
        group, _ = self.initial({"front": png(), "left": png("red")})
        group = self.application.update_inputs(group["id"], {
            **group["saved_draft"], "expected_group_revision": group["revision"]},
            {"front": png(), "left": png("blue")})
        with self.assertRaises(RequestError) as caught:
            self.scoped_plan(group, "left", "local edit")
        self.assertEqual(caught.exception.code, "missing_current_view")
        self.assertEqual(self.queue, [])

    def test_local_interrupted_submission_without_a_saved_response_stays_ambiguous_without_resubmission(self):
        from advanced_multiview_image_gateway import AdvancedMultiviewImageGateway
        for provider in ("gpt", "yoyoclaw"):
            with self.subTest(provider=provider):
                group = self.create(design_provider=provider,
                                    design_model=QWEN_IMAGE if provider == "yoyoclaw" else "")
                self.execute(self.plan(group)); self.drain()
                group = self.repository.load(group["id"])
                group = self.application.save_draft(group["id"], {
                    "expected_group_revision": group["revision"],
                    "draft": {**group["saved_draft"], "prompt": "unsubmitted global change", "style": "cartoon"}})
                before = deepcopy(group)
                plan = self.scoped_plan(group, "left", "only the left shoulder")
                self.execute(plan); self.queue.clear(); self.requests.clear()
                operation = self.application.operation(group["id"], plan["id"])
                operation["attempts"]["left"]["state"] = "submitting"
                operation["new_image_calls"] = 1
                self.repository.write_record(group["id"], f"operations/{plan['id']}/operation.json", operation)
                fake = _FakeYoYoImageGateway()
                gateway = AdvancedMultiviewImageGateway(fake)
                repository = AdvancedMultiviewRepository(Path(self.temporary.name))
                generate = mock.Mock(side_effect=AssertionError("Recovery cannot create an image"))
                workflow = AdvancedMultiviewDesignWorkflow(repository, generate, gateway.recover)
                with mock.patch("openai_preprocessor._image_provider_request") as create:
                    for _ in range(2):
                        workflow.recover_interrupted(group["id"], expected_operation=plan["id"])
                    create.assert_not_called()
                restored = repository.load(group["id"])
                recovered = self.application.operation(group["id"], plan["id"])
                self.assertEqual(recovered["state"], "recovery_required")
                self.assertEqual(list(recovered["attempts"]), ["left"])
                self.assertEqual(recovered["attempts"]["left"]["state"], "ambiguous")
                self.assertEqual(recovered["new_image_calls"], 1)
                self.assertEqual(recovered["design_request"], plan["design_request"])
                self.assertEqual(recovered["frozen"], plan["frozen"])
                self.assertIsNone(restored["active_operation_id"])
                for field in ("saved_draft", "latest_submitted_draft", "initial_input", "pending_views"):
                    self.assertEqual(restored[field], before[field])
                for view in VIEWS:
                    self.assertEqual(restored["views"][view]["current_version_id"], before["views"][view]["current_version_id"])
                    if view != "left": self.assertEqual(restored["views"][view], before["views"][view])
                generate.assert_not_called()
                self.assertEqual(fake.generate_calls, [])
                self.assertEqual(fake.download_calls, [])
                self.assertEqual(self.requests, [])
                self.assertEqual(self.queue, [])

    def test_local_saved_gateway_responses_resume_only_their_original_download_for_both_providers(self):
        import base64
        import openai_preprocessor as gpt
        from advanced_multiview_image_gateway import AdvancedMultiviewImageGateway
        for provider in ("gpt", "yoyoclaw"):
            for state in ("submitting", "result_pending"):
                with self.subTest(provider=provider, state=state):
                    group = self.create(design_provider=provider,
                                        design_model=QWEN_IMAGE if provider == "yoyoclaw" else "")
                    self.execute(self.plan(group)); self.drain()
                    group = self.repository.load(group["id"])
                    group = self.application.save_draft(group["id"], {
                        "expected_group_revision": group["revision"],
                        "draft": {**group["saved_draft"], "prompt": "unsubmitted global change", "style": "cartoon"}})
                    before = deepcopy(group)
                    plan = self.scoped_plan(group, "right", "only the right shoulder")
                    self.execute(plan); self.queue.clear(); self.requests.clear()
                    fake = _FakeYoYoImageGateway()
                    fake.download_error = ProviderGatewayError("download unavailable", code="result_download_failed",
                        category="download", provider="yoyoclaw", retryable=True)
                    gateway = AdvancedMultiviewImageGateway(fake)
                    workflow = AdvancedMultiviewDesignWorkflow(self.repository, gateway.generate, gateway.recover)
                    response = {"id": "original-local-response", "data": [{"b64_json": base64.b64encode(png("teal")).decode()}]}
                    with mock.patch.object(gpt, "_image_config", return_value=mock.Mock(model="gpt-image-2")), \
                         mock.patch.object(gpt, "_image_provider_request", return_value=response) as create, \
                         mock.patch.object(gpt, "_save_provider_image", side_effect=gpt.OpenAIPreprocessorError(
                             "download unavailable", code="result_download_failed", retryable=True)):
                        workflow.run(group["id"], plan["id"])
                        self.assertEqual(create.call_count, 1 if provider == "gpt" else 0)
                    operation = self.application.operation(group["id"], plan["id"])
                    self.assertEqual(operation["attempts"]["right"]["state"], "result_pending")
                    self.assertEqual(operation["new_image_calls"], 1)
                    if state == "submitting":
                        # A crash can precede the attempt's download-failure checkpoint.
                        operation["attempts"]["right"]["state"] = state
                        self.repository.write_record(group["id"], f"operations/{plan['id']}/operation.json", operation)
                    repository = AdvancedMultiviewRepository(Path(self.temporary.name))
                    generate = mock.Mock(side_effect=AssertionError("Recovery cannot create an image"))
                    restarted = AdvancedMultiviewDesignWorkflow(repository, generate, gateway.recover)
                    with mock.patch.object(gpt, "_image_provider_request") as create, \
                         mock.patch.object(gpt, "_save_provider_image", side_effect=gpt.OpenAIPreprocessorError(
                             "download unavailable", code="result_download_failed", retryable=True)):
                        restarted.recover_interrupted(group["id"], expected_operation=plan["id"])
                        self.assertEqual(self.application.operation(group["id"], plan["id"])["attempts"]["right"]["state"],
                                         "result_pending")
                        create.assert_not_called()
                    fake.download_error = None
                    with mock.patch.object(gpt, "_image_provider_request") as create:
                        for _ in range(2):
                            restarted.recover_interrupted(group["id"], expected_operation=plan["id"])
                        create.assert_not_called()
                    restored = repository.load(group["id"])
                    operation = self.application.operation(group["id"], plan["id"])
                    self.assertEqual(operation["state"], "succeeded")
                    self.assertEqual(list(operation["attempts"]), ["right"])
                    self.assertEqual(operation["design_request"], plan["design_request"])
                    self.assertEqual(operation["frozen"], plan["frozen"])
                    self.assertEqual(operation["new_image_calls"], 1)
                    self.assertEqual(operation["attempts"]["right"]["provider_request_id"],
                                     "yoyo-request-1" if provider == "yoyoclaw" else "original-local-response")
                    self.assertNotEqual(restored["views"]["right"]["current_version_id"], before["views"]["right"]["current_version_id"])
                    self.assertEqual(restored["pending_views"], ["front", "left", "back"])
                    for field in ("saved_draft", "latest_submitted_draft", "initial_input"):
                        self.assertEqual(restored[field], before[field])
                    for view in ("front", "left", "back"):
                        self.assertEqual(restored["views"][view], before["views"][view])
                    generate.assert_not_called()
                    self.assertEqual(len(fake.generate_calls), 1 if provider == "yoyoclaw" else 0)
                    self.assertEqual(len(fake.download_calls), 3 if provider == "yoyoclaw" else 0)
                    self.assertEqual(self.requests, [])
                    self.assertEqual(self.queue, [])

    def test_recovered_local_upload_result_clears_only_its_invalidated_source_relationship(self):
        for provider in ("gpt", "yoyoclaw"):
            with self.subTest(provider=provider):
                images = {"front": png(), "left": png("red")}
                group = self.create(images, design_provider=provider,
                                    design_model=QWEN_IMAGE if provider == "yoyoclaw" else "")
                self.execute(self.plan(group)); self.drain()
                group = self.repository.load(group["id"])
                group = self.application.update_inputs(group["id"], {
                    **group["saved_draft"], "expected_group_revision": group["revision"]},
                    {"front": images["front"], "left": png("blue")})
                self.failure["left"] = ProviderGatewayError("rejected", code="rejected", category="request")
                self.execute(self.scoped_plan(group)); self.drain()
                self.failure.clear()
                before = self.repository.load(group["id"])
                self.assertEqual(before["pending_views"], ["left"])
                self.assertEqual(before["changed_upload_views"], ["left"])
                self.assertIsNone(before["views"]["left"]["current_version_id"])
                plan = self.scoped_plan(before, "left", "repair only this source")
                self.requests.clear()
                self.execute(plan)
                original_save = self.repository.save

                def fail_pointer(value):
                    if value["views"]["left"]["current_version_id"]:
                        raise RequestError("persistence_failed", "full", 503)
                    return original_save(value)

                with mock.patch.object(self.repository, "save", side_effect=fail_pointer):
                    self.drain()
                self.workflow.recover_interrupted(group["id"])
                self.workflow.recover_interrupted(group["id"])
                restored = self.repository.load(group["id"])
                operation = self.application.operation(group["id"], plan["id"])
                self.assertEqual(operation["state"], "succeeded")
                self.assertEqual(operation["new_image_calls"], 1)
                self.assertEqual([request.view for request in self.requests], ["left"])
                self.assertEqual(restored["pending_views"], [])
                self.assertEqual(restored["changed_upload_views"], [])
                self.assertTrue(restored["views"]["left"]["current_version_id"])
                self.assertEqual(restored["uploads"], before["uploads"])
                for field in ("saved_draft", "latest_submitted_draft", "initial_input"):
                    self.assertEqual(restored[field], before[field])
                for view in ("front", "back", "right"):
                    self.assertEqual(restored["views"][view], before["views"][view])

    def test_missing_main_allows_only_unchanged_or_deleted_auxiliary_uploads_until_restored(self):
        group, _ = self.initial({"front": png(), "left": png("red"), "back": png("green")})
        before = deepcopy(group)
        group = self.application.update_inputs(group["id"], {
            **group["saved_draft"], "expected_group_revision": group["revision"]},
            {"left": png("red"), "back": png("green")})
        self.assertIsNone(group["views"]["front"]["current_version_id"])
        self.assertEqual(group["views"]["left"], before["views"]["left"])
        for images, replaced in (({"left": png("blue")}, []), ({"right": png()}, []),
                                 ({"left": png("red")}, ["left"])):
            with self.subTest(images=list(images), replaced=replaced), self.assertRaises(RequestError):
                self.application.update_inputs(group["id"], {**group["saved_draft"],
                    "expected_group_revision": group["revision"], "replaced_views": replaced}, images)
            self.assertEqual(self.repository.load(group["id"]), group)
        group = self.application.update_inputs(group["id"], {
            **group["saved_draft"], "expected_group_revision": group["revision"]}, {"left": png("red")})
        group = self.application.update_inputs(group["id"], {
            **group["saved_draft"], "expected_group_revision": group["revision"]}, {"front": png(), "left": png("red")})
        self.assertIsNotNone(group["uploads"]["front"])

    def test_all_uploaded_direction_subsets_generate_only_their_own_targets_for_each_provider(self):
        for provider in ("gpt", "yoyoclaw"):
            for mask in range(8):
                selected = ["front"] + [v for slot,v in enumerate(VIEWS[1:]) if mask & (1 << slot)]
                with self.subTest(provider=provider,selected=selected):
                    group = self.create({v:png(v == "front" and "red" or "green") for v in selected},
                                        design_provider=provider,design_model=QWEN_IMAGE if provider == "yoyoclaw" else "")
                    plan = self.plan(group)
                    self.assertEqual(plan["targets"],selected)
                    self.assertEqual(plan["frozen"]["design_targets"],selected)
                    self.assertEqual(plan["max_new_image_calls"],len(selected))
                    self.requests.clear(); self.execute(plan); self.drain()
                    self.assertEqual([r.view for r in self.requests],selected)
                    snapshot = self.repository.snapshot(group["id"])
                    self.assertEqual(snapshot["design_targets"],selected)
                    self.assertEqual([v for v in VIEWS if snapshot["views"][v]["current"]],selected)

    def test_deleted_uploads_stay_empty_across_reload_and_style_changes_preserve_immutable_versions(self):
        group,_ = self.initial({v:png() for v in VIEWS})
        versions = {v:group["views"][v]["current_version_id"] for v in VIEWS}
        inputs = {v:png() for v in ("front","back")}
        updated = self.application.update_inputs(group["id"],
            {"expected_group_revision":group["revision"],**group["saved_draft"]},inputs)
        for view in ("left","right"):
            self.assertIsNone(updated["views"][view]["current_version_id"])
            self.assertIsNotNone(self.repository.version(group["id"],view,versions[view]))
        reloaded = AdvancedMultiviewRepository(self.repository.root.parent).snapshot(group["id"])
        for view in ("left","right"): self.assertIsNone(reloaded["views"][view]["current"])
        updated = self.application.save_draft(group["id"],{"expected_group_revision":updated["revision"],
            "draft":{**group["saved_draft"],"style":"cartoon"}})
        plan = self.plan(updated,"edit",base_versions={v:versions[v] for v in ("front","back")},**updated["saved_draft"])
        self.assertEqual(plan["targets"],["front","back"])
        self.requests.clear(); self.execute(plan); self.drain()
        self.assertEqual([r.view for r in self.requests],["front","back"])

    def test_explicit_same_content_replacement_invalidates_design_and_failure_does_not_restore_it(self):
        group,_ = self.initial({"front":png(),"right":png("red")})
        previous = group["views"]["right"]["current_version_id"]
        updated = self.application.update_inputs(group["id"],
            {"expected_group_revision":group["revision"],**group["saved_draft"],"replaced_views":'["right"]'},
            {"front":png(),"right":png("red")})
        self.assertNotEqual(updated["input_epoch"],group["input_epoch"])
        self.assertIsNone(updated["views"]["right"]["current_version_id"])
        self.failure["right"] = ProviderGatewayError("rejected",code="rejected",category="request")
        plan = self.plan(updated,"edit",target_views=["right"],base_versions={"right":None})
        self.execute(plan); self.drain()
        current = self.repository.snapshot(group["id"])
        self.assertIsNone(current["views"]["right"]["current"])
        self.assertIsNotNone(self.repository.version(group["id"],"right",previous))
        self.assertIsNotNone(current["views"]["front"]["current"])

    def test_incomplete_and_empty_input_drafts_save_without_authorizing_new_generation(self):
        group,_ = self.initial({"front":png(),"right":png("red")})
        for images,code in (({"right":png("red")},"missing_front"),({},"invalid_request")):
            group = self.application.update_inputs(group["id"],
                {"expected_group_revision":group["revision"],**group["saved_draft"],"prompt":""},images)
            with self.subTest(code=code),self.assertRaises(RequestError) as caught: self.plan(group,"edit")
            self.assertEqual(caught.exception.code,code)
        self.assertTrue(all(self.repository.snapshot(group["id"])["views"][v]["current"] is None for v in VIEWS))

    def test_removing_every_upload_restores_all_text_targets_only_on_explicit_generation(self):
        for order in (VIEWS, tuple(reversed(VIEWS)), (None,)):
            for prompt_when_removed in ("A small robot", ""):
                for save_prompt_with_inputs in (False, True):
                    with self.subTest(order=order, prompt=prompt_when_removed,
                                      inputs=save_prompt_with_inputs):
                        group, _ = self.initial({v: png() for v in VIEWS})
                        versions = {v: group["views"][v]["current_version_id"] for v in VIEWS}
                        self.requests.clear()
                        remaining = {v: png() for v in VIEWS}
                        for view in order:
                            if view is None:
                                remaining.clear()
                            else:
                                remaining.pop(view)
                            group = self.application.update_inputs(group["id"],
                                {"expected_group_revision": group["revision"],
                                 **group["saved_draft"], "prompt": prompt_when_removed}, remaining)
                            self.assertEqual(self.requests, [])
                            self.assertEqual(self.queue, [])
                        self.assertTrue(all(self.repository.snapshot(group["id"])["views"][v]["current"]
                                            is None for v in VIEWS))
                        if not prompt_when_removed:
                            with self.assertRaises(RequestError):
                                self.plan(group, "edit", target_views=["front"], base_versions={"front": None})
                            draft = {**group["saved_draft"], "prompt": "A small robot"}
                            if save_prompt_with_inputs:
                                group = self.application.update_inputs(group["id"],
                                    {"expected_group_revision": group["revision"], **draft}, {})
                            else:
                                group = self.application.save_draft(group["id"],
                                    {"expected_group_revision": group["revision"], "draft": draft})
                        group = self.repository.load(group["id"])
                        self.assertEqual(group["pending_views"], list(VIEWS))
                        # A stale local repaint selection cannot reduce the new text generation.
                        plan = self.plan(group, "edit", target_views=["front"],
                                         base_versions={"front": None}, **group["saved_draft"])
                        self.assertEqual(plan["targets"], list(VIEWS))
                        self.assertEqual(plan["max_new_image_calls"], 4)
                        self.assertEqual(self.requests, [])
                        self.execute(plan); self.drain()
                        self.assertEqual([r.view for r in self.requests], list(VIEWS))
                        for view in VIEWS:
                            self.assertIsNotNone(self.repository.version(group["id"], view, versions[view]))

    def test_replaced_direction_metadata_rejects_unknown_duplicate_or_unuploaded_directions(self):
        group = self.create({"front":png()})
        for replaced in ('not-json','["top"]','["front","front"]','["left"]'):
            with self.subTest(replaced=replaced),self.assertRaises(RequestError):
                self.application.update_inputs(group["id"],
                    {"expected_group_revision":group["revision"],**group["saved_draft"],"replaced_views":replaced},
                    {"front":png()})

    def test_empty_selection_and_explicit_all_share_effective_range(self):
        self.assertEqual(targets([]), VIEWS)
        self.assertEqual(targets(["right", "left"]), ("left", "right"))
        for value in (["front", "front"], ["top"], "front", [True]):
            with self.subTest(value=value), self.assertRaises(RequestError):
                targets(value)

    def test_uploads_preserve_originals_and_never_fill_unprovided_slots(self):
        original = png()
        group = self.create({"front": original})
        self.assertEqual(self.repository.path(group["id"], group["uploads"]["front"]["path"]).read_bytes(), original)
        self.assertTrue(all(group["uploads"][v] is None for v in VIEWS[1:]))
        self.assertEqual(self.requests, [])

    def test_side_uploads_cannot_be_silently_treated_as_text(self):
        with self.assertRaises(RequestError) as error:
            self.create({"back": png()})
        self.assertEqual(error.exception.code, "missing_front")

    def test_corrupt_animated_small_and_oversized_inputs_are_rejected(self):
        animated = BytesIO()
        Image.new("RGB", (96, 96), "red").save(animated, "WEBP", save_all=True,
                                               append_images=[Image.new("RGB", (96, 96), "green")])
        for data in (b"not an image", png(size=32), animated.getvalue(), b"x" * (20 * 1024 * 1024 + 1)):
            with self.subTest(bytes=len(data)), self.assertRaises(RequestError):
                self.create({"front": data})

    def test_request_identity_is_idempotent_and_content_bound(self):
        request_id = new_id()
        fields = {"prompt": "Same object"}
        a = self.repository.create(request_id, fields, {})
        b = self.repository.create(request_id, fields, {})
        self.assertEqual(a, b)
        with self.assertRaises(RequestError):
            self.repository.create(request_id, {"prompt": "Different object"}, {})

    def test_unsafe_id_and_cross_group_paths_are_rejected(self):
        group = self.create()
        for value in ("../outside", "../../group.json", str(Path(self.temporary.name) / "outside")):
            with self.subTest(value=value), self.assertRaises(RequestError):
                self.repository.path(group["id"], value, must_exist=False)
        with self.assertRaises(RequestError):
            self.repository.load("../outside")

    def test_failed_atomic_save_does_not_advance_durable_revision(self):
        group = self.create()
        group["saved_draft"]["prompt"] = "A changed draft"
        with mock.patch("advanced_multiview_repository.os.replace", side_effect=OSError("full")):
            with self.assertRaises(RequestError):
                self.repository.save(group)
        loaded = self.repository.load(group["id"])
        self.assertEqual(loaded["revision"], 0)
        self.assertEqual(loaded["saved_draft"]["prompt"], "One object")

    def test_stale_revision_cannot_overwrite_new_draft(self):
        group = self.create()
        changed = deepcopy(group)
        changed["saved_draft"]["prompt"] = "New"
        self.repository.save(changed)
        with self.assertRaises(RequestError):
            self.repository.save(group)
        self.assertEqual(self.repository.load(group["id"])["saved_draft"]["prompt"], "New")

    def test_group_name_is_optional_and_never_changes_the_design_prompt(self):
        group = self.create()
        path = self.repository.directory(group["id"]) / "group.json"
        legacy = json.loads(path.read_text(encoding="utf-8"))
        legacy.pop("display_name")
        path.write_text(json.dumps(legacy), encoding="utf-8")
        self.assertNotIn("display_name", self.repository.load(group["id"]))

        renamed = self.application.rename_group(group["id"], {
            "expected_group_revision": group["revision"], "display_name": "  白色杯子套图  "})
        self.assertEqual(renamed["display_name"], "白色杯子套图")
        self.assertEqual(renamed["saved_draft"]["prompt"], "One object")
        history = self.repository.list_groups()
        self.assertEqual(history, [])  # Unpublished creations stay out of history.

        cleared = self.application.rename_group(group["id"], {
            "expected_group_revision": renamed["revision"], "display_name": ""})
        self.assertEqual(cleared["display_name"], "")
        self.assertEqual(cleared["saved_draft"]["prompt"], "One object")

    def test_group_name_rejects_stale_invalid_and_busy_changes(self):
        group = self.create()
        renamed = self.application.rename_group(group["id"], {
            "expected_group_revision": group["revision"], "display_name": "First"})
        with self.assertRaises(RequestError) as stale:
            self.application.rename_group(group["id"], {
                "expected_group_revision": group["revision"], "display_name": "Stale"})
        self.assertEqual(stale.exception.code, "revision_conflict")
        for invalid in ("two\nlines", "名" * 81):
            with self.subTest(invalid=invalid[:8]), self.assertRaises(RequestError):
                self.application.rename_group(group["id"], {
                    "expected_group_revision": renamed["revision"], "display_name": invalid})

        plan = self.plan(renamed)
        self.execute(plan)
        busy = self.repository.load(group["id"])
        with self.assertRaises(RequestError) as error:
            self.application.rename_group(group["id"], {
                "expected_group_revision": busy["revision"], "display_name": "Busy"})
        self.assertEqual(error.exception.code, "group_busy")
        self.assertEqual(self.repository.load(group["id"])["display_name"], "First")

    def test_damaged_schema_is_preserved_and_reported(self):
        group = self.create()
        path = self.repository.directory(group["id"]) / "group.json"
        path.write_text('{"schema":"future"}', encoding="utf-8")
        with self.assertRaises(RequestError):
            self.repository.load(group["id"])
        self.assertEqual(json.loads(path.read_text())["schema"], "future")

    def test_first_generation_uses_uploaded_directions_or_four_text_views(self):
        cases = [()] + [("front",) + extra for count in range(4) for extra in combinations(VIEWS[1:], count)]
        for uploaded in cases:
            with self.subTest(uploaded=uploaded):
                self.requests.clear()
                images = {v: png() for v in uploaded}
                group, _ = self.initial(images)
                expected = list(uploaded or VIEWS)
                self.assertEqual([r.view for r in self.requests], expected)
                self.assertEqual(self.repository.snapshot(group["id"])["all_views_available"], len(expected) == 4)
                self.assertEqual([role for role, _ in self.requests[0].references],
                                 ["front_upload"] if "front" in uploaded else [])
                for request in self.requests:
                    roles = [role for role, _ in request.references]
                    if request.view in uploaded:
                        self.assertEqual(roles, [request.view + "_upload"])
                    elif request.view != "front":
                        self.assertIn("front_design", roles)

    def test_each_uploaded_view_is_its_only_initial_reference(self):
        images = {view: png() for view in VIEWS}
        self.initial(images)
        for request in self.requests:
            self.assertEqual([role for role, _ in request.references], [request.view + "_upload"])

    def test_unuploaded_directions_never_create_extra_design_calls(self):
        for auxiliary in ("back", "right"):
            with self.subTest(auxiliary=auxiliary):
                self.requests.clear()
                group, _ = self.initial({"front": png(), auxiliary: png()})
                requests = {request.view: request for request in self.requests}
                self.assertEqual(set(requests), {"front", auxiliary})
                self.assertEqual([role for role, _ in requests[auxiliary].references], [auxiliary + "_upload"])
                self.requests.clear()
                with self.assertRaises(RequestError) as rejected:
                    self.plan(group, "edit", target_views=["left"], base_versions={"left": None})
                self.assertEqual(rejected.exception.code, "invalid_request")
                self.assertEqual(self.requests, [])
                self.assertEqual(self.queue, [])
                for selection in ([], ["front"]):
                    bases = {view: group["views"][view]["current_version_id"]
                             for view in (selection or ["front", auxiliary])}
                    plan = self.plan(group, "edit", target_views=selection, base_versions=bases)
                    self.assertEqual(plan["targets"], selection or ["front", auxiliary])
                    self.assertEqual(plan["frozen"]["design_targets"], ["front", auxiliary])
                self.assertEqual(self.requests, [])
                self.assertEqual(self.queue, [])

    def test_confirmation_cancel_and_unavailable_capability_make_no_calls(self):
        group = self.create()
        self.plan(group)
        self.assertEqual(self.queue, [])
        self.assertEqual(self.repository.list_groups(), [])
        self.capability["initial_text"] = "unknown"
        with self.assertRaises(RequestError):
            self.plan(group)
        self.assertEqual(self.requests, [])

    def test_confirmed_plan_freezes_relative_camera_contract_and_one_call_per_target(self):
        plan = self.plan(self.create())
        contract = plan["frozen"]["view_contract"]
        self.assertEqual(contract["schema"], "advanced-relative-camera-v2")
        self.assertEqual(contract["views"]["left"]["signed_yaw_degrees"], 90)
        self.assertEqual(contract["views"]["left"]["top_view_orbit"], "clockwise")
        self.assertEqual(contract["views"]["right"]["signed_yaw_degrees"], -90)
        self.assertEqual(contract["views"]["right"]["top_view_orbit"], "counterclockwise")
        self.assertEqual(plan["max_new_image_calls"], 4)

    def test_one_operation_freezes_one_yoyoclaw_model_and_a_later_edit_can_change_it(self):
        group = self.create(design_provider="yoyoclaw", design_model=QWEN_IMAGE)
        plan = self.plan(group)
        self.execute(plan)
        self.drain()
        self.assertEqual({request.model for request in self.requests}, {QWEN_IMAGE})
        current = self.repository.load(group["id"])
        back = current["views"]["back"]["current_version_id"]
        self.requests.clear()
        edit = self.plan(current, "edit", prompt="Refine", design_provider="yoyoclaw",
                         design_model=DOUBAO_IMAGE, target_views=["back"],
                         base_versions={"back": back})
        self.execute(edit)
        self.drain()
        self.assertEqual([request.model for request in self.requests], [DOUBAO_IMAGE])

    def test_failed_card_retry_keeps_original_model_when_current_input_changes(self):
        self.failure["left"] = ProviderGatewayError("rejected", code="rejected", category="request")
        group = self.create(design_provider="yoyoclaw", design_model=QWEN_IMAGE)
        initial = self.plan(group)
        self.execute(initial)
        self.drain()
        current = self.repository.load(group["id"])
        changed = {**current["saved_draft"], "style": "sculpture", "design_model": DOUBAO_IMAGE}
        current = self.application.save_draft(group["id"], {
            "expected_group_revision": current["revision"], "draft": changed})
        retry = self.plan(current, "retry", operation_id=initial["id"], target_views=["left"])
        self.assertEqual(retry["draft"]["style"], "sculpture")
        self.assertEqual(retry["draft"]["design_provider"], "yoyoclaw")
        self.assertEqual(retry["draft"]["design_model"], QWEN_IMAGE)

    def test_double_submit_returns_same_operation_and_enqueues_once(self):
        plan = self.plan(self.create())
        a = self.execute(plan)
        b = self.execute(plan)
        self.assertEqual(a, b)
        self.assertEqual(len(self.queue), 1)
        self.drain()
        self.execute(plan)
        self.assertEqual(len(self.requests), 4)

    def test_changed_draft_invalidates_prepared_confirmation(self):
        group = self.create()
        plan = self.plan(group)
        self.application.save_draft(group["id"], {"expected_group_revision": group["revision"],
                                                  "draft": {"prompt": "New text"}})
        with self.assertRaises(RequestError):
            self.execute(plan)
        self.assertEqual(self.queue, [])

    def test_edit_uses_current_design_and_only_updates_selected_views(self):
        group, _ = self.initial()
        before = {v: group["views"][v]["current_version_id"] for v in VIEWS}
        self.requests.clear()
        plan = self.plan(group, "edit", prompt="Change back accessory", target_views=["back"],
                         base_versions={"back": before["back"]})
        self.execute(plan)
        self.drain()
        after = self.repository.load(group["id"])
        self.assertEqual([r.view for r in self.requests], ["back"])
        self.assertEqual([role for role, _ in self.requests[0].references], ["back_design"])
        self.assertIn(before["back"], str(self.requests[0].references[0][1]))
        for view in VIEWS:
            self.assertEqual(after["views"][view]["current_version_id"] == before[view], view != "back")
        # A second edit must use the newly published result, not the upload or first design.
        current = after["views"]["back"]["current_version_id"]
        self.execute(self.plan(after, "edit", prompt="Refine", target_views=["back"], base_versions={"back": current}))
        self.drain()
        self.assertIn(current, str(self.requests[-1].references[0][1]))

    def test_replaced_upload_stays_in_group_and_leads_the_selected_edit(self):
        group, _ = self.initial({"front": png("navy")})
        before = {view: group["views"][view]["current_version_id"] for view in VIEWS}
        old_epoch = group["input_epoch"]
        updated = self.application.update_inputs(group["id"], {
            "expected_group_revision": group["revision"], **group["saved_draft"]},
            {"front": png("purple")})

        self.assertEqual(updated["id"], group["id"])
        self.assertNotEqual(updated["input_epoch"], old_epoch)
        self.assertEqual(updated["pending_views"], ["front"])
        self.assertEqual(updated["changed_upload_views"], ["front"])
        self.assertEqual(len(self.repository.list_groups()), 1)
        self.assertIsNone(updated["views"]["front"]["current_version_id"])
        self.assertIsNotNone(self.repository.version(group["id"], "front", before["front"]))

        self.requests.clear()
        plan = self.plan(updated, "edit", prompt="Use the replacement", target_views=["front"],
                         base_versions={"front": None})
        self.execute(plan)
        self.drain()
        self.assertEqual([request.view for request in self.requests], ["front"])
        self.assertEqual([role for role, _ in self.requests[0].references],
                         ["front_upload"])
        current = self.repository.load(group["id"])
        self.assertEqual(current["pending_views"], [])
        self.assertEqual(current["changed_upload_views"], [])
        for view in VIEWS[1:]:
            self.assertEqual(current["views"][view]["current_version_id"], before[view])

    def test_added_upload_is_required_alongside_extra_user_selection(self):
        group, _ = self.initial({"front": png("navy"), "right": png("yellow")})
        front = self.repository.path(group["id"], group["uploads"]["front"]["path"]).read_bytes()
        before = {view: group["views"][view]["current_version_id"] for view in VIEWS}
        updated = self.application.update_inputs(group["id"], {
            "expected_group_revision": group["revision"], **group["saved_draft"]},
            {"front": front, "left": png("purple"), "right": png("yellow")})
        self.assertEqual(updated["pending_views"], ["left"])

        plan = self.plan(updated, "edit", prompt="Also refine right", target_views=["right"],
                         base_versions={"right": before["right"]})
        self.assertEqual(plan["targets"], ["left", "right"])
        self.requests.clear()
        self.execute(plan)
        self.drain()
        self.assertEqual([request.view for request in self.requests], ["left", "right"])
        self.assertEqual([role for role, _ in self.requests[0].references],
                         ["left_upload"])

    def test_replaced_main_upload_is_available_to_user_selected_side_views(self):
        group, _ = self.initial({"front": png("navy"), "right": png("yellow")})
        before = {view: group["views"][view]["current_version_id"] for view in VIEWS}
        updated = self.application.update_inputs(group["id"], {
            "expected_group_revision": group["revision"], **group["saved_draft"]},
            {"front": png("purple"), "right": png("yellow")})
        plan = self.plan(updated, "edit", prompt="Align the right view", target_views=["right"],
                         base_versions={"right": before["right"]})
        self.assertEqual(plan["targets"], ["front", "right"])
        self.requests.clear()
        self.execute(plan)
        self.drain()
        requests = {request.view: request for request in self.requests}
        self.assertEqual([role for role, _ in requests["right"].references],
                         ["right_design", "right_upload", "front_upload"])

    def test_style_change_marks_only_effective_targets_pending(self):
        group, _ = self.initial({"front": png()})
        draft = {**group["saved_draft"], "style": "realistic"}
        updated = self.application.save_draft(group["id"], {
            "expected_group_revision": group["revision"], "draft": draft})
        self.assertEqual(updated["id"], group["id"])
        self.assertEqual(updated["pending_views"], ["front"])
        self.assertEqual(updated["changed_upload_views"], [])
        self.assertEqual(len(self.repository.list_groups()), 1)

        before = {view: updated["views"][view]["current_version_id"] for view in VIEWS}
        plan = self.plan(updated, "edit", **draft, target_views=["front"],
                         base_versions={"front": before["front"]})
        self.assertEqual(plan["targets"], ["front"])
        self.execute(plan)
        self.drain()
        self.assertEqual(self.repository.load(group["id"])["pending_views"], [])

    def test_failed_initial_view_retries_with_the_current_style_in_the_same_group(self):
        self.failure["left"] = ProviderGatewayError("rejected", code="rejected", category="request")
        group, initial = self.initial()
        self.assertIsNone(group["views"]["left"]["current_version_id"])
        self.failure.clear()
        style = "sculpture" if group["saved_draft"]["style"] != "sculpture" else "realistic"
        draft = {**group["saved_draft"], "style": style}
        updated = self.application.save_draft(group["id"], {
            "expected_group_revision": group["revision"], "draft": draft})

        retry = self.plan(updated, "retry", operation_id=initial["id"], target_views=["left"])
        self.assertEqual(retry["draft"]["style"], style)
        self.assertEqual(retry["frozen"]["input_epoch"], updated["input_epoch"])
        self.requests.clear()
        self.execute(retry)
        self.drain()

        current = self.repository.load(group["id"])
        self.assertEqual(current["id"], group["id"])
        self.assertIsNotNone(current["views"]["left"]["current_version_id"])
        self.assertEqual(current["pending_views"], ["front", "back", "right"])
        self.assertEqual([request.view for request in self.requests], ["left"])
        self.assertEqual(self.requests[0].style, style)

    def test_incomplete_input_draft_is_saved_but_generation_still_requires_front(self):
        group, _ = self.initial({"front": png(), "left": png("purple")})
        updated = self.application.update_inputs(group["id"], {
            "expected_group_revision": group["revision"], **group["saved_draft"]}, {"left": png("purple")})
        with self.assertRaises(RequestError) as error:
            self.plan(updated, "edit")
        self.assertEqual(error.exception.code, "missing_front")
        current = self.repository.load(group["id"])
        self.assertGreater(current["revision"], group["revision"])
        self.assertIsNone(current["uploads"]["front"])
        self.assertIsNotNone(current["uploads"]["left"])
        self.assertIsNone(current["views"]["front"]["current_version_id"])

    def test_partial_edit_keeps_failed_old_view_and_retries_it_alone(self):
        group, _ = self.initial()
        before = {v: group["views"][v]["current_version_id"] for v in VIEWS}
        self.failure["back"] = ProviderGatewayError("rejected", code="rejected", category="request")
        self.requests.clear()
        plan = self.plan(group, "edit", prompt="Change", target_views=["left", "back"],
                         base_versions={v: before[v] for v in ("left", "back")})
        self.execute(plan)
        self.drain()
        group = self.repository.load(group["id"])
        self.assertNotEqual(group["views"]["left"]["current_version_id"], before["left"])
        self.assertEqual(group["views"]["back"]["current_version_id"], before["back"])
        self.assertTrue(self.repository.snapshot(group["id"])["all_views_available"])
        self.failure.clear()
        self.requests.clear()
        retry = self.plan(group, "retry", operation_id=plan["id"], target_views=["back"])
        self.execute(retry)
        self.drain()
        self.assertEqual([r.view for r in self.requests], ["back"])

    def test_missing_initial_view_retry_uses_frozen_original_inputs(self):
        self.failure["back"] = ProviderGatewayError("rejected", code="rejected", category="request")
        group, plan = self.initial()
        self.assertFalse(self.repository.snapshot(group["id"])["all_views_available"])
        self.failure.clear()
        self.requests.clear()
        retry = self.plan(group, "retry", operation_id=plan["id"], target_views=["back"])
        self.execute(retry)
        self.drain()
        self.assertTrue(self.requests[0].initial)
        self.assertEqual([r.view for r in self.requests], ["back"])
        self.assertTrue(self.repository.snapshot(group["id"])["all_views_available"])

    def test_failed_front_blocks_dependents_and_explicit_retry_lists_remaining_calls(self):
        self.failure["front"] = ProviderGatewayError("rejected", code="rejected", category="request")
        group, plan = self.initial()
        self.assertEqual(len(self.requests), 1)
        self.failure.clear()
        retry = self.plan(group, "retry", operation_id=plan["id"], target_views=["front"])
        self.assertEqual(retry["targets"], list(VIEWS))
        self.assertEqual(retry["max_new_image_calls"], 4)
        self.execute(retry)
        self.drain()
        self.assertTrue(self.repository.snapshot(group["id"])["all_views_available"])

    def test_old_ambiguous_receipt_is_preserved_while_a_new_model_fills_failed_targets(self):
        self.failure["left"] = ProviderGatewayError("timeout", code="timeout", category="availability", ambiguous=True)
        group, old = self.initial()
        self.assertEqual([r.view for r in self.requests], ["front", "left"])
        with self.assertRaises(RequestError):
            self.plan(group, "retry", operation_id=old["id"], target_views=["left"])
        self.assertEqual(len(self.requests), 2)
        self.failure.clear()
        plan = self.plan(group, "edit", design_provider="yoyoclaw", design_model=QWEN_IMAGE,
                         target_views=["left", "back", "right"], base_versions=dict.fromkeys(VIEWS[1:]))
        self.assertEqual(plan["max_new_image_calls"], 3)
        self.assertEqual(set(plan["target_modes"].values()), {"initial"})
        self.execute(plan)
        self.drain()
        self.assertEqual([r.model for r in self.requests[2:]], [QWEN_IMAGE] * 3)
        self.assertEqual(self.application.operation(group["id"], old["id"])["attempts"]["left"]["state"], "ambiguous")
        self.assertTrue(self.repository.snapshot(group["id"])["all_views_available"])

    def test_one_current_model_plan_mixes_edits_and_missing_views_without_extra_calls(self):
        self.failure["left"] = ProviderGatewayError("rejected", code="rejected", category="request")
        group, _ = self.initial()
        original = {v: group["views"][v]["current_version_id"] for v in VIEWS}
        self.failure.clear()
        plan = self.plan(group, "edit", design_provider="yoyoclaw", design_model=HY_IMAGE,
                         target_views=["left", "back"], base_versions=original)
        self.assertEqual(plan["targets"], ["left", "back"])
        self.assertEqual(plan["target_modes"], {"left": "initial", "back": "edit"})
        self.assertEqual(plan["max_new_image_calls"], 2)
        self.execute(plan)
        self.drain()
        self.assertTrue(self.requests[-2].initial)
        self.assertFalse(self.requests[-1].initial)
        self.assertEqual([r.model for r in self.requests[-2:]], [HY_IMAGE, HY_IMAGE])
        restored = self.repository.load(group["id"])
        for view in ("front", "right"):
            self.assertEqual(restored["views"][view]["current_version_id"], original[view])

    def test_missing_text_anchor_is_listed_and_generated_before_selected_empty_view(self):
        self.failure["front"] = ProviderGatewayError("rejected", code="rejected", category="request")
        group, _ = self.initial()
        self.failure.clear()
        plan = self.plan(group, "edit", prompt="One object", design_model=QWEN_IMAGE,
                         target_views=["left"], base_versions=dict.fromkeys(VIEWS))
        self.assertEqual(plan["targets"], ["front", "left"])
        self.assertEqual(plan["dependency_views"], ["front"])
        self.assertEqual(plan["max_new_image_calls"], 2)
        self.execute(plan)
        self.drain()
        self.assertEqual([r.view for r in self.requests], ["front", "front", "left"])
        self.assertEqual([role for role, _ in self.requests[-1].references], ["front_design"])

    def test_existing_upload_avoids_unnecessary_main_dependency_and_failed_anchor_spends_no_extra_call(self):
        for images in ({"front": png(), "left": png("green")}, {}):
            with self.subTest(has_upload=bool(images)):
                group = self.create(images)
                plan = self.plan(group)
                self.failure["front"] = ProviderGatewayError("no", code="rejected", category="request")
                self.execute(plan)
                self.drain()
                group = self.repository.load(group["id"])
                before = len(self.requests)
                repair = self.plan(group, "edit", prompt="One object", target_views=["left"],
                                   base_versions=dict.fromkeys(VIEWS))
                self.assertEqual(repair["dependency_views"], [] if images else ["front"])
                self.execute(repair)
                self.drain()
                self.assertEqual(len(self.requests) - before, 1)
                state = self.repository.load(group["id"])
                self.assertIsNone(state["active_operation_id"])

    def test_two_missing_views_share_one_explicit_main_dependency_and_budget(self):
        for anchor_fails in (False, True):
            with self.subTest(anchor_fails=anchor_fails):
                self.failure["front"] = ProviderGatewayError("no", code="rejected", category="request")
                group, _ = self.initial()
                if not anchor_fails:
                    self.failure.clear()
                before = len(self.requests)
                plan = self.plan(group, "edit", design_provider="yoyoclaw", design_model=QWEN_IMAGE,
                                 target_views=["left", "back"], base_versions=dict.fromkeys(VIEWS))
                self.assertEqual(plan["targets"], ["front", "left", "back"])
                self.assertEqual(plan["dependency_views"], ["front"])
                self.assertEqual(plan["max_new_image_calls"], 3)
                self.execute(plan)
                self.drain()
                submitted = self.requests[before:]
                self.assertEqual([r.view for r in submitted], ["front"] if anchor_fails else ["front", "left", "back"])
                self.assertTrue(all(r.model == QWEN_IMAGE for r in submitted))
                result = self.repository.load(group["id"])
                self.assertIsNone(result["active_operation_id"])
                for view in ("front", "left", "back"):
                    self.assertEqual(bool(result["views"][view]["current_version_id"]), not anchor_fails)
                self.assertIsNone(result["views"]["right"]["current_version_id"])

    def test_confirmed_visible_base_is_used_even_if_canonical_current_was_not_displayed(self):
        group, _ = self.initial()
        visible = group["views"]["left"]["current_version_id"]
        self.execute(self.plan(group, "edit", prompt="First edit", target_views=["left"], base_versions={"left": visible}))
        self.drain()
        group = self.repository.load(group["id"])
        canonical = group["views"]["left"]["current_version_id"]
        plan = self.plan(group, "edit", prompt="Edit the visible version", target_views=["left"], base_versions={"left": visible})
        self.assertEqual(plan["base_versions"]["left"], visible)
        self.assertEqual(plan["publication_bases"]["left"], canonical)
        self.execute(plan)
        self.drain()
        self.assertIn(visible, str(self.requests[-1].references[0][1]))
        snapshot = self.repository.snapshot(group["id"])
        self.assertEqual(snapshot["views"]["left"]["current"]["base_version_id"], visible)

    def test_stop_before_or_after_acceptance_prevents_all_queued_supplier_calls(self):
        for stop_first in (True, False):
            with self.subTest(stop_first=stop_first):
                group = self.create()
                plan = self.plan(group)
                if not stop_first:
                    self.execute(plan)
                self.application.stop(group["id"], plan["id"])
                self.execute(plan)
                self.drain()
                operation = self.application.operation(group["id"], plan["id"])
                self.assertEqual(operation["state"], "stopped")
                self.assertEqual(operation["new_image_calls"], 0)
        self.assertEqual(self.requests, [])

    def test_new_all_view_generation_uses_current_model_and_never_reuses_stopped_candidate(self):
        group = self.create()
        old = self.plan(group)
        self.after_image = lambda _: self.application.stop(group["id"], old["id"])
        self.execute(old)
        self.drain()
        candidate = self.application.operation(group["id"], old["id"])["attempts"]["front"]["version_id"]
        group = self.repository.load(group["id"])
        self.after_image = None
        plan = self.plan(group, "edit", prompt="One object", design_model=DOUBAO_IMAGE,
                         target_views=[], base_versions=dict.fromkeys(VIEWS))
        self.assertEqual(plan["max_new_image_calls"], 4)
        self.assertEqual(plan["reusable_versions"], {})
        self.execute(plan)
        self.drain()
        self.assertEqual([r.model for r in self.requests[1:]], [DOUBAO_IMAGE] * 4)
        self.assertNotEqual(self.repository.load(group["id"])["views"]["front"]["current_version_id"], candidate)

    def test_stop_does_not_publish_late_image_or_start_following_calls(self):
        group = self.create()
        plan = self.plan(group)
        self.after_image = lambda _: self.application.stop(group["id"], plan["id"])
        self.execute(plan)
        self.drain()
        group = self.repository.load(group["id"])
        self.assertEqual(len(self.requests), 1)
        self.assertIsNone(group["views"]["front"]["current_version_id"])
        self.assertIsNone(group["active_operation_id"])
        attempt = self.application.operation(group["id"], plan["id"])["attempts"]["front"]
        self.assertEqual(attempt["state"], "stopped")
        self.assertTrue(attempt["version_id"])

    def test_serial_executor_projects_the_new_group_as_queued_until_old_sent_request_returns(self):
        from advanced_multiview_runtime import AdvancedMultiviewRuntime
        started, release = threading.Event(), threading.Event()
        requests, futures = [], []

        def generate(request, output):
            requests.append(request)
            if len(requests) == 1:
                started.set()
                if not release.wait(5):
                    raise RuntimeError("test barrier timed out")
            output.write_bytes(png())
            return DesignImageResult()

        executor = ThreadPoolExecutor(max_workers=1)
        self.addCleanup(executor.shutdown, wait=True)
        self.addCleanup(release.set)
        workflow = AdvancedMultiviewDesignWorkflow(self.repository, generate)
        app = AdvancedMultiviewApplication(self.repository, capabilities=lambda *_: self.capability,
            submit=lambda work: futures.append(executor.submit(work)), run_design=workflow.run)
        runtime = AdvancedMultiviewRuntime.__new__(AdvancedMultiviewRuntime)
        runtime.repository, runtime.application, runtime.workflow, runtime.models = self.repository, app, workflow, None
        old, new = self.create(), self.create()
        plans = [app.prepare(g["id"], {"command_id": new_id(), "kind": "initial",
                                      "expected_group_revision": g["revision"]}) for g in (old, new)]

        def accept(plan):
            return app.execute(plan["group_id"], {"command_id": plan["id"], "plan_id": plan["id"],
                                                 "confirmation_fingerprint": plan["confirmation_fingerprint"]})

        accept(plans[0])
        self.assertTrue(started.wait(3))
        app.stop(old["id"], plans[0]["id"])
        accept(plans[1])
        accept(plans[1])
        self.assertEqual(runtime.snapshot(new["id"])["execution_state"], "queued")
        self.assertEqual(len(requests), 1)
        self.assertEqual(len(futures), 2)
        release.set()
        for future in futures:
            future.result(timeout=5)
        self.assertEqual(len(requests), 5)
        self.assertEqual(app.operation(old["id"], plans[0]["id"])["new_image_calls"], 1)
        self.assertIsNone(runtime.snapshot(new["id"])["active_operation_id"])
        self.assertTrue(runtime.snapshot(new["id"])["all_views_available"])
        self.assertIsNone(runtime.snapshot(old["id"])["views"]["front"]["current_version_id"])

    def test_discarding_a_queued_group_spends_no_calls_and_does_not_block_the_next_group(self):
        groups = [self.create() for _ in range(3)]
        plans = [self.plan(group) for group in groups]
        self.execute(plans[0])
        self.execute(plans[1])
        self.application.stop(groups[1]["id"], plans[1]["id"])
        self.execute(plans[2])
        self.drain()
        stopped = self.application.operation(groups[1]["id"], plans[1]["id"])
        self.assertEqual(stopped["state"], "stopped")
        self.assertEqual(stopped["new_image_calls"], 0)
        stopped_attempts = {attempt["id"] for attempt in stopped["attempts"].values()}
        self.assertFalse(any(request.attempt_id in stopped_attempts for request in self.requests))
        self.assertEqual(len(self.requests), 8)
        for group in groups:
            result = self.repository.snapshot(group["id"])
            self.assertIsNone(result["active_operation_id"])
            self.assertEqual(result["all_views_available"], group["id"] != groups[1]["id"])

    def test_visible_bases_cannot_reference_a_version_owned_by_another_group_or_view(self):
        first, _ = self.initial()
        second, _ = self.initial()
        for version in (first["views"]["left"]["current_version_id"], second["views"]["front"]["current_version_id"]):
            with self.subTest(version=version), self.assertRaises(RequestError):
                self.plan(second, "edit", target_views=["left"], base_versions={"left": version})

    def test_user_can_regenerate_after_terminal_transport_loss_without_overwriting_the_old_receipt(self):
        from advanced_multiview_runtime import AdvancedMultiviewRuntime
        for view in ("front", "left"):
            with self.subTest(view=view):
                group, _ = self.initial({"front": png("navy"), "left": png("green")})
                group_id = group["id"]
                old_version = group["views"][view]["current_version_id"]
                old_plan = self.scoped_plan(group, view)
                self.failure[view] = ProviderGatewayError("connection lost", code="image_connection_failed",
                                                          category="availability", ambiguous=True)
                self.execute(old_plan)
                with self.assertRaises(RequestError) as error:
                    self.scoped_plan(self.repository.load(group_id), view)
                self.assertEqual(error.exception.code, "group_busy")
                self.drain()
                failed = self.repository.load(group_id)
                self.assertIsNone(failed["active_operation_id"])
                self.assertEqual(failed["views"][view]["current_version_id"], old_version)
                receipt = self.application.operation(group_id, old_plan["id"])
                self.assertEqual(receipt["attempts"][view]["state"], "ambiguous")
                self.failure.clear()
                count = len(self.requests)
                retry = self.scoped_plan(failed, view if view == "front" else None)
                self.assertEqual(len(self.requests), count)
                self.assertEqual(retry["targets"], ["front"] if view == "front" else ["front", "left"])
                self.execute(retry)
                self.execute(retry)
                self.drain()
                self.assertEqual(len(self.requests) - count, len(retry["targets"]))
                after = self.repository.load(group_id)
                self.assertNotEqual(after["views"][view]["current_version_id"], old_version)
                runtime = AdvancedMultiviewRuntime.__new__(AdvancedMultiviewRuntime)
                runtime.repository, runtime.application, runtime.workflow = self.repository, self.application, self.workflow
                with self.assertRaises(RequestError) as stale:
                    runtime.recover(group_id, old_plan["id"])
                self.assertEqual(stale.exception.code, "stale_result")
                self.assertEqual(self.application.operation(group_id, old_plan["id"]), receipt)
                self.assertEqual(self.repository.load(group_id), after)

    def test_explicit_recovery_of_corrupt_results_releases_retry_without_creating_a_new_image(self):
        from advanced_multiview_runtime import AdvancedMultiviewRuntime
        group, _ = self.initial({"front": png("navy")})
        old_version = group["views"]["front"]["current_version_id"]
        plan = self.scoped_plan(group, "front")
        calls = len(self.requests)

        def corrupt(request, output):
            self.requests.append(request)
            output.write_bytes(b"invalid result")
            return DesignImageResult()

        self.workflow.generate_image = corrupt
        self.execute(plan); self.drain()
        pending = self.repository.load(group["id"])
        with self.assertRaises(RequestError) as error:
            self.scoped_plan(pending, "front")
        self.assertEqual(error.exception.code, "provider_task_ambiguous")
        runtime = AdvancedMultiviewRuntime.__new__(AdvancedMultiviewRuntime)
        runtime.repository, runtime.application, runtime.workflow = self.repository, self.application, self.workflow
        resolved = runtime.recover(group["id"], plan["id"])
        self.assertEqual(resolved["attempts"]["front"]["state"], "failed")
        self.assertEqual(len(self.requests), calls + 1)
        released = self.repository.load(group["id"])
        self.assertEqual(released["views"]["front"]["current_version_id"], old_version)
        self.assertIsNone(released["active_operation_id"])
        self.assertEqual(self.scoped_plan(released, "front")["targets"], ["front"])

    def test_later_user_generation_prevents_recovery_of_superseded_saved_receipts(self):
        from advanced_multiview_runtime import AdvancedMultiviewRuntime
        self.failure["left"] = ProviderGatewayError("unknown", code="upstream", category="availability", ambiguous=True)
        group, old = self.initial()
        self.failure.clear()
        self.execute(self.plan(group, "edit", prompt="A new attempt", target_views=["left"], base_versions={"left": None}))
        self.drain()
        runtime = AdvancedMultiviewRuntime.__new__(AdvancedMultiviewRuntime)
        runtime.repository, runtime.application, runtime.workflow, runtime.models = self.repository, self.application, self.workflow, None
        before = self.repository.load(group["id"])
        with self.assertRaises(RequestError) as error:
            runtime.recover(group["id"], old["id"])
        self.assertEqual(error.exception.code, "stale_result")
        self.assertEqual(self.repository.load(group["id"]), before)

    def test_invalid_obtained_image_does_not_reintroduce_a_busy_lock_during_restart_recovery(self):
        calls = []
        def invalid_result(request, output):
            calls.append(request)
            output.write_bytes(b"invalid image result")
            return DesignImageResult(provider_request_id="known-response")
        self.workflow.generate_image = invalid_result
        group, old = self.initial()
        self.assertEqual(len(calls), 1)
        self.assertIsNone(group["active_operation_id"])
        for _ in range(2):
            self.workflow.recover_interrupted(group["id"])
            group = self.repository.load(group["id"])
            self.assertIsNone(group["active_operation_id"])
            self.assertEqual(group["views"]["front"]["latest_attempt"]["state"], "result_pending")
            self.assertIsNone(group["views"]["front"]["current_version_id"])
        self.assertEqual(len(calls), 1)
        plan = self.plan(group, "edit", prompt="Try with current model", target_views=["front"],
                         base_versions={"front": None}, design_model=QWEN_IMAGE)
        self.assertEqual(plan["max_new_image_calls"], 1)
        attempt = self.application.operation(group["id"], old["id"])["attempts"]["front"]
        self.assertTrue(self.repository.path(group["id"], f"staging/{attempt['id']}/result.png").exists())

    def test_reopen_and_restart_do_not_generate_and_missing_files_block_four_view_readiness(self):
        group, _ = self.initial()
        calls = len(self.requests)
        repository = AdvancedMultiviewRepository(Path(self.temporary.name))
        restored = repository.snapshot(group["id"])
        self.assertEqual(len(self.requests), calls)
        self.assertTrue(restored["all_views_available"])
        record = restored["views"]["back"]["current"]
        repository.path(group["id"], record["path"]).unlink()
        restored = repository.snapshot(group["id"])
        self.assertFalse(restored["all_views_available"])
        self.assertEqual(restored["views"]["back"]["current_version_id"], record["id"])

    def test_explicit_retry_reuses_stopped_candidate_and_only_generates_missing_views(self):
        group = self.create()
        initial = self.plan(group)
        self.after_image = lambda _: self.application.stop(group["id"], initial["id"])
        self.execute(initial)
        self.drain()
        old = self.application.operation(group["id"], initial["id"])
        candidate = old["attempts"]["front"]["version_id"]
        group = self.repository.load(group["id"])
        retry = self.plan(group, "retry", operation_id=initial["id"], target_views=["front"])
        self.assertEqual(retry["max_new_image_calls"], 3)
        self.after_image = None
        self.execute(retry)
        self.drain()
        restored = self.repository.snapshot(group["id"])
        self.assertEqual(restored["views"]["front"]["current_version_id"], candidate)
        self.assertTrue(restored["all_views_available"])
        self.assertEqual([r.view for r in self.requests], list(VIEWS))

    def test_replayed_acceptance_reattaches_receipt_after_failed_group_commit(self):
        group = self.create()
        plan = self.plan(group)
        with mock.patch.object(self.repository, "save", side_effect=RequestError("persistence_failed", "full", 503)):
            with self.assertRaises(RequestError):
                self.execute(plan)
        self.assertEqual(self.queue, [])
        self.execute(plan)
        self.execute(plan)
        self.assertEqual(len(self.queue), 1)
        self.drain()
        self.assertEqual(len(self.requests), 4)

    def test_restart_stops_orphan_acceptance_receipt_without_submitting(self):
        group = self.create()
        plan = self.plan(group)
        with mock.patch.object(self.repository, "save", side_effect=RequestError("persistence_failed", "full", 503)):
            with self.assertRaises(RequestError):
                self.execute(plan)
        self.workflow.recover_interrupted(group["id"])
        self.assertEqual(self.application.operation(group["id"], plan["id"])["state"], "stopped")
        recovered = self.repository.load(group["id"])
        for view in VIEWS:
            self.assertEqual(recovered["views"][view]["latest_attempt"]["state"], "stopped")
            self.assertEqual(recovered["views"][view]["latest_attempt"]["operation_id"], plan["id"])
        retry = self.plan(recovered, "retry", operation_id=plan["id"], target_views=["front"])
        self.assertEqual(retry["max_new_image_calls"], 4)
        self.assertEqual(self.requests, [])
        self.assertEqual(self.queue, [])

    def test_pointer_commit_failure_recovers_saved_candidate_without_generation(self):
        group = self.create()
        plan = self.plan(group)
        self.execute(plan)
        original_save = self.repository.save

        def fail_pointer(value):
            if value["views"]["front"]["current_version_id"]:
                raise RequestError("persistence_failed", "full", 503)
            return original_save(value)

        with mock.patch.object(self.repository, "save", side_effect=fail_pointer):
            self.drain()
        operation = self.application.operation(group["id"], plan["id"])
        self.assertEqual(operation["state"], "recovery_required")
        self.assertEqual(operation["attempts"]["front"]["state"], "result_saved")
        self.workflow.recover_interrupted(group["id"])
        restored = self.repository.snapshot(group["id"])
        self.assertTrue(restored["views"]["front"]["current"])
        self.assertEqual(len(self.requests), 1)

    def test_initial_confirmation_uses_last_saved_draft_before_first_submission(self):
        group = self.create()
        group = self.application.save_draft(group["id"], {
            "expected_group_revision": group["revision"], "draft": {"prompt": "Latest description"}})
        self.execute(self.plan(group))
        self.drain()
        self.assertTrue(all(r.prompt == "Latest description" for r in self.requests))
        self.assertEqual(self.repository.load(group["id"])["initial_input"]["prompt"], "Latest description")

    def test_missing_initial_retry_keeps_original_anchor_after_front_is_edited(self):
        self.failure["back"] = ProviderGatewayError("rejected", code="rejected", category="request")
        group, initial = self.initial()
        front = group["views"]["front"]["current_version_id"]
        self.failure.clear()
        self.execute(self.plan(group, "edit", prompt="New front accessory", target_views=["front"],
                               base_versions={"front": front}))
        self.drain()
        group = self.repository.load(group["id"])
        retry = self.plan(group, "retry", operation_id=initial["id"], target_views=["back"])
        self.execute(retry)
        self.drain()
        self.assertEqual(self.requests[-1].view, "back")
        self.assertTrue(any(front in str(path) for _, path in self.requests[-1].references))

    def test_stopped_attempt_states_are_projected_to_history_without_busy_leftovers(self):
        group = self.create()
        plan = self.plan(group)
        self.execute(plan)
        self.application.stop(group["id"], plan["id"])
        self.drain()
        group = self.repository.snapshot(group["id"])
        self.assertTrue(all(group["views"][v]["latest_attempt"]["state"] == "stopped" for v in VIEWS))
        self.assertIsNone(group["active_operation_id"])
        self.assertEqual(self.requests, [])

    def test_saved_gpt_response_recovers_download_without_creating_another_image(self):
        import base64
        from advanced_multiview_image_gateway import AdvancedMultiviewImageGateway
        group = self.create()
        plan = self.plan(group)
        self.execute(plan)
        self.queue.clear()
        operation = self.application.operation(group["id"], plan["id"])
        attempt = operation["attempts"]["front"]
        attempt["state"] = "submitting"
        self.repository.write_record(group["id"], f"operations/{plan['id']}/operation.json", operation)
        output = self.repository.path(group["id"], f"staging/{attempt['id']}/result.png", must_exist=False)
        self.repository.atomic_json(output.with_suffix(".response.json"),
                                    {"id": "saved-response", "data": [{"b64_json": base64.b64encode(png()).decode()}]})
        gateway = AdvancedMultiviewImageGateway()
        workflow = AdvancedMultiviewDesignWorkflow(self.repository, mock.Mock(), gateway.recover)
        with mock.patch("openai_preprocessor._image_provider_request") as create:
            workflow.recover_interrupted(group["id"])
            create.assert_not_called()
        self.assertTrue(self.repository.snapshot(group["id"])["views"]["front"]["current"])
        self.assertEqual(self.requests, [])

    def test_saved_result_download_releases_the_group_store_and_respects_stop(self):
        group = self.create()
        plan = self.plan(group)
        self.execute(plan)
        self.queue.clear()
        operation = self.application.operation(group["id"], plan["id"])
        operation["attempts"]["front"]["state"] = "submitting"
        self.repository.write_record(group["id"], f"operations/{plan['id']}/operation.json", operation)
        entered, release = threading.Event(), threading.Event()

        def download(output):
            entered.set()
            if not release.wait(5):
                raise AssertionError("Timed out waiting for the stop transaction")
            output.parent.mkdir(parents=True, exist_ok=True)
            output.write_bytes(png())
            return DesignImageResult(provider_request_id="original-request")

        workflow = AdvancedMultiviewDesignWorkflow(self.repository, mock.Mock(), download)
        with ThreadPoolExecutor(max_workers=2) as worker:
            pending = worker.submit(workflow.recover_interrupted, group["id"])
            try:
                self.assertTrue(entered.wait(5))
                stopped = worker.submit(self.application.stop, group["id"], plan["id"]).result(timeout=3)
                self.assertTrue(stopped["stop_intent"])
            finally:
                release.set()
            pending.result(timeout=5)
        self.assertIsNone(self.repository.snapshot(group["id"])["views"]["front"]["current"])
        attempt = self.application.operation(group["id"], plan["id"])["attempts"]["front"]
        self.assertEqual(attempt["state"], "stopped")
        self.assertEqual(attempt["provider_request_id"], "original-request")
        workflow.generate_image.assert_not_called()

    def test_yoyoclaw_gateway_routes_selected_model_and_recovers_saved_response_without_resubmission(self):
        from advanced_multiview_design_workflow import DesignImageRequest
        from advanced_multiview_image_gateway import AdvancedMultiviewImageGateway
        fake = _FakeYoYoImageGateway()
        gateway = AdvancedMultiviewImageGateway(fake)
        references = []
        for index in range(3):
            path = Path(self.temporary.name) / f"yoyo-reference-{index}.png"
            path.write_bytes(png(["red", "green", "blue"][index]))
            references.append((f"role-{index}", path))
        request = DesignImageRequest("right", False, "same object", "realistic", "", "yoyoclaw",
                                     HY_IMAGE, new_id(), tuple(references))
        output = Path(self.temporary.name) / "yoyo-output.png"
        result = gateway.generate(request, output)
        self.assertEqual(result.provider_request_id, "yoyo-request-1")
        self.assertEqual(fake.generate_calls[0][0], HY_IMAGE)
        self.assertEqual(fake.generate_calls[0][2], tuple(path for _, path in references))
        self.assertTrue(output.with_suffix(".response.json").is_file())
        output.unlink()
        recovered = gateway.recover(output)
        self.assertEqual(recovered.provider_request_id, "yoyo-request-1")
        self.assertEqual(len(fake.generate_calls), 1)
        self.assertEqual(len(fake.download_calls), 2)
        self.assertTrue(output.is_file())

    def test_yoyoclaw_capabilities_list_each_available_supported_model_without_fallback(self):
        from advanced_multiview_image_gateway import AdvancedMultiviewImageGateway
        fake = _FakeYoYoImageGateway()
        gateway = AdvancedMultiviewImageGateway(fake)
        capabilities = gateway.catalog_capabilities()
        yoyo = [item for item in capabilities if item["provider"] == "yoyoclaw"]
        self.assertEqual([item["model"] for item in yoyo], fake.models)
        self.assertTrue(all(item["initial_named_references"] == "supported" for item in yoyo))
        self.assertTrue(all(item["max_reference_images"] == 3 for item in yoyo))
        fake.models.remove(QWEN_IMAGE)
        unavailable = gateway.capabilities("yoyoclaw", QWEN_IMAGE)
        self.assertEqual(unavailable["reason"], "model_unavailable")
        self.assertEqual(unavailable["initial_text"], "unknown")

    def test_provider_failure_persists_and_logs_safe_correlated_fields(self):
        self.failure["front"] = ProviderGatewayError(
            "provider rejected", code="OutputImageSensitiveContentDetected.PolicyViolation",
            category="provider", provider="yoyoclaw", operation="image_generation",
            status=400, provider_message="Rejected token=private-value", request_id="request-safe-1",
        )
        output = StringIO()
        with mock.patch("sys.stderr", output):
            created = self.create(design_provider="yoyoclaw", design_model=QWEN_IMAGE)
            plan = self.plan(created)
            self.execute(plan)
            self.drain()
            group = self.repository.load(created["id"])
        attempt = self.application.operation(group["id"], plan["id"])["attempts"]["front"]
        error = attempt["error"]
        self.assertEqual(error["code"], "OutputImageSensitiveContentDetected.PolicyViolation")
        self.assertEqual(error["http_status"], 400)
        self.assertEqual(error["request_id"], "request-safe-1")
        self.assertIn("<redacted>", error["provider_message"])
        event = next(json.loads(line) for line in output.getvalue().splitlines()
                     if "advanced_multiview.image.failed" in line)
        self.assertEqual(event["operation_id"], plan["id"])
        self.assertEqual(event["attempt_id"], attempt["id"])
        self.assertEqual(event["view"], "front")
        self.assertEqual(event["http_status"], 400)
        self.assertNotIn("private-value", output.getvalue())

    def test_candidate_receipt_failure_recovers_immutable_version_without_generation(self):
        group = self.create()
        plan = self.plan(group)
        self.execute(plan)
        original_write = self.repository.write_record

        def fail_result_receipt(group_id, relative, record):
            if relative.endswith("operation.json") and record["attempts"]["front"]["state"] == "result_saved":
                raise RequestError("persistence_failed", "full", 503)
            return original_write(group_id, relative, record)

        with mock.patch.object(self.repository, "write_record", side_effect=fail_result_receipt):
            self.drain()
        self.workflow.recover_interrupted(group["id"])
        self.assertTrue(self.repository.snapshot(group["id"])["views"]["front"]["current"])
        self.assertEqual(len(self.requests), 1)

    def test_interrupted_submission_is_marked_ambiguous_without_supplier_call(self):
        group = self.create()
        plan = self.plan(group)
        self.execute(plan)
        self.queue.clear()
        operation = self.application.operation(group["id"], plan["id"])
        operation["attempts"]["front"]["state"] = "submitting"
        self.repository.write_record(group["id"], f"operations/{plan['id']}/operation.json", operation)
        self.workflow.recover_interrupted(group["id"])
        restored = self.repository.load(group["id"])
        self.assertEqual(restored["views"]["front"]["latest_attempt"]["state"], "ambiguous")
        self.assertEqual(self.requests, [])
        self.assertIsNone(restored["active_operation_id"])

    def test_upload_parser_preserves_named_binary_payloads_and_unicode(self):
        boundary = "orca-test-boundary"
        chunks = []
        fields = {"request_id": new_id(), "prompt": "同一对象"}
        for name, value in fields.items():
            chunks.append(f'--{boundary}\r\nContent-Disposition: form-data; name="{name}"\r\n\r\n{value}\r\n'.encode())
        for view in VIEWS:
            chunks.extend([f'--{boundary}\r\nContent-Disposition: form-data; name="{view}_image"; filename="image.png"\r\n'
                           'Content-Type: image/png\r\n\r\n'.encode(), png(view == "front" and "red" or "green"), b"\r\n"])
        chunks.append(f"--{boundary}--\r\n".encode())
        body = b"".join(chunks)
        parsed_fields, images = parse_upload(BytesIO(body), f"multipart/form-data; boundary={boundary}", len(body))
        self.assertEqual(parsed_fields, fields)
        self.assertEqual(set(images), set(VIEWS))
        self.assertEqual(images["front"], png("red"))

    def test_supplier_edit_uploads_have_valid_image_mime_types_and_extensions(self):
        from email.parser import BytesParser
        from email.policy import default
        from advanced_multiview_design_workflow import DesignImageRequest
        from advanced_multiview_image_gateway import multipart_references
        references = []
        for number, format_name in enumerate(("PNG", "JPEG")):
            path = Path(self.temporary.name) / f"reference-{number}"
            Image.new("RGB", (256, 256), "teal").save(path, format=format_name)
            references.append(("front_design" if number == 0 else "left_upload", path))
        request = DesignImageRequest("left", True, "Same object", "realistic", "", "gpt", "gpt-image-2", new_id(), tuple(references))
        body, content_type = multipart_references(request, "one side view", "gpt-image-2")
        parsed = BytesParser(policy=default).parsebytes(f"Content-Type: {content_type}\r\n\r\n".encode() + body)
        files = [part for part in parsed.iter_parts() if part.get_filename()]
        self.assertEqual(len(files), 2)
        for part, (_, path), mime, extension in zip(files, references, ("image/png", "image/jpeg"), ("png", "jpg")):
            self.assertEqual(part.get_content_type(), mime)
            self.assertTrue(part.get_filename().endswith("." + extension))
            self.assertEqual(part.get_param("name", header="content-disposition"), "image[]")
            self.assertEqual(part.get_payload(decode=True), path.read_bytes())

    def test_corresponding_upload_prompt_preserves_its_view_shape_pose_and_crop(self):
        from advanced_multiview_design_workflow import DesignImageRequest
        from advanced_multiview_image_gateway import design_prompt
        from openai_preprocessor import build_geometry_reference_prompt
        path = Path(self.temporary.name) / "left.png"
        path.write_bytes(png())
        request = DesignImageRequest("left", True, "Keep the red coat", "realistic", "", "gpt", "gpt-image-2",
                                     new_id(), (("left_upload", path),))
        prompt = design_prompt(request)
        self.assertIn("uploaded left view", prompt)
        self.assertIn("shape, proportions, pose, camera viewpoint, rotation", prompt)
        self.assertIn("framing, crop, visible extent", prompt)
        self.assertIn("Keep the red coat", prompt)
        self.assertIn(build_geometry_reference_prompt("Keep the red coat", "realistic"), prompt)
        self.assertIn("uniform opaque solid-color studio background", prompt)
        self.assertIn("Replace any such backdrop in the source image", prompt)
        self.assertNotIn("signed yaw", prompt)

    def test_missing_view_prompt_uses_only_camera_angle_and_allows_hidden_region_inference(self):
        from advanced_multiview_design_workflow import DesignImageRequest
        from advanced_multiview_image_gateway import design_prompt
        from model_contracts import DEFAULT_IMAGE_INSTRUCTION
        from openai_preprocessor import build_geometry_reference_prompt
        path = Path(self.temporary.name) / "reference.png"
        path.write_bytes(png())
        request = DesignImageRequest("left", True, "", "realistic", "", "gpt", "gpt-image-2", new_id(),
                                     (("front_upload", path), ("front_design", path)))
        prompt = design_prompt(request)
        self.assertIn("signed yaw +90 degrees (clockwise)", prompt)
        self.assertIn("Preserve the shape, proportions, and pose", prompt)
        self.assertIn("person, figurine, pet, building, or other object", prompt)
        self.assertIn("regions that are not visible", prompt)
        self.assertIn("reasonable inference", prompt)
        self.assertIn(build_geometry_reference_prompt(DEFAULT_IMAGE_INSTRUCTION, "realistic"), prompt)
        self.assertIn("Multiview override", prompt)
        self.assertIn("background replacement", prompt)
        self.assertNotIn("quality-correction", prompt)
        self.assertNotIn("same physical right hand", prompt)

    def test_rotated_views_are_relative_to_the_main_reference_camera(self):
        from advanced_multiview_design_workflow import DesignImageRequest
        from advanced_multiview_image_gateway import design_prompt
        path = Path(self.temporary.name) / "reference.png"
        path.write_bytes(png())
        expected = {
            "left": "signed yaw +90 degrees (clockwise)",
            "back": "signed yaw +180 degrees (half-turn)",
            "right": "signed yaw -90 degrees (counterclockwise)",
        }
        for view, phrase in expected.items():
            with self.subTest(view=view):
                request = DesignImageRequest(view, True, "", "realistic", "", "gpt", "gpt-image-2", new_id(),
                                             (("front_upload", path), ("front_design", path)))
                prompt = design_prompt(request)
                self.assertIn(phrase, prompt)
                self.assertIn("keep the subject fixed", prompt)

    def test_text_only_first_view_becomes_the_main_reference_camera(self):
        from advanced_multiview_design_workflow import DesignImageRequest
        from advanced_multiview_image_gateway import design_prompt
        from openai_preprocessor import build_text_geometry_reference_prompt
        request = DesignImageRequest("front", True, "A small building", "realistic", "", "gpt", "gpt-image-2",
                                     new_id(), ())
        prompt = design_prompt(request)
        self.assertIn("main-reference camera at 0 degrees", prompt)
        self.assertIn("A small building", prompt)
        self.assertIn(build_text_geometry_reference_prompt("A small building", "realistic"), prompt)
        self.assertIn("uniform opaque solid-color studio background", prompt)

    def test_edit_prompt_preserves_current_target_view_and_only_applies_requested_change(self):
        from advanced_multiview_design_workflow import DesignImageRequest
        from advanced_multiview_image_gateway import design_prompt
        from openai_preprocessor import build_geometry_reference_prompt
        path = Path(self.temporary.name) / "current.png"
        path.write_bytes(png())
        request = DesignImageRequest("back", False, "Change the jacket color", "realistic", "", "gpt",
                                     "gpt-image-2", new_id(), (("back_design", path),))
        prompt = design_prompt(request)
        self.assertIn("Apply only the requested description and style changes", prompt)
        self.assertIn("camera viewpoint, rotation", prompt)
        self.assertIn("framing, crop", prompt)
        self.assertIn("Change the jacket color", prompt)
        self.assertIn(build_geometry_reference_prompt("Change the jacket color", "realistic"), prompt)
        self.assertIn("uniform opaque solid-color studio background", prompt)
        self.assertNotIn("signed yaw", prompt)

    def test_supplier_edit_accepts_five_references_with_auto_size_and_high_fidelity(self):
        from email.parser import BytesParser
        from email.policy import default
        from advanced_multiview_design_workflow import DesignImageRequest
        from advanced_multiview_image_gateway import multipart_references
        references = []
        for number in range(5):
            path = Path(self.temporary.name) / f"five-reference-{number}.png"
            path.write_bytes(png())
            references.append((f"reference_{number}", path))
        request = DesignImageRequest("left", True, "Same person", "realistic", "", "gpt", "gpt-image-2",
                                     new_id(), tuple(references))
        body, content_type = multipart_references(request, "one side view", "gpt-image-2")
        parsed = BytesParser(policy=default).parsebytes(f"Content-Type: {content_type}\r\n\r\n".encode() + body)
        parts = list(parsed.iter_parts())
        fields = {part.get_param("name", header="content-disposition"): part.get_content()
                  for part in parts if not part.get_filename()}
        self.assertEqual(len([part for part in parts if part.get_filename()]), 5)
        self.assertEqual(fields["size"], "auto")
        self.assertEqual(fields["background"], "opaque")
        self.assertEqual(fields["input_fidelity"], "high")

    def test_provider_generation_id_is_kept_when_response_has_no_top_level_id(self):
        from advanced_multiview_image_gateway import _provider_request_id
        self.assertEqual(_provider_request_id({"data": [{"generation_id": "generation-123"}]}),
                         "generation-123")
        self.assertEqual(_provider_request_id({"id": "request-123", "data": [{"generation_id": "ignored"}]}),
                         "request-123")

    def test_upload_parser_rejects_duplicates_unknown_fields_and_incomplete_requests(self):
        for field in ("front_image", "prompt", "unknown"):
            boundary = "boundary"
            body = (f'--{boundary}\r\nContent-Disposition: form-data; name="{field}"\r\n\r\nx\r\n'
                    f'--{boundary}\r\nContent-Disposition: form-data; name="{field}"\r\n\r\ny\r\n'
                    f'--{boundary}--\r\n').encode()
            with self.subTest(field=field), self.assertRaises(RequestError):
                parse_upload(BytesIO(body), f"multipart/form-data; boundary={boundary}", len(body))
        with self.assertRaises(RequestError):
            parse_upload(BytesIO(b"broken"), "multipart/form-data; boundary=boundary", 100)

    def test_boundary_like_bytes_in_file_do_not_truncate_the_upload(self):
        data = b"x" * (64 * 1024 - 130) + b"\r\n--boundaryXX" + b"y" * 100
        body = (b'--boundary\r\nContent-Disposition: form-data; name="front_image"; filename="x.png"\r\n\r\n'
                + data + b"\r\n--boundary--\r\n")
        _, images = parse_upload(BytesIO(body), "multipart/form-data; boundary=boundary", len(body))
        self.assertEqual(images["front"], data)

    def test_unauthenticated_transport_does_not_construct_or_recover_runtime(self):
        handler = mock.Mock()
        handler.path = PREFIX + "/groups"
        handler._require_native_client.return_value = False
        factory = mock.Mock()
        self.assertTrue(dispatch(handler, factory, "GET"))
        factory.assert_not_called()

    def test_scoped_geometry_discovery_does_not_construct_a_group_runtime(self):
        from advanced_multiview_runtime import _capabilities

        handler = mock.Mock()
        handler.path = PREFIX + "/capabilities?scope=geometry&provider=tripo"
        handler._require_native_client.return_value = True
        image = mock.Mock()
        model = mock.Mock(return_value={"available": True, "named_four_view_model": "supported"})
        runtime = mock.Mock(side_effect=AssertionError("Unexpected group recovery"))
        self.assertTrue(dispatch(handler, runtime, "GET", lambda scope, provider: _capabilities(scope, image, model, provider)))
        runtime.assert_not_called()
        image.catalog_capabilities.assert_not_called()
        model.assert_called_once_with("tripo", "")
        status, document = handler.send_json.call_args.args
        self.assertEqual(status, 200)
        self.assertEqual(document["model_providers"], {"tripo": model.return_value})

    def test_stop_can_win_while_design_admission_waits_for_provider_capability(self):
        group = self.create()
        plan = self.plan(group)
        entered = threading.Event()
        release = threading.Event()

        def capability(*_):
            entered.set()
            if not release.wait(5):
                raise AssertionError("The stop did not finish while discovery was pending")
            return self.capability

        self.application.capabilities = capability
        with ThreadPoolExecutor(max_workers=2) as worker:
            pending = worker.submit(self.execute, plan)
            try:
                self.assertTrue(entered.wait(5))
                stopped = worker.submit(self.application.stop, group["id"], plan["id"]).result(timeout=3)
                self.assertTrue(stopped["stop_intent"])
            finally:
                release.set()
            self.assertEqual(pending.result(timeout=5)["state"], "stopped")
        self.assertEqual(self.queue, [])
        self.assertEqual(self.requests, [])

    def test_input_update_multipart_keeps_the_group_and_enforces_the_main_reference(self):
        from advanced_multiview_runtime import AdvancedMultiviewRuntime

        def multipart(fields, images):
            boundary = "orca-input-update"
            chunks = []
            for name, value in fields.items():
                chunks.append(
                    f'--{boundary}\r\nContent-Disposition: form-data; name="{name}"\r\n\r\n{value}\r\n'.encode())
            for view, data in images.items():
                chunks.extend([
                    f'--{boundary}\r\nContent-Disposition: form-data; name="{view}_image"; '
                    'filename="image.png"\r\nContent-Type: image/png\r\n\r\n'.encode(),
                    data,
                    b"\r\n",
                ])
            chunks.append(f"--{boundary}--\r\n".encode())
            body = b"".join(chunks)
            return body, f"multipart/form-data; boundary={boundary}"

        group, _ = self.initial({"front": png("navy")})
        runtime = AdvancedMultiviewRuntime.__new__(AdvancedMultiviewRuntime)
        runtime.repository, runtime.application, runtime.models = self.repository, self.application, None
        fields = {**group["saved_draft"], "expected_group_revision": str(group["revision"])}
        body, content_type = multipart(fields, {"front": png("purple")})
        handler = mock.Mock()
        handler.path = PREFIX + f"/groups/{group['id']}/inputs"
        handler.headers = {"Content-Type": content_type, "Content-Length": str(len(body))}
        handler.rfile = BytesIO(body)
        handler._require_native_client.return_value = True

        self.assertTrue(dispatch(handler, runtime, "POST"))
        status, response = handler.send_json.call_args.args
        self.assertEqual(status, 200)
        updated = response["group"]
        self.assertEqual(updated["id"], group["id"])
        self.assertEqual(updated["pending_views"], ["front"])
        self.assertEqual(updated["changed_upload_views"], ["front"])
        self.assertEqual(updated["uploads"]["front"]["sha256"], hashlib.sha256(png("purple")).hexdigest())
        handler._model_error.assert_not_called()

        body, content_type = multipart(
            {**updated["saved_draft"], "expected_group_revision": str(updated["revision"])},
            {"left": png("green")},
        )
        rejected = mock.Mock()
        rejected.path = handler.path
        rejected.headers = {"Content-Type": content_type, "Content-Length": str(len(body))}
        rejected.rfile = BytesIO(body)
        rejected._require_native_client.return_value = True
        self.assertTrue(dispatch(rejected, runtime, "POST"))
        self.assertEqual(rejected._model_error.call_args.args[:2], (400, "missing_front"))
        rejected.send_json.assert_not_called()
        current = self.repository.load(group["id"])
        self.assertEqual(current["revision"], updated["revision"])
        self.assertEqual(current["uploads"]["front"]["sha256"], updated["uploads"]["front"]["sha256"])
        self.assertIsNone(current["uploads"]["left"])

    def test_saving_a_draft_returns_current_images_and_model_projection(self):
        from advanced_multiview_runtime import AdvancedMultiviewRuntime
        group, _ = self.initial()
        runtime = AdvancedMultiviewRuntime.__new__(AdvancedMultiviewRuntime)
        runtime.repository, runtime.application, runtime.models = self.repository, self.application, None
        handler = mock.Mock()
        handler.path = PREFIX + f"/groups/{group['id']}/draft"
        handler._require_native_client.return_value = True
        handler._read_model_json.return_value = {
            "expected_group_revision": group["revision"], "draft": {"prompt": "Changed text"}}
        self.assertTrue(dispatch(handler, runtime, "PUT"))
        status, response = handler.send_json.call_args.args
        self.assertEqual(status, 200)
        saved = response["group"]
        self.assertEqual(saved["saved_draft"]["prompt"], "Changed text")
        self.assertEqual(saved["model_jobs"], [])
        self.assertFalse(saved["model_busy"])
        for view in VIEWS:
            self.assertEqual(saved["views"][view]["current"]["id"], group["views"][view]["current_version_id"])
        self.assertEqual(len(self.requests), 4)

    def test_renaming_a_group_returns_the_complete_unchanged_design_snapshot(self):
        from advanced_multiview_runtime import AdvancedMultiviewRuntime
        group, _ = self.initial()
        runtime = AdvancedMultiviewRuntime.__new__(AdvancedMultiviewRuntime)
        runtime.repository, runtime.application, runtime.models = self.repository, self.application, None
        handler = mock.Mock()
        handler.path = PREFIX + f"/groups/{group['id']}/name"
        handler._require_native_client.return_value = True
        handler._read_model_json.return_value = {
            "expected_group_revision": group["revision"], "display_name": "白色杯子"}
        self.assertTrue(dispatch(handler, runtime, "PUT"))
        status, response = handler.send_json.call_args.args
        self.assertEqual(status, 200)
        renamed = response["group"]
        self.assertEqual(renamed["display_name"], "白色杯子")
        self.assertEqual(renamed["saved_draft"]["prompt"], "One object")
        self.assertTrue(renamed["all_views_available"])
        for view in VIEWS:
            self.assertEqual(renamed["views"][view]["current"]["id"], group["views"][view]["current_version_id"])
        self.assertEqual(len(self.requests), 4)

    def test_non_advanced_routes_keep_legacy_transport_untouched(self):
        handler = mock.Mock()
        handler.path = "/v1/orcaslicer/model-jobs/text"
        self.assertFalse(dispatch(handler, mock.Mock(), "POST"))
        handler._require_native_client.assert_not_called()

    def test_negative_reconcile_rejects_a_delayed_original_submission(self):
        from advanced_multiview_runtime import AdvancedMultiviewRuntime
        group = self.create(); plan = self.plan(group)
        runtime = AdvancedMultiviewRuntime.__new__(AdvancedMultiviewRuntime)
        runtime.repository, runtime.workflow, runtime.application = self.repository, self.workflow, self.application
        runtime.models = None
        self.assertFalse(runtime.reconcile(group["id"], plan["id"])["accepted"])
        with self.assertRaises(RequestError) as result:
            self.execute(plan)
        self.assertEqual(result.exception.code, "submission_not_accepted")
        self.assertEqual(self.queue, [])
        self.assertEqual(self.requests, [])


if __name__ == "__main__":
    unittest.main()
