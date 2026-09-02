from __future__ import annotations

import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock

from PIL import Image

from tools.ai import run_paid_tripo_texture_validation as runner


class PaidTripoTextureValidationTests(unittest.TestCase):
    def _input(self, root: Path) -> tuple[Path, dict]:
        path = root / "reference.png"
        Image.new("RGB", (64, 64), "red").save(path)
        return path, runner.paid_tripo._check_input(path, ())

    def test_task_id_is_persisted_before_reference_copy(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            input_path, input_info = self._input(root)
            output = root / "output"
            output.mkdir()
            state_path = output / "validation-state.json"
            with mock.patch.object(runner.shutil, "copy2", side_effect=OSError("locked")):
                with self.assertRaises(OSError):
                    runner._create_or_resume_texture(
                        input_path,
                        input_info,
                        output,
                        state_path,
                        source_task_id="source-task",
                        confirm_paid_call=True,
                        face_limit=1000000,
                        palette=("#FF0000",),
                        texture_alignment="geometry",
                        texture_quality="detailed",
                        texture_seed=3,
                        uploader=lambda path: "image-token",
                        creator=lambda *args, **kwargs: "texture-task",
                    )
            state = json.loads(state_path.read_text(encoding="utf-8"))
            self.assertEqual(state["generation_task_id"], "texture-task")
            self.assertEqual(state["generation_status"], "submitted")

    def test_ambiguous_creation_is_not_replayed(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            input_path, input_info = self._input(root)
            output = root / "output"
            output.mkdir()
            state_path = output / "validation-state.json"

            def fail(*args, **kwargs):
                raise runner.tripo_client.TripoError("ambiguous")

            with self.assertRaises(runner.tripo_client.TripoError):
                runner._create_or_resume_texture(
                    input_path,
                    input_info,
                    output,
                    state_path,
                    source_task_id="source-task",
                    confirm_paid_call=True,
                    face_limit=1000000,
                    palette=("#FF0000",),
                    texture_alignment="geometry",
                    texture_quality="detailed",
                    texture_seed=None,
                    uploader=lambda path: "image-token",
                    creator=fail,
                )
            with self.assertRaisesRegex(RuntimeError, "ambiguous"):
                runner._create_or_resume_texture(
                    input_path,
                    input_info,
                    output,
                    state_path,
                    source_task_id="source-task",
                    confirm_paid_call=True,
                    face_limit=1000000,
                    palette=("#FF0000",),
                    texture_alignment="geometry",
                    texture_quality="detailed",
                    texture_seed=None,
                    uploader=lambda path: self.fail("must not upload again"),
                    creator=lambda *args, **kwargs: self.fail("must not create again"),
                )

    def test_multiview_creation_uploads_official_view_order_and_persists_task(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            inputs = []
            infos = []
            for index, color in enumerate(("red", "green", "blue", "white")):
                path = root / f"view-{index}.png"
                Image.new("RGB", (64, 64), color).save(path)
                inputs.append(path)
                infos.append(runner.paid_tripo._check_input(path, ()))
            output = root / "output"
            output.mkdir()
            created_with = []

            state, task_directory = runner._create_or_resume_multiview_texture(
                tuple(inputs),
                tuple(infos),
                output,
                output / "validation-state.json",
                source_task_id="source-task",
                confirm_paid_call=True,
                face_limit=1000000,
                palette=("#FF0000",),
                texture_alignment="geometry",
                texture_quality="detailed",
                texture_seed=7,
                uploader=lambda path: f"token-{path.stem}",
                creator=lambda source, tokens, **kwargs: created_with.append((source, tokens, kwargs)) or "texture-task",
            )

            self.assertEqual(state["generation_task_id"], "texture-task")
            self.assertEqual(
                created_with[0][1],
                ["token-view-0", "token-view-1", "token-view-2", "token-view-3"],
            )
            self.assertEqual(created_with[0][2]["texture_seed"], 7)
            for view in runner.TEXTURE_VIEW_ORDER:
                self.assertTrue((task_directory / f"texture-reference-{view}.png").is_file())


if __name__ == "__main__":
    unittest.main()
