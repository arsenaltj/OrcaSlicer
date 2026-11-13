from __future__ import annotations

from dataclasses import fields, replace
from io import BytesIO
from pathlib import Path
import tempfile
import threading
import unittest
import uuid
from unittest.mock import Mock

from PIL import Image
from model_contracts import Job, RequestError
from model_job_application import ModelJobApplication, ModelJobApplicationPorts
from model_job_repository import _load_job, _persist_job
from model_request import _validate_image_data


class SmartGenerationTests(unittest.TestCase):
    def test_removed_creation_requests_never_submit_a_provider_task(self):
        with tempfile.TemporaryDirectory() as directory:
            app = self.make_application(Path(directory), {})
            for mode in ("smart", "advanced"):
                with self.subTest(mode=mode), self.assertRaises(RequestError) as caught:
                    app.create_text_job({**self.request(), "workflow_mode": mode, "portrait_print": True})
                self.assertEqual(caught.exception.status, 410)
            with self.assertRaises(RequestError) as caught:
                app.create_image_job({**self.request(), "portrait_print": "true"}, b"unused", "image/png")
            self.assertEqual(caught.exception.status, 410)
            with self.assertRaises(RequestError) as caught:
                app.execute_job("retry-portrait", "missing", {})
            self.assertEqual(caught.exception.status, 404)
            self.submit.assert_not_called()

    def test_retired_history_keeps_assets_without_reauthorizing_execution(self):
        import json
        from model_contracts import JOB_STATE_FILENAME
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            app = self.make_application(root, {})
            first = app.create_text_job(self.request())
            job = app.ports.jobs[first.payload["job"]["id"]]
            model = job.directory / "model.glb"
            model.write_bytes(b"existing model")
            job.artifact_path = model
            job.artifact_format = "glb"
            job.state = "queued"
            job.phase = "resuming"
            _persist_job(job)
            state_path = job.directory / JOB_STATE_FILENAME
            record = json.loads(state_path.read_text(encoding="utf-8"))
            record["portrait_print"] = True
            state_path.write_text(json.dumps(record), encoding="utf-8")
            restored = _load_job(job.directory)
            self.assertTrue(restored.retired_workflow)
            self.assertEqual(restored.state, "stopped")
            self.assertEqual(restored.artifact_path, model.resolve())
            restarted = self.make_application(root, {restored.id: restored})
            for command in ("generate", "confirm-palette", "retry-preparation", "retexture"):
                with self.subTest(command=command), self.assertRaises(RequestError) as caught:
                    restarted.execute_job(command, restored.id, {})
                self.assertEqual(caught.exception.status, 410)
            _persist_job(restored)
            self.assertTrue(_load_job(job.directory).retired_workflow)
            self.assertEqual(model.read_bytes(), b"existing model")
            self.submit.assert_not_called()

    def make_application(self, root, jobs):
        def denied(*args, **kwargs):
            raise AssertionError("Unexpected provider or capability invocation")

        def new_job(source, palette, roles, style, custom, print_settings, **kwargs):
            job_id = str(uuid.uuid4())
            directory = root / job_id
            directory.mkdir()
            return Job(job_id, source, directory, style=style, custom_style=custom)

        values = {field.name: denied for field in fields(ModelJobApplicationPorts)}
        self.submit = Mock()
        values.update(jobs=jobs, lock=threading.RLock(), new_job=new_job,
                      persist=_persist_job, present=lambda j: {"id": j.id, "state": j.state},
                      submit=self.submit, preprocess_text=denied, preprocess_image=denied,
                      image_provider_status=lambda *a: {"available": True},
                      get_job=jobs.get, fallback_enabled=lambda: False,
                      latest_task_id=lambda j: "", cleanup=lambda j: None,
                      can_retry_unsubmitted=lambda j: False)
        return ModelJobApplication(ModelJobApplicationPorts(**values))

    def request(self):
        flow = str(uuid.uuid4())
        return {"workflow_mode": "smart", "flow_id": flow, "request_id": flow,
                "prompt": "a green vase", "style": "realistic"}

    def test_replayed_creation_survives_restart_without_a_second_submission(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            jobs = {}
            app = self.make_application(root, jobs)
            request = self.request()
            first = app.create_text_job(request)
            self.assertEqual(app.create_text_job(request).payload, first.payload)
            self.assertEqual(self.submit.call_count, 1)
            job = next(iter(jobs.values()))
            loaded = _load_job(job.directory)
            self.assertIsNotNone(loaded)
            restarted = self.make_application(root, {loaded.id: loaded})
            self.assertEqual(restarted.create_text_job(request).payload, first.payload)
            self.submit.assert_not_called()
            self.assertEqual(loaded.input_fingerprint, job.input_fingerprint)
            with self.assertRaises(RequestError) as caught:
                restarted.create_text_job({**request, "prompt": "another subject"})
            self.assertEqual(caught.exception.code, "request_conflict")

    def test_smart_creation_requires_gpt_and_never_falls_back(self):
        with tempfile.TemporaryDirectory() as directory:
            app = self.make_application(Path(directory), {})
            with self.assertRaises(RequestError):
                app.create_text_job({**self.request(), "design_provider": "yoyoclaw"})
            app.ports = replace(app.ports, image_provider_status=lambda *args: {"available": False}, fallback_enabled=lambda: True)
            with self.assertRaises(RequestError):
                app.create_text_job(self.request())
            self.submit.assert_not_called()

    def test_stop_intent_and_progress_are_durable_and_reference_retry_does_not_generate(self):
        with tempfile.TemporaryDirectory() as directory:
            jobs = {}
            app = self.make_application(Path(directory), jobs)
            app.create_text_job(self.request())
            job = next(iter(jobs.values()))
            job.preview_path = job.directory / "preview.png"
            Image.new("RGBA", (64, 64), (10, 20, 30, 100)).save(job.preview_path)
            job.state = "awaiting_confirmation"
            job.smart_progress = 29
            app.stop(job.id, {})
            restored = _load_job(job.directory)
            self.assertTrue(restored.smart_stop_intent)
            self.assertEqual(restored.smart_progress, 29)
            self.submit.reset_mock()
            app.retry_preparation(job.id, {})
            self.submit.assert_not_called()
            self.assertEqual(job.state, "awaiting_confirmation")
            self.assertFalse(job.smart_stop_intent)

    def test_static_webp_decodes_and_animated_or_corrupt_webp_is_rejected(self):
        output = BytesIO()
        Image.new("RGBA", (96, 64), (16, 32, 64, 100)).save(output, "WEBP", lossless=True)
        data = output.getvalue()
        self.assertEqual(_validate_image_data(data, minimum_edge=64).content_type, "image/webp")
        with self.assertRaises(ValueError):
            _validate_image_data(data[:-8], minimum_edge=64)
        animated = BytesIO()
        Image.new("RGB", (64, 64), "red").save(animated, "WEBP", save_all=True,
            append_images=[Image.new("RGB", (64, 64), "blue")], duration=100, loop=0)
        with self.assertRaisesRegex(ValueError, "Animated WebP"):
            _validate_image_data(animated.getvalue(), minimum_edge=64)


    def test_concurrent_replayed_creation_submits_only_once(self):
        from concurrent.futures import ThreadPoolExecutor
        with tempfile.TemporaryDirectory() as directory:
            app = self.make_application(Path(directory), {})
            request = self.request()
            with ThreadPoolExecutor(max_workers=4) as executor:
                responses = list(executor.map(lambda _: app.create_text_job(request), range(8)))
            self.assertEqual(len({r.payload["job"]["id"] for r in responses}), 1)
            self.assertEqual(self.submit.call_count, 1)

    def test_saved_model_receipt_is_replayed_and_stop_requires_explicit_resume(self):
        with tempfile.TemporaryDirectory() as directory:
            jobs = {}
            app = self.make_application(Path(directory), jobs)
            app.create_text_job(self.request())
            job = next(iter(jobs.values()))
            job.smart_model_request_id = job.flow_id + ":model:1"
            job.state = "queued"
            self.submit.reset_mock()
            app.generate(job.id, {"prepared_prompt": ""})
            self.submit.assert_not_called()
            job.state = "stopped"; job.smart_stop_intent = True
            with self.assertRaises(RequestError) as caught:
                app.generate(job.id, {"prepared_prompt": ""})
            self.assertEqual(caught.exception.code, "stopped")
            self.submit.assert_not_called()

    def test_explicit_preparation_retry_preserves_failed_evidence_and_never_submits_3d(self):
        with tempfile.TemporaryDirectory() as directory:
            jobs = {}
            app = self.make_application(Path(directory), jobs)
            app.create_text_job(self.request())
            job = next(iter(jobs.values()))
            job.preview_path = job.directory / "preview.png"
            Image.new("RGB", (64, 64), "green").save(job.preview_path)
            evidence = job.preview_path
            job.state = "failed"
            self.submit.reset_mock()
            app.retry_preparation(job.id, {"regenerate": True})
            self.submit.assert_called_once()
            self.assertIs(self.submit.call_args.args[1], app.ports.preprocess_text)
            self.assertIsNone(job.preview_path)
            self.assertTrue(evidence.is_file())
            self.assertEqual(job.state, "preprocessing")

    def test_ambiguous_preparation_failure_is_not_blindly_resubmitted(self):
        with tempfile.TemporaryDirectory() as directory:
            jobs = {}
            app = self.make_application(Path(directory), jobs)
            app.create_text_job(self.request())
            job = next(iter(jobs.values()))
            job.state = "failed"; job.preprocess_failure = {"ambiguous": True}
            self.submit.reset_mock()
            with self.assertRaises(RequestError) as caught:
                app.retry_preparation(job.id, {})
            self.assertEqual(caught.exception.code, "preparation_ambiguous")
            self.submit.assert_not_called()

    def test_webp_keeps_original_bytes_and_uses_lossless_png_for_preparation(self):
        with tempfile.TemporaryDirectory() as directory:
            jobs = {}
            app = self.make_application(Path(directory), jobs)
            original = Image.new("RGBA", (96, 64), (16, 32, 64, 100))
            encoded = BytesIO(); original.save(encoded, "WEBP", lossless=True)
            data = encoded.getvalue()
            request = self.request(); request["instruction"] = request.pop("prompt")
            app.create_image_job(request, data, "image/webp")
            job = next(iter(jobs.values()))
            self.assertEqual(job.input_path.suffix, ".webp")
            self.assertEqual(job.input_path.read_bytes(), data)
            derivative = self.submit.call_args.args[2]
            self.assertEqual(derivative.suffix, ".png")
            with Image.open(derivative) as decoded:
                self.assertEqual(decoded.convert("RGBA").tobytes(), original.tobytes())
            loaded = _load_job(job.directory)
            self.assertEqual(loaded.input_path.read_bytes(), data)


if __name__ == "__main__":
    unittest.main()
