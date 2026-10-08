"""Saved 2D designs can start a new draft without changing history or calling providers."""
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock

from PIL import Image
import orca_ai_sidecar as sidecar


class ImageHistoryReuseTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        for patch in (
            mock.patch.object(sidecar, "_model_output_root", return_value=self.root),
            mock.patch("urllib.request.OpenerDirector.open", side_effect=AssertionError("offline test")),
            mock.patch.object(sidecar, "_submit", side_effect=AssertionError("no generation when reusing a design")),
            mock.patch.object(sidecar.PaidTaskAuthorization, "confirmed", side_effect=AssertionError("no paid authorization")),
        ):
            patch.start()
            self.addCleanup(patch.stop)
        self.source = sidecar._new_job("image", style="custom", custom_style="paper sculpture")
        self.source.input_path = self.source.directory / "input.png"
        self.source.preview_path = self.source.directory / "preview.png"
        Image.new("RGB", (128, 128), "red").save(self.source.input_path)
        Image.new("RGB", (128, 128), "blue").save(self.source.preview_path)
        self.source.raw_preview_path = self.source.preview_path
        self.source.model_reference_path = self.source.preview_path
        self.source.user_prompt = "cat with a blue scarf"
        self.source.state = "ready"
        self.source.artifact_path = self.source.directory / "model.glb"
        self.source.artifact_path.write_bytes(b"existing model")
        self.source.attempts = [{"generation_task_id": "existing-paid-task"}]
        sidecar._persist_job(self.source)

    def test_saved_context_survives_restart_without_copying_paid_model_identity(self):
        before = (self.source.directory / "job.json").read_bytes()
        child = sidecar._reuse_design_job(self.source)
        self.assertNotEqual(child.id, self.source.id)
        self.assertEqual(child.state, "awaiting_confirmation")
        self.assertEqual(child.attempts, [])
        self.assertIsNone(child.artifact_path)
        self.assertEqual(child.style, "custom")
        self.assertEqual(child.custom_style, "paper sculpture")
        self.assertEqual(child.user_prompt, self.source.user_prompt)
        self.assertEqual(child.preview_path.read_bytes(), self.source.preview_path.read_bytes())
        self.assertNotEqual(child.preview_path, self.source.preview_path)
        restored = sidecar._load_job(child.directory)
        self.assertIsNotNone(restored)
        self.assertEqual(restored.source_design_job_id, self.source.id)
        self.assertEqual((self.source.directory / "job.json").read_bytes(), before)
        self.assertEqual(self.source.artifact_path.read_bytes(), b"existing model")

    def test_pending_stopped_and_failed_designs_are_reusable(self):
        for state in ("awaiting_confirmation", "stopped", "failed", "cancelled"):
            with self.subTest(state=state):
                self.source.state = state
                self.assertEqual(sidecar._reuse_design_job(self.source).state, "awaiting_confirmation")

    def test_active_jobs_cannot_be_forked(self):
        for state in ("preprocessing", "queued", "generating"):
            self.source.state = state
            with self.assertRaises(sidecar.RequestError):
                sidecar._reuse_design_job(self.source)

    def test_missing_or_outside_images_do_not_create_a_draft(self):
        outside = self.root / "external.png"
        Image.new("RGB", (128, 128), "green").save(outside)
        for path in (outside, self.source.directory / "missing.png"):
            self.source.model_reference_path = path
            with self.assertRaises(sidecar.RequestError):
                sidecar._reuse_design_job(self.source)
        self.assertEqual(len(list(self.root.glob("*/job.json"))), 1)

    def test_route_accepts_only_an_existing_design_and_registers_a_local_draft(self):
        handler = object.__new__(sidecar.Handler)
        handler._read_model_json = mock.Mock(return_value={})
        handler._get_job = mock.Mock(return_value=self.source)
        handler.send_json = mock.Mock()
        with mock.patch.dict(sidecar._JOBS, {}, clear=True):
            handler._reuse_design(self.source.id)
            status, response = handler.send_json.call_args.args
            self.assertEqual(status, 200)
            self.assertEqual(response["job"]["state"], "awaiting_confirmation")
            self.assertIn(response["job"]["id"], sidecar._JOBS)
        handler._read_model_json.return_value = {"input_path": str(self.source.input_path)}
        with self.assertRaises(sidecar.RequestError):
            handler._reuse_design(self.source.id)
        handler._read_model_json.return_value = {}
        handler._get_job.return_value = None
        with self.assertRaises(sidecar.RequestError):
            handler._reuse_design(self.source.id)

    def test_route_is_validated_as_a_job_action(self):
        handler = object.__new__(sidecar.Handler)
        self.assertEqual(handler._job_route(f"/v1/orcaslicer/model-jobs/{self.source.id}/reuse-design"),
                         (self.source.id, "reuse-design"))


if __name__ == "__main__":
    unittest.main()
