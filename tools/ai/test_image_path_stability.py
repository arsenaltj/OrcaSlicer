#!/usr/bin/env python3
import contextlib
from io import BytesIO
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from PIL import Image, ImageDraw

TOOLS_AI = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS_AI))

import orca_ai_sidecar as sidecar
import openai_preprocessor


def image_bytes(format_name: str = "PNG", size: int = 96, *, blank: bool = False) -> bytes:
    output = BytesIO()
    image = Image.new("RGB", (size, size), "white")
    if not blank:
        draw = ImageDraw.Draw(image)
        draw.rectangle((size // 5, size // 5, size * 4 // 5, size * 4 // 5), fill="#C83B32")
        draw.ellipse((size // 3, size // 3, size * 2 // 3, size * 2 // 3), fill="#243A68")
    image.save(output, format=format_name)
    return output.getvalue()


def write_preview(path: Path, size: int = 512) -> None:
    path.write_bytes(image_bytes(size=size))


@contextlib.contextmanager
def output_root(path: str):
    previous = os.environ.get("ORCASLICER_AI_OUTPUT_DIR")
    os.environ["ORCASLICER_AI_OUTPUT_DIR"] = path
    try:
        yield
    finally:
        if previous is None:
            os.environ.pop("ORCASLICER_AI_OUTPUT_DIR", None)
        else:
            os.environ["ORCASLICER_AI_OUTPUT_DIR"] = previous


class ImagePathStabilityTests(unittest.TestCase):
    def test_source_gate_fully_decodes_png_and_jpeg(self):
        for format_name in ("PNG", "JPEG"):
            with self.subTest(format=format_name):
                info = sidecar._validate_image_data(
                    image_bytes(format_name), minimum_edge=sidecar.MIN_SOURCE_IMAGE_EDGE
                )
                self.assertEqual(info.width, 96)
                self.assertEqual(info.height, 96)

    def test_source_gate_rejects_truncated_and_tiny_images(self):
        with self.assertRaisesRegex(ValueError, "damaged|decoded"):
            sidecar._validate_image_data(
                b"\x89PNG\r\n\x1a\ntruncated", minimum_edge=sidecar.MIN_SOURCE_IMAGE_EDGE
            )
        with self.assertRaisesRegex(ValueError, "at least 64 x 64"):
            sidecar._validate_image_data(
                image_bytes(size=32), minimum_edge=sidecar.MIN_SOURCE_IMAGE_EDGE
            )

    def test_3d_reference_gate_rejects_small_blank_and_transparent_images(self):
        with self.assertRaisesRegex(ValueError, "at least 256 x 256"):
            sidecar._validate_image_data(
                image_bytes(size=128),
                minimum_edge=sidecar.MIN_MODEL_REFERENCE_EDGE,
                require_visual_detail=True,
            )
        with self.assertRaisesRegex(ValueError, "blank"):
            sidecar._validate_image_data(
                image_bytes(size=256, blank=True),
                minimum_edge=sidecar.MIN_MODEL_REFERENCE_EDGE,
                require_visual_detail=True,
            )
        output = BytesIO()
        Image.new("RGBA", (256, 256), (0, 0, 0, 0)).save(output, format="PNG")
        with self.assertRaisesRegex(ValueError, "transparent"):
            sidecar._validate_image_data(
                output.getvalue(),
                minimum_edge=sidecar.MIN_MODEL_REFERENCE_EDGE,
                require_visual_detail=True,
            )

    def test_all_four_image_styles_reach_a_valid_3d_reference_three_times(self):
        styles = (
            ("sculpture", ""),
            ("realistic", ""),
            ("cartoon", ""),
            ("custom", "复古木刻玩具"),
        )
        with tempfile.TemporaryDirectory() as directory, output_root(directory):
            for pass_index in range(3):
                for style, custom_style in styles:
                    with self.subTest(pass_index=pass_index, style=style):
                        job = sidecar._new_job("image", (), {}, style, custom_style)
                        source = job.directory / "input.png"
                        source.write_bytes(image_bytes())
                        job.input_path = source

                        def preprocess(_source, _instruction, destination, *_args, **_kwargs):
                            write_preview(Path(destination))

                        with mock.patch.object(sidecar, "preprocess_image", side_effect=preprocess):
                            sidecar._preprocess_image_job(job, source, "只改变风格")

                        self.assertEqual(job.state, "awaiting_confirmation")
                        self.assertIsNotNone(job.preview_path)
                        sidecar._validate_image_file(
                            job.preview_path,
                            minimum_edge=sidecar.MIN_MODEL_REFERENCE_EDGE,
                            require_visual_detail=True,
                        )

    def test_multicolor_text_and_image_paths_reach_model_reference_three_times(self):
        palette = ("#FFFFFF", "#C83B32", "#243A68")
        with tempfile.TemporaryDirectory() as directory, output_root(directory):
            for pass_index in range(3):
                with self.subTest(pass_index=pass_index, source="text-sculpture"):
                    sculpture_job = sidecar._new_job("text", (), style="sculpture")

                    def generate_sculpture(_instruction, destination, *_args, **_kwargs):
                        write_preview(Path(destination))

                    with mock.patch.object(sidecar, "preprocess_text", return_value="single-color sculpture"), \
                         mock.patch.object(sidecar, "generate_image", side_effect=generate_sculpture):
                        sidecar._preprocess_text_job(sculpture_job, "一个人物雕塑")
                    self.assertEqual(sculpture_job.state, "awaiting_confirmation")
                    self.assertIsNotNone(sculpture_job.preview_path)
                    sidecar._validate_image_file(
                        sculpture_job.preview_path,
                        minimum_edge=sidecar.MIN_MODEL_REFERENCE_EDGE,
                        require_visual_detail=True,
                    )

                with self.subTest(pass_index=pass_index, source="text"):
                    text_job = sidecar._new_job("text", palette, style="realistic")

                    def generate(_instruction, destination, *_args, **_kwargs):
                        write_preview(Path(destination))

                    with mock.patch.object(sidecar, "preprocess_text", return_value="printable subject"), \
                         mock.patch.object(sidecar, "generate_image", side_effect=generate):
                        sidecar._preprocess_text_job(text_job, "一个写实摆件")
                    self.assertEqual(text_job.state, "awaiting_confirmation")
                    sidecar._validate_image_file(
                        text_job.model_reference_path,
                        minimum_edge=sidecar.MIN_MODEL_REFERENCE_EDGE,
                        require_visual_detail=True,
                    )

                with self.subTest(pass_index=pass_index, source="image"):
                    image_job = sidecar._new_job("image", palette, style="cartoon")
                    source = image_job.directory / "input.png"
                    source.write_bytes(image_bytes())
                    image_job.input_path = source

                    def preprocess(_source, _instruction, destination, *_args, **_kwargs):
                        write_preview(Path(destination))

                    with mock.patch.object(sidecar, "preprocess_image", side_effect=preprocess):
                        sidecar._preprocess_image_job(image_job, source, "可爱卡通")
                    self.assertEqual(image_job.state, "awaiting_confirmation")
                    sidecar._validate_image_file(
                        image_job.model_reference_path,
                        minimum_edge=sidecar.MIN_MODEL_REFERENCE_EDGE,
                        require_visual_detail=True,
                    )

    def test_invalid_reference_fails_before_any_paid_model_call(self):
        with tempfile.TemporaryDirectory() as directory, output_root(directory):
            job = sidecar._new_job("image", style="realistic")
            preview = job.directory / "preview.png"
            preview.write_bytes(b"\x89PNG\r\n\x1a\ntruncated")
            job.preview_path = preview
            gateway = mock.Mock()
            with mock.patch.object(sidecar, "_MODEL_PROVIDER_GATEWAY", gateway):
                sidecar._generate_job(
                    job,
                    "",
                    authorization=sidecar.PaidTaskAuthorization.confirmed(f"{job.id}:model:1"),
                )
            self.assertEqual(job.state, "failed")
            self.assertIn("not suitable for 3D input", job.message)
            gateway.start_or_reuse_model_task.assert_not_called()

    def test_restore_demotes_corrupt_preview_instead_of_exposing_false_ready_state(self):
        with tempfile.TemporaryDirectory() as directory, output_root(directory):
            job = sidecar._new_job("image", style="cartoon")
            source = job.directory / "input.png"
            source.write_bytes(image_bytes())
            preview = job.directory / "preview.png"
            preview.write_bytes(b"\x89PNG\r\n\x1a\ntruncated")
            job.input_path = source
            job.preview_path = preview
            job.preview_content_type = "image/png"
            job.state = "awaiting_confirmation"
            job.phase = "awaiting_confirmation"
            sidecar._persist_job(job)
            with sidecar._JOBS_LOCK:
                previous = dict(sidecar._JOBS)
                sidecar._JOBS.clear()
            try:
                sidecar._restore_jobs()
                restored = sidecar._JOBS[job.id]
                self.assertEqual(restored.state, "failed")
                self.assertIsNone(restored.preview_path)
                self.assertIn("missing or damaged", restored.message)
            finally:
                with sidecar._JOBS_LOCK:
                    sidecar._JOBS.clear()
                    sidecar._JOBS.update(previous)

    def test_every_public_style_prompt_preserves_subject_inventory(self):
        for style in sidecar.STYLE_IDS:
            custom = "木刻大色块" if style == "custom" else ""
            prompt = openai_preprocessor.build_style_preview_prompt(
                "只改变风格",
                () if style == "sculpture" else ("#FFFFFF", "#243A68"),
                style,
                custom_style=custom,
            )
            with self.subTest(style=style):
                self.assertIn("source image is the authority", prompt)
                self.assertIn("recognizable identity", prompt)
                self.assertIn("Do not turn the chosen", prompt)
                self.assertIn("Do not invent unseen anatomy", prompt)


if __name__ == "__main__":
    unittest.main()
