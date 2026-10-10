"""Offline timing/statistics coverage; every provider boundary is mocked."""
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

from design_generation_timing import DesignTimingHistory, timing_key
import orca_ai_sidecar as sidecar


class DesignTimingTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.history = DesignTimingHistory(self.root)
        self.provider = {"available": True, "source": "pro", "base_url": "https://test.invalid/v1"}
        self.key = timing_key(self.provider, "image-model", "high", "image", "sculpture")
        for patch in (
            mock.patch.object(sidecar, "_model_output_root", return_value=self.root),
            mock.patch.object(sidecar, "image_provider_status", return_value=self.provider),
            mock.patch("urllib.request.OpenerDirector.open", side_effect=AssertionError("offline test")),
        ):
            patch.start()
            self.addCleanup(patch.stop)

    def test_estimate_needs_three_samples_and_survives_restart(self):
        for duration in (40, 60):
            self.history.record(self.key, duration, now=100)
            self.assertEqual(self.history.estimate(self.key, now=100), 0)
        self.history.record(self.key, 4000, now=100)
        self.assertEqual(DesignTimingHistory(self.root).estimate(self.key, now=100), 60)

    def test_groups_separate_image_routes_models_quality_modes_and_styles(self):
        for duration in (40, 60, 80):
            self.history.record(self.key, duration)
        variants = [
            ({**self.provider, "base_url": "https://other.invalid/v1"}, "image-model", "high", "image", "sculpture"),
            (self.provider, "other-model", "high", "image", "sculpture"),
            (self.provider, "image-model", "low", "image", "sculpture"),
            (self.provider, "image-model", "high", "text", "sculpture"),
            (self.provider, "image-model", "high", "image", "cartoon"),
        ]
        for values in variants:
            self.assertEqual(self.history.estimate(timing_key(*values)), 0)
        stored = self.history.path.read_text()
        self.assertNotIn(self.provider["base_url"], stored)
        self.assertNotIn("image-model", stored)
        self.assertEqual(timing_key({}, "m", "high", "text", "s"), "")

    def test_invalid_expired_and_future_samples_are_ignored(self):
        for value in (-1, 0, True, "60", float("nan"), float("inf"), 7201, 10 ** 400):
            self.assertFalse(self.history.record(self.key, value))
        for _ in range(3):
            self.history.record(self.key, 60, now=100)
        self.assertEqual(self.history.estimate(self.key, now=100 + 31 * 86400), 0)
        self.assertEqual(self.history.estimate(self.key, now=99), 0)

    def test_history_keeps_only_recent_samples(self):
        for i in range(25):
            self.history.record(self.key, i + 1, now=100 + i)
        self.assertEqual(len(json.loads(self.history.path.read_text())["groups"][self.key]), 20)
        self.assertEqual(self.history.estimate(self.key, now=125), 16)

    def test_malformed_and_oversized_history_does_not_block_generation(self):
        invalid_numbers = json.dumps({"version": 1, "groups": {self.key: [[10 ** 400, 60], [1, 10 ** 400]]}})
        for content in ('{broken', '[]', '{"version":1,"groups":[]}', invalid_numbers, ' ' * (256 * 1024 + 1)):
            self.history.path.write_text(content)
            self.assertEqual(self.history.estimate(self.key), 0)
            self.assertTrue(self.history.record(self.key, 60))
        with mock.patch("design_generation_timing.os.replace", side_effect=OSError("read only")):
            self.assertFalse(self.history.record(self.key, 60))

    def test_queue_time_is_included_and_duplicate_completion_is_not_counted(self):
        job = sidecar._new_job("image")
        with mock.patch.object(sidecar.time, "monotonic", return_value=100) as clock, \
             mock.patch.object(sidecar, "_executor_for") as executor:
            sidecar._submit(job, sidecar._preprocess_image_job, Path("input.png"), "prompt")
            self.assertEqual(job.design_started_monotonic, 100)
            executor.return_value.submit.assert_called_once()
            clock.return_value = 160
            sidecar._begin_design_timing(job)
            sidecar._complete_design_timing(job)
            sidecar._complete_design_timing(job)
            public = sidecar._public_job(job)["design_timing"]
            self.assertEqual(public["elapsed_seconds"], 60)
            self.assertNotIn("group", public)
        key = job.image_metrics["design_timing"]["group"]
        rows = json.loads(self.history.path.read_text())["groups"][key]
        self.assertEqual(len(rows), 1)
        sidecar._persist_job(job)
        restored = sidecar._load_job(job.directory)
        self.assertEqual(restored.image_metrics["design_timing"]["duration_seconds"], 60)
        self.assertEqual(sidecar._public_design_timing(restored), {})

    def test_cancelled_or_legacy_jobs_do_not_create_samples(self):
        job = sidecar._new_job("image")
        self.assertEqual(sidecar._public_design_timing(job), {})
        with mock.patch.object(sidecar.time, "monotonic", return_value=100) as clock:
            sidecar._begin_design_timing(job)
            clock.return_value = 160
            job.stop_event.set()
            sidecar._complete_design_timing(job)
        self.assertFalse(self.history.path.exists())

    def test_successful_text_and_image_workers_record_samples(self):
        for source in ("text", "image"):
            with self.subTest(source=source), \
                 mock.patch.object(sidecar.time, "monotonic", return_value=100) as clock, \
                 mock.patch.object(sidecar, "_validate_image_file", return_value=SimpleNamespace(content_type="image/png")), \
                 mock.patch.object(sidecar, "_assess_reference_advice"), \
                 mock.patch.object(sidecar, "_assess_job_preview_visual_quality"), \
                 mock.patch.object(sidecar, "review_nonportrait_reference", return_value={"status": "advisory"}):
                job = sidecar._new_job(source)
                def generate(*args, **kwargs):
                    Path(args[1] if source == "text" else args[2]).write_bytes(b"mock image")
                    clock.return_value = 165
                with mock.patch.object(sidecar, "generate_geometry_reference_image", side_effect=generate), \
                     mock.patch.object(sidecar, "preprocess_image", side_effect=generate):
                    if source == "text":
                        sidecar._preprocess_text_job(job, "a cat")
                    else:
                        sidecar._preprocess_image_job(job, Path("input.png"), "a cat")
                self.assertEqual(job.state, "awaiting_confirmation")
                self.assertEqual(job.image_metrics["design_timing"]["duration_seconds"], 65)
                self.assertEqual(sidecar._load_job(job.directory).image_metrics["design_timing"]["duration_seconds"], 65)

    def test_provider_failure_does_not_create_a_success_sample(self):
        job = sidecar._new_job("image")
        with mock.patch.object(sidecar, "preprocess_image", side_effect=sidecar.OpenAIPreprocessorError("offline failure")):
            sidecar._preprocess_image_job(job, Path("input.png"), "a cat")
        self.assertEqual(job.state, "failed")
        self.assertNotIn("duration_seconds", job.image_metrics["design_timing"])
        self.assertFalse(self.history.path.exists())

    def test_new_attempt_uses_a_fresh_clock_and_fixed_estimate(self):
        job = sidecar._new_job("image")
        with mock.patch.object(sidecar.time, "monotonic", return_value=100) as clock, \
             mock.patch.object(sidecar, "_executor_for"):
            sidecar._submit(job, sidecar._preprocess_image_job)
            key = job.image_metrics["design_timing"]["group"]
            for duration in (60, 80, 100):
                self.history.record(key, duration)
            clock.return_value = 200
            sidecar._submit(job, sidecar._preprocess_image_job)
            self.assertEqual(sidecar._public_design_timing(job)["elapsed_seconds"], 0)
            self.assertEqual(sidecar._public_design_timing(job)["estimated_seconds"], 80)
            for _ in range(20):
                self.history.record(key, 150)
            clock.return_value = 220
            self.assertEqual(sidecar._public_design_timing(job)["estimated_seconds"], 80)


if __name__ == "__main__":
    unittest.main()
