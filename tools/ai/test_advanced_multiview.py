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

    def catalog(self, role):
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

    def test_first_generation_always_produces_four_named_views(self):
        cases = [()] + [("front",) + extra for count in range(4) for extra in combinations(VIEWS[1:], count)]
        for uploaded in cases:
            with self.subTest(uploaded=uploaded):
                self.requests.clear()
                images = {v: png() for v in uploaded}
                group, _ = self.initial(images)
                self.assertEqual([r.view for r in self.requests], list(VIEWS))
                self.assertTrue(self.repository.snapshot(group["id"])["all_views_available"])
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

    def test_missing_view_uses_main_upload_and_design_without_other_view_context(self):
        self.initial({"front": png(), "back": png()})
        requests = {request.view: request for request in self.requests}
        self.assertEqual([role for role, _ in requests["left"].references],
                         ["front_upload", "front_design"])
        self.assertEqual([role for role, _ in requests["back"].references], ["back_upload"])

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
        group, _ = self.initial({"front": png()})
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
        self.assertEqual({view: updated["views"][view]["current_version_id"] for view in VIEWS}, before)

        self.requests.clear()
        plan = self.plan(updated, "edit", prompt="Use the replacement", target_views=["front"],
                         base_versions={"front": before["front"]})
        self.execute(plan)
        self.drain()
        self.assertEqual([request.view for request in self.requests], ["front"])
        self.assertEqual([role for role, _ in self.requests[0].references],
                         ["front_upload", "front_design"])
        current = self.repository.load(group["id"])
        self.assertEqual(current["pending_views"], [])
        self.assertEqual(current["changed_upload_views"], [])
        for view in VIEWS[1:]:
            self.assertEqual(current["views"][view]["current_version_id"], before[view])

    def test_added_upload_is_required_alongside_extra_user_selection(self):
        group, _ = self.initial({"front": png("navy")})
        front = self.repository.path(group["id"], group["uploads"]["front"]["path"]).read_bytes()
        before = {view: group["views"][view]["current_version_id"] for view in VIEWS}
        updated = self.application.update_inputs(group["id"], {
            "expected_group_revision": group["revision"], **group["saved_draft"]},
            {"front": front, "left": png("purple")})
        self.assertEqual(updated["pending_views"], ["left"])

        plan = self.plan(updated, "edit", prompt="Also refine right", target_views=["right"],
                         base_versions={"right": before["right"]})
        self.assertEqual(plan["targets"], ["left", "right"])
        self.requests.clear()
        self.execute(plan)
        self.drain()
        self.assertEqual([request.view for request in self.requests], ["left", "right"])
        self.assertEqual([role for role, _ in self.requests[0].references],
                         ["left_upload", "left_design"])

    def test_replaced_main_upload_is_available_to_user_selected_side_views(self):
        group, _ = self.initial({"front": png("navy")})
        before = {view: group["views"][view]["current_version_id"] for view in VIEWS}
        updated = self.application.update_inputs(group["id"], {
            "expected_group_revision": group["revision"], **group["saved_draft"]},
            {"front": png("purple")})
        plan = self.plan(updated, "edit", prompt="Align the right view", target_views=["right"],
                         base_versions={"right": before["right"]})
        self.assertEqual(plan["targets"], ["front", "right"])
        self.requests.clear()
        self.execute(plan)
        self.drain()
        requests = {request.view: request for request in self.requests}
        self.assertEqual([role for role, _ in requests["right"].references],
                         ["right_design", "front_upload"])

    def test_style_change_marks_all_views_pending_without_creating_a_group(self):
        group, _ = self.initial({"front": png()})
        draft = {**group["saved_draft"], "style": "realistic"}
        updated = self.application.save_draft(group["id"], {
            "expected_group_revision": group["revision"], "draft": draft})
        self.assertEqual(updated["id"], group["id"])
        self.assertEqual(updated["pending_views"], list(VIEWS))
        self.assertEqual(updated["changed_upload_views"], [])
        self.assertEqual(len(self.repository.list_groups()), 1)

        before = {view: updated["views"][view]["current_version_id"] for view in VIEWS}
        plan = self.plan(updated, "edit", **draft, target_views=["back"],
                         base_versions={"back": before["back"]})
        self.assertEqual(plan["targets"], list(VIEWS))
        self.execute(plan)
        self.drain()
        self.assertEqual(self.repository.load(group["id"])["pending_views"], [])

    def test_failed_initial_view_retries_with_the_current_style_in_the_same_group(self):
        self.failure["left"] = ProviderGatewayError("rejected", code="rejected", category="request")
        group, initial = self.initial({"front": png()})
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

    def test_input_update_keeps_required_front_and_durable_group_on_failure(self):
        group, _ = self.initial({"front": png()})
        with self.assertRaises(RequestError) as error:
            self.application.update_inputs(group["id"], {
                "expected_group_revision": group["revision"], **group["saved_draft"]},
                {"left": png("purple")})
        self.assertEqual(error.exception.code, "missing_front")
        current = self.repository.load(group["id"])
        self.assertEqual(current["revision"], group["revision"])
        self.assertIsNotNone(current["uploads"]["front"])
        self.assertIsNone(current["uploads"]["left"])

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
        group, plan = self.initial({"front": png()})
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
        group, _ = self.initial({"front": png()})
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
        for images in ({"front": png()}, {}):
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
        rejected._model_error.assert_called_once()
        self.assertEqual(rejected._model_error.call_args.args[1], "missing_front")
        self.assertEqual(self.repository.load(group["id"])["revision"], updated["revision"])

    def test_saving_a_draft_returns_current_images_and_model_projection(self):
        from advanced_multiview_runtime import AdvancedMultiviewRuntime
        group, _ = self.initial({"front": png()})
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
        group, _ = self.initial({"front": png()})
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
