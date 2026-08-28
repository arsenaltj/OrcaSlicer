#!/usr/bin/env python3
import json
import sys
from pathlib import Path
import tempfile
import unittest
from unittest import mock

from PIL import Image


TOOLS_AI = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS_AI))
import orca_ai_sidecar as sidecar  # noqa: E402


PALETTE = ("#D93632", "#3B8C54", "#315CA8", "#F2F1EA")
JOB_ID = "00000000-0000-0000-0000-000000000001"


def hex_rgb(color):
    return tuple(int(color[index:index + 2], 16) for index in (1, 3, 5))


def synthetic_preview(path, *_args, **_kwargs):
    image = Image.new("RGB", (512, 512), (244, 242, 235))
    for x in range(86, 426):
        for y in range(64, 448):
            image.putpixel((x, y), (210, 55, 50) if x < 256 else (45, 105, 175))
    for x in range(240, 272):
        for y in range(235, 267):
            image.putpixel((x, y), (40, 150, 80))
    image.save(path)
    return Path(path)


class PrintableSidecarIntegrationTests(unittest.TestCase):
    def make_job(self, directory, source="text"):
        job_directory = Path(directory) / JOB_ID
        job_directory.mkdir(exist_ok=True)
        return sidecar.Job(
            id=JOB_ID,
            source=source,
            directory=job_directory,
            palette=PALETTE,
            style="cartoon",
            print_settings=sidecar._normalize_print_settings({
                "width_mm": 96,
                "nozzle_mm": 0.4,
                "line_width_mm": 0.4,
                "minimum_feature_mm": 2.0,
                "shadow_color": "blue",
            }),
        )

    def test_text_job_generates_image_then_all_printable_outputs(self):
        with tempfile.TemporaryDirectory() as directory:
            job = self.make_job(directory)
            job.user_prompt = "一只机械麒麟"
            with mock.patch.object(sidecar, "preprocess_text", return_value="one printable mechanical qilin"), \
                 mock.patch.object(
                     sidecar,
                     "generate_image",
                     side_effect=lambda _instruction, output, *_args: synthetic_preview(output),
                 ) as generate:
                sidecar._preprocess_text_job(job, job.user_prompt)

            self.assertEqual(job.state, "awaiting_confirmation")
            self.assertTrue(job.raw_preview_path.is_file())
            self.assertTrue(job.strict_preview_path.is_file())
            self.assertTrue(job.preview_path.is_file())
            self.assertTrue(job.heatmap_path.is_file())
            self.assertTrue(job.metadata_path.is_file())
            self.assertEqual(set(job.mask_paths), {"primary", "structure", "light", "accent"})
            self.assertGreater(job.image_metrics["minimum_feature_px"], 1)
            self.assertTrue(job.image_metrics["model_input_quality"]["model_input_eligible"])
            self.assertEqual(generate.call_args.args[-3], "blue")
            self.assertEqual(generate.call_args.args[-2], job.palette_roles)
            self.assertEqual(generate.call_args.args[-1], "")

    def test_image_job_preserves_raw_preview_and_uses_clean_preview_for_3d(self):
        with tempfile.TemporaryDirectory() as directory:
            job = self.make_job(directory, "image")
            input_path = Path(directory) / "input.png"
            Image.new("RGB", (96, 96), "white").save(input_path)
            job.input_path = input_path
            with mock.patch.object(sidecar, "preprocess_image", side_effect=lambda _source, _instruction, output, *_args: synthetic_preview(output)):
                sidecar._preprocess_image_job(job, input_path, "保留主体")

            self.assertNotEqual(job.raw_preview_path, job.preview_path)
            self.assertEqual(job.preview_path.name, "clean_preview.png")
            with Image.open(job.preview_path) as preview:
                self.assertEqual(preview.mode, "RGBA")
                self.assertLessEqual(
                    set(preview.convert("RGB").getdata()),
                    {hex_rgb(color) for color in PALETTE},
                )

    def test_printable_outputs_round_trip_through_job_state(self):
        with tempfile.TemporaryDirectory() as directory:
            job = self.make_job(directory)
            raw = job.directory / "style-preview-raw.png"
            synthetic_preview(raw)
            sidecar._apply_printable_image_pipeline(job, raw)
            sidecar._assess_job_model_reference(job)
            job.state = "awaiting_confirmation"
            sidecar._persist_job(job)

            restored = sidecar._load_job(job.directory)

            self.assertIsNotNone(restored)
            self.assertEqual(restored.print_settings, job.print_settings)
            self.assertEqual(restored.image_metrics, job.image_metrics)
            self.assertEqual(set(restored.mask_paths), set(job.mask_paths))
            public = sidecar._public_job(restored)
            self.assertTrue(public["image_outputs"]["strict_preview"]["ready"])
            self.assertTrue(public["image_outputs"]["metadata"]["ready"])
            self.assertEqual(public["image_metrics"]["minimum_feature_px"], job.image_metrics["minimum_feature_px"])
            self.assertEqual(
                public["image_metrics"]["model_input_quality"],
                job.image_metrics["model_input_quality"],
            )

    def test_all_documented_output_routes_are_loopback_download_routes(self):
        job_id = "00000000-0000-0000-0000-000000000001"
        for action in (
            "raw-preview", "strict-preview", "preview", "heatmap", "metadata",
            "background-mask", "subject-mask", "mask-red", "mask-white",
        ):
            self.assertEqual(
                sidecar.Handler._job_route(f"/v1/orcaslicer/model-jobs/{job_id}/{action}"),
                (job_id, action),
            )

    def test_preprocess_failure_round_trip_preserves_actionable_category(self):
        with tempfile.TemporaryDirectory() as directory:
            job = self.make_job(directory)
            error = sidecar.OpenAIPreprocessorError(
                "service unavailable",
                code="image_service_unavailable",
                retryable=True,
                ambiguous=True,
            )
            sidecar._fail_preprocess_job(job, error)
            restored = sidecar._load_job(job.directory)
            public = sidecar._public_job(restored)

        self.assertEqual(public["state"], "failed")
        self.assertEqual(public["provider_failure"]["code"], "image_service_unavailable")
        self.assertTrue(public["provider_failure"]["retryable"])
        self.assertTrue(public["provider_failure"]["ambiguous"])


if __name__ == "__main__":
    unittest.main()
