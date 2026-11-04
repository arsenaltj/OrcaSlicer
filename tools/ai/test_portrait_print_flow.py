"""Offline consumers of the explicit portrait policy; providers are never contacted."""
import json
import hashlib
from types import SimpleNamespace
from dataclasses import asdict
from pathlib import Path
import tempfile
import unittest
import urllib.request
from unittest import mock

import orca_ai_sidecar as sidecar
import openai_preprocessor as preprocessor
import yoyoclaw_text_gateway as text_gateway
from yoyoclaw_client import YoYoClawError
from openai_preprocessor import PrintablePaletteRecommendation, PrintablePaletteRecommendationColor
from model_creation_policy import _use_unrestricted_creation
from model_provider_gateway import ProviderGatewayError, PaidTaskAuthorization
from test_sidecar_contract import multipart_image_request, sidecar_server, temporary_environment, valid_png_bytes


def recommendation(count=6):
    colors = ("#D96B43", "#2B2422", "#F2D7B5", "#2F6B5F", "#3267A8", "#9B3F77")
    roles = ("primary", "structure", "light", "accent", "secondary", "detail")
    return PrintablePaletteRecommendation("可打印的大块人像区域", tuple(
        PrintablePaletteRecommendationColor(color, "区域颜色", role, "主体区域", "保留区域边界")
        for color, role in zip(colors[:count], roles[:count])))


class PortraitPrintFlowTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.environment = temporary_environment(ORCASLICER_AI_OUTPUT_DIR=self.directory.name)
        self.environment.__enter__()
        self.addCleanup(self.environment.__exit__, None, None, None)
        for patch in (mock.patch.dict(sidecar._JOBS, {}, clear=True),
                      mock.patch.object(sidecar, "_image_provider_status", return_value={"available": True})):
            patch.start()
            self.addCleanup(patch.stop)

    def request(self, count=6):
        return {"request_id": "frozen-portrait", "prompt": "a portrait", "style": "realistic",
                "palette_color_count": count, "portrait_print": True}

    def create(self, count=6):
        with mock.patch.object(sidecar, "recommend_printable_palette", return_value=recommendation(count)):
            with mock.patch.object(sidecar, "_submit", side_effect=lambda job, worker, *args: worker(job, *args)):
                result = sidecar._job_application().create_text_job(self.request(count))
        return sidecar._JOBS[result.payload["job"]["id"]]

    def confirm(self, job):
        return sidecar._job_application().confirm_palette(job.id, {
            "palette": [item["hex"] for item in job.palette_recommendation["colors"]],
            "palette_roles": {item["role"]: item["hex"] for item in job.palette_recommendation["colors"]}})

    def test_manual_generation_keeps_original_framing_confirmation_and_no_automatic_retry(self):
        job = self.create()
        job.portrait_print = False
        workflow = sidecar._design_workflow()
        instruction = "a full body person wearing a blue coat"
        self.assertEqual(workflow.portrait_instruction(job, instruction), instruction)
        failure = preprocessor.OpenAIPreprocessorError("HTTP 502", code="provider_http_error")
        callback = mock.Mock(side_effect=failure)
        with self.assertRaises(preprocessor.OpenAIPreprocessorError):
            workflow.portrait_call(job, "design", callback)
        callback.assert_called_once()
        image = job.directory / "manual.png"
        image.write_bytes(valid_png_bytes())
        with mock.patch.object(sidecar, "complete_yoyoclaw_once") as vision:
            workflow.check_portrait_design(job, image)
        vision.assert_not_called()
        self.assertNotIn("portrait_design", job.image_metrics)

    def test_explicit_image_failure_retries_once_but_unknown_submission_does_not(self):
        job = self.create()
        workflow = sidecar._design_workflow()
        failure = preprocessor.OpenAIPreprocessorError("HTTP 502", code="provider_http_error")
        callback = mock.Mock(side_effect=[failure, "ok"])
        self.assertEqual(workflow.portrait_call(job, "design", callback), "ok")
        self.assertEqual(callback.call_count, 2)
        self.assertEqual(job.image_metrics["portrait_attempts"]["design"], 2)
        unknown = preprocessor.OpenAIPreprocessorError("response lost", ambiguous=True)
        callback = mock.Mock(side_effect=unknown)
        with self.assertRaises(preprocessor.OpenAIPreprocessorError):
            workflow.portrait_call(job, "palette", callback)
        callback.assert_called_once()
        reopened = sidecar._load_job(job.directory)
        self.assertFalse(reopened.portrait_retry_authorized)
        callback = mock.Mock(side_effect=failure)
        with self.assertRaises(preprocessor.OpenAIPreprocessorError):
            workflow.portrait_call(reopened, "new-stage", callback)
        callback.assert_called_once()

    def test_half_body_review_is_hash_bound_and_distinguishes_nonportrait_from_unavailable(self):
        job = self.create()
        image = job.directory / "style-preview-raw.png"; image.write_bytes(valid_png_bytes())
        job.raw_preview_path = image
        job.image_metrics["model_input_quality"] = {"blockers": ["low_likeness"], "warnings": []}
        for status, response in (("pass", '{"single_person":true,"half_body":true,"head_visible":true}'),
                ("not_portrait", '{"single_person":false,"half_body":false,"head_visible":true}'),
                ("unavailable", "not valid json")):
            with self.subTest(status=status), mock.patch.object(sidecar, "complete_yoyoclaw_once", return_value=response):
                sidecar._design_workflow().check_portrait_design(job, image)
                self.assertEqual(job.image_metrics["portrait_design"]["status"], status)
                self.assertEqual(job.image_metrics["portrait_design"]["sha256"], hashlib.sha256(image.read_bytes()).hexdigest())
                self.assertEqual(job.image_metrics["model_input_quality"]["blockers"], [])
        instruction = sidecar._design_workflow().portrait_instruction(job, "a full body person")
        self.assertIn("half-body", instruction); self.assertIn("never add pelvis, legs, feet", instruction)
        self.assertIn("Orca adds a geometric base after 3D generation", instruction)

    def test_model_generation_retries_only_matching_explicit_terminal_task_once(self):
        for failure_kind in ("terminal_then_success", "terminal_twice", "unknown", "foreign"):
            with self.subTest(failure_kind=failure_kind), mock.patch.dict(sidecar._JOBS, {}, clear=True):
                job = self.create()
                gateway = mock.Mock()
                def start(_request, **kwargs):
                    kwargs["authorization"].consume("tripo", "model_generation")
                    return SimpleNamespace(task_id=f"model-{gateway.start_or_reuse_model_task.call_count}", reused=False)
                gateway.start_or_reuse_model_task.side_effect = start
                first = ProviderGatewayError("provider error 1001", code="provider_task_failed", category="provider",
                    provider="tripo", operation="model_generation", terminal=failure_kind != "unknown",
                    ambiguous=failure_kind == "unknown", task_id="foreign" if failure_kind == "foreign" else "model-1")
                second = ProviderGatewayError("provider error 1001 again", code="provider_task_failed", category="provider",
                    provider="tripo", operation="model_generation", terminal=True, task_id="model-2")
                gateway.wait_for_task.side_effect = [first, second if failure_kind == "terminal_twice" else {}]
                candidate = job.directory / "candidate.obj"; candidate.write_text("v 0 0 0\n", encoding="utf-8")
                with mock.patch.object(sidecar, "_model_gateway", return_value=gateway), \
                        mock.patch.object(sidecar, "_ensure_portrait_multiview", return_value=None), \
                        mock.patch.object(sidecar, "_download_generation_artifact", return_value=candidate), \
                        mock.patch.object(sidecar, "_validate_obj_topology", return_value=(1000000, 0, 0)), \
                        mock.patch.object(sidecar, "_promote_attempt_artifact"), \
                        mock.patch.object(sidecar, "_automatic_visual_review", return_value=None), \
                        mock.patch.object(sidecar, "_write_job_color_intent"):
                    sidecar._model_workflow().generate(job, "portrait", False, PaidTaskAuthorization.confirmed("test", "tripo"))
                self.assertEqual(gateway.start_or_reuse_model_task.call_count, 2 if failure_kind.startswith("terminal") else 1)
                self.assertEqual(job.state, "ready" if failure_kind == "terminal_then_success" else "failed")
                if failure_kind.startswith("terminal"):
                    self.assertIn("model-1", sidecar._public_job(job)["terminal_task_ids"])
                    self.assertEqual(job.image_metrics["portrait_attempts"]["model_retries"], 1)

    def test_paid_model_submission_requires_review_of_the_exact_image(self):
        job = self.create()
        with mock.patch.object(sidecar, "_submit"):
            self.confirm(job)
        job.state = "awaiting_confirmation"
        image = job.directory / "style-preview-raw.png"; image.write_bytes(valid_png_bytes())
        job.raw_preview_path = image;job.image_metrics["design_reference"] = "ai-design-v1"
        job.image_metrics["portrait_design"] = {"status": "pass", "sha256": "wrong"}
        with mock.patch.object(sidecar, "_submit") as submit, mock.patch.object(sidecar, "_validate_image_file"), \
                mock.patch.object(sidecar, "_assess_reference_advice"):
            with self.assertRaises(sidecar.RequestError) as error:
                sidecar._job_application().generate(job.id, {"prepared_prompt": "portrait"})
        self.assertEqual(error.exception.code, "portrait_review_required"); submit.assert_not_called()

    def test_pending_retry_does_not_attach_the_retired_task_to_the_new_attempt(self):
        job = self.create()
        job.attempts = [{"generation_task_id": "old", "terminal_task_id": "old"}, {"status": "creating"}]
        waiting = sidecar._public_job(job)
        self.assertEqual(waiting["attempt"], 1)
        self.assertEqual(waiting["provider_tasks"]["generation_task_id"], "old")
        job.attempts[1]["generation_task_id"] = "new"
        running = sidecar._public_job(job)
        self.assertEqual(running["attempt"], 2)
        self.assertEqual(running["provider_tasks"]["generation_task_id"], "new")
        self.assertIn("old", running["terminal_task_ids"])

    def test_conversion_retries_once_without_regenerating_geometry_and_preserves_ambiguity(self):
        for kind in ("success_after_failure", "failure_twice", "unknown", "resume"):
            with self.subTest(kind=kind), mock.patch.dict(sidecar._JOBS, {}, clear=True):
                job = self.create(); job.output_format = "obj"
                job.attempts = [{"generation_task_id": "geometry"}]
                gateway = mock.Mock()
                def convert(*args, **kwargs):
                    return SimpleNamespace(task_id=f"convert-{gateway.start_or_reuse_conversion.call_count}", reused=False)
                gateway.start_or_reuse_conversion.side_effect = convert
                failure = lambda task: ProviderGatewayError("conversion failed", code="provider_task_failed",
                    category="provider", provider="tripo", terminal=kind != "unknown", ambiguous=kind == "unknown", task_id=task)
                gateway.wait_for_task.side_effect = [failure("convert-1"),
                    failure("convert-2") if kind == "failure_twice" else {}]
                candidate = job.directory / "prepared.obj"
                with mock.patch.object(sidecar, "_MODEL_PROVIDER_GATEWAY", gateway), \
                        mock.patch.object(sidecar, "_prepare_obj_artifact", return_value=candidate):
                    if kind == "success_after_failure":
                        self.assertEqual(sidecar._download_conversion(job, "geometry", "obj"), candidate)
                    else:
                        with self.assertRaises(ProviderGatewayError):
                            sidecar._download_conversion(job, "geometry", "obj", resume=kind == "resume")
                self.assertEqual(gateway.start_or_reuse_conversion.call_count, 2 if kind in
                    {"success_after_failure", "failure_twice"} else 1)
                gateway.start_or_reuse_model_task.assert_not_called()
                self.assertEqual(job.attempts[0]["generation_task_id"], "geometry")
                if kind in {"success_after_failure", "failure_twice"}:
                    self.assertEqual(job.attempts[0]["failed_conversion_task_ids"], ["convert-1"])

    def test_final_four_five_six_colors_survive_confirm_generate_and_restart(self):
        for count in (4, 5, 6):
            with self.subTest(count=count), mock.patch.dict(sidecar._JOBS, {}, clear=True):
                job = self.create(count)
                self.assertEqual(job.state, "awaiting_palette_confirmation")
                self.assertFalse(job.design_submission_started)
                with mock.patch.object(sidecar, "_submit") as submit:
                    self.confirm(job)
                    self.confirm(job)
                submit.assert_called_once()
                self.assertEqual(len(job.palette), count)
                self.assertTrue(job.design_submission_started)
                before = tuple(job.palette)
                _use_unrestricted_creation(job)
                self.assertEqual(job.palette, before)
                sidecar._persist_job(job)
                restored = sidecar._load_job(job.directory)
                self.assertIsNotNone(restored)
                self.assertEqual(restored.palette, before)
                self.assertTrue(restored.portrait_print)
                self.assertTrue(restored.design_submission_started)
                self.assertEqual(restored.creation_request_id, "frozen-portrait")

    def test_portrait_obj_keeps_topology_for_native_workbench_repair(self):
        raw = Path(self.directory.name) / "incoming.obj"
        raw.write_text("v 0 0 0 1 0 0\nv 20 0 0 0 1 0\nv 0 20 0 0 0 1\nv 0 0 20 1 1 0\n"
                       "f 1 3 2\nf 1 2 4\nf 2 3 4\nf 3 1 4\n", encoding="utf-8")
        with mock.patch("model_artifact_workflow._remove_small_detached_obj_components") as remove, \
                mock.patch("model_artifact_workflow._repair_small_obj_topology_defects") as repair, \
                mock.patch.object(sidecar, "_validate_artifact"), \
                mock.patch.object(sidecar, "analyze_printable_obj", return_value={}), \
                mock.patch.object(sidecar, "write_model_quality_report"):
            result = sidecar._prepare_obj_artifact(raw, Path(self.directory.name), (), {}, native_repair_deferred=True)
        remove.assert_not_called(); repair.assert_not_called()
        faces = [line for line in result.read_text().splitlines() if line.startswith("f ")]
        self.assertEqual(len(faces), 4)

    def test_duplicate_creation_reuses_real_job_and_rejects_changed_inputs(self):
        job = self.create()
        with mock.patch.object(sidecar, "_submit") as submit:
            result = sidecar._job_application().create_text_job(self.request())
            self.assertEqual(result.payload["job"]["id"], job.id)
            altered = {**self.request(), "prompt": "different portrait"}
            with self.assertRaises(sidecar.RequestError) as error:
                sidecar._job_application().create_text_job(altered)
            self.assertEqual(error.exception.code, "request_conflict")
        submit.assert_not_called()
        self.assertEqual(len(sidecar._JOBS), 1)

    def test_existing_image_only_configuration_uses_yoyoclaw_text_and_vision_for_palette(self):
        for count in (4, 5, 6):
            for image_mode in (False, True):
                with self.subTest(count=count, image=image_mode), \
                        mock.patch.dict(sidecar._JOBS, {}, clear=True), \
                        temporary_environment(OPENAI_API_KEY="", OPENAI_PRO_API="test-image-only-key",
                                              OPENAI_PRO_URL="https://image.example/v1"), \
                        mock.patch.object(preprocessor, "_config", side_effect=AssertionError("legacy text channel")), \
                        mock.patch.object(text_gateway, "YoYoClawTextGateway") as gateway, \
                        mock.patch.object(sidecar, "_preprocess_text_job") as design, \
                        mock.patch.object(sidecar, "_preprocess_image_job") as edit, \
                        mock.patch.object(sidecar, "_submit", side_effect=lambda job, worker, *args: worker(job, *args)):
                    palette = recommendation(count)
                    gateway.return_value.complete.return_value = json.dumps({
                        "summary": palette.summary, "colors": [asdict(color) for color in palette.colors]})
                    application = sidecar._job_application()
                    if image_mode:
                        fields = {key: str(value).lower() if isinstance(value, bool) else str(value)
                                  for key, value in self.request(count).items() if key != "prompt"}
                        fields["instruction"] = "a portrait"
                        response = application.create_image_job(fields, valid_png_bytes(), "image/png")
                    else:
                        response = application.create_text_job(self.request(count))
                    job = sidecar._JOBS[response.payload["job"]["id"]]
                    self.assertEqual(job.state, "awaiting_palette_confirmation")
                    self.assertEqual(len(job.palette_recommendation["colors"]), count)
                    self.assertFalse(job.design_submission_started)
                    self.assertFalse(job.model_submission_started)
                    self.assertEqual(preprocessor._image_config().source, "pro")
                    gateway.return_value.complete.assert_called_once()
                    call = gateway.return_value.complete.call_args
                    self.assertEqual(call.kwargs["requires_image"], image_mode)
                    content = call.args[0][1]["content"]
                    if image_mode:
                        self.assertTrue(content[1]["image_url"]["url"].startswith("data:image/jpeg;base64,"))
                        self.assertEqual(job.input_path.read_bytes(), valid_png_bytes())
                    else:
                        self.assertEqual(content, "a portrait")
                    design.assert_not_called()
                    edit.assert_not_called()

    def test_palette_failure_never_retries_switches_channel_or_starts_generation(self):
        failures = [
            YoYoClawError("response lost", code="provider_connection_failed", category="network", ambiguous=True),
            YoYoClawError("unauthorized", code="authentication_failed", category="authentication", status=401),
            "invalid palette JSON",
        ]
        for failure in failures:
            with self.subTest(failure=str(failure)), mock.patch.dict(sidecar._JOBS, {}, clear=True), \
                    mock.patch.object(text_gateway, "YoYoClawTextGateway") as gateway, \
                    mock.patch.object(preprocessor, "complete_text") as legacy, \
                    mock.patch.object(preprocessor, "complete_vision") as legacy_vision, \
                    mock.patch.object(sidecar, "_preprocess_text_job") as design, \
                    mock.patch.object(sidecar, "_generate_job") as model, \
                    mock.patch.object(sidecar, "_submit", side_effect=lambda job, worker, *args: worker(job, *args)):
                if isinstance(failure, Exception):
                    gateway.return_value.complete.side_effect = failure
                else:
                    gateway.return_value.complete.return_value = failure
                application = sidecar._job_application()
                response = application.create_text_job(self.request())
                job = sidecar._JOBS[response.payload["job"]["id"]]
                duplicate = application.create_text_job(self.request())
                self.assertEqual(duplicate.payload["job"]["id"], job.id)
                self.assertEqual(job.state, "failed")
                self.assertFalse(job.design_submission_started)
                self.assertFalse(job.model_submission_started)
                restored = sidecar._load_job(job.directory)
                self.assertEqual(restored.state, "failed")
                self.assertFalse(restored.preprocess_failure["retryable"])
                gateway.return_value.complete.assert_called_once()
                legacy.assert_not_called()
                legacy_vision.assert_not_called()
                design.assert_not_called()
                model.assert_not_called()

    def test_invalid_recommendation_cannot_start_a_design_or_model(self):
        for count in (3, 7):
            with self.subTest(count=count), self.assertRaises(sidecar.RequestError):
                sidecar._job_application().create_text_job(self.request(count))
        with mock.patch.object(sidecar, "recommend_printable_palette", return_value=recommendation(3)), \
                mock.patch.object(sidecar, "_preprocess_text_job") as design, \
                mock.patch.object(sidecar, "_submit", side_effect=lambda job, worker, *args: worker(job, *args)):
            response = sidecar._job_application().create_text_job(self.request())
        self.assertEqual(response.payload["job"]["state"], "failed")
        design.assert_not_called()

    def test_lost_design_dispatch_never_repeats_after_reopen(self):
        job = self.create()
        with mock.patch.object(sidecar, "_submit", side_effect=sidecar.RequestError("unavailable", "offline", 503)):
            with self.assertRaises(sidecar.RequestError):
                self.confirm(job)
        restored = sidecar._load_job(job.directory)
        sidecar._JOBS[job.id] = restored
        with mock.patch.object(sidecar, "_submit") as submit:
            self.confirm(restored)
        submit.assert_not_called()
        self.assertTrue(restored.design_submission_started)
        self.assertEqual(restored.state, "failed")

    def test_second_model_submission_is_rejected_even_without_provider_id(self):
        job = self.create()
        with mock.patch.object(sidecar, "_submit"):
            self.confirm(job)
        job.model_submission_started = True
        job.state = "awaiting_confirmation"
        with mock.patch.object(sidecar, "_submit") as submit:
            with self.assertRaises(sidecar.RequestError) as error:
                sidecar._job_application().generate(job.id, {"prepared_prompt": "portrait"})
        self.assertEqual(error.exception.code, "portrait_task_reserved")
        submit.assert_not_called()

    def test_restart_requires_manual_continuation_of_the_saved_task(self):
        job = self.create()
        job.state, job.phase = "queued", "resuming"
        job.attempts = [{"generation_task_id": "real-provider-task"}]
        with mock.patch.object(sidecar, "_submit") as submit:
            sidecar._resume_restored_jobs([job])
        submit.assert_not_called()
        self.assertEqual(job.phase, "portrait_resume_required")
        self.assertEqual(job.attempts[0]["generation_task_id"], "real-provider-task")

    def test_multipart_opt_in_recommends_before_any_image_submission(self):
        fields = {key: str(value).lower() if isinstance(value, bool) else str(value)
                  for key, value in self.request().items() if key != "prompt"}
        fields["instruction"] = "a portrait"
        body, content_type = multipart_image_request(fields, valid_png_bytes())
        with sidecar_server(sidecar.Handler) as port, \
                mock.patch.object(sidecar, "_submit") as submit:
            request = urllib.request.Request(
                f"http://127.0.0.1:{port}/v1/orcaslicer/model-jobs/recommend-image-palette",
                data=body, method="POST", headers={"X-OrcaSlicer-Client": "native", "Content-Type": content_type})
            with urllib.request.build_opener(urllib.request.ProxyHandler({})).open(request, timeout=5) as response:
                public = json.loads(response.read())["job"]
        self.assertTrue(public["portrait_print"])
        self.assertEqual(public["creation_request_id"], fields["request_id"])
        self.assertEqual(public["state"], "recommending_palette")
        self.assertIs(submit.call_args.args[1], sidecar._recommend_palette_job)

    def test_obj_conversion_preserves_provider_appearance_and_excludes_color_intent(self):
        job = self.create()
        job.palette = tuple(color.hex for color in recommendation().colors)
        job.output_format = "obj"
        job.attempts = [{"generation_task_id": "model", "conversion_task_id": "conversion"}]
        def download(_result, destination, *_args, **_kwargs):
            destination.write_bytes(b"original-provider-obj")
        with mock.patch.object(sidecar, "_MODEL_PROVIDER_GATEWAY") as gateway, \
                mock.patch.object(sidecar, "_prepare_obj_artifact", return_value=job.directory / "model.obj") as prepare:
            gateway.download_artifact.side_effect = download
            sidecar._model_artifact_workflow().download_conversion(job, "model", "obj", resume=True)
        self.assertEqual(prepare.call_args.args[2:], ((), {}))
        with mock.patch.object(sidecar, "write_color_intent_manifest") as intent:
            sidecar._write_job_color_intent(job, job.directory / "model.obj")
        intent.assert_not_called()
        self.assertIsNone(job.color_intent_path)


if __name__ == "__main__":
    unittest.main()
