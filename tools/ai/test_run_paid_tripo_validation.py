import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


TOOLS_AI = Path(__file__).resolve().parent
if str(TOOLS_AI) not in sys.path:
    sys.path.insert(0, str(TOOLS_AI))

import run_paid_tripo_validation as validation  # noqa: E402


class PaidTripoValidationProfileTests(unittest.TestCase):
    def make_input(self, root: Path) -> tuple[Path, dict[str, str]]:
        source = root / "input.png"
        source.write_bytes(b"test image bytes")
        return source, {"sha256": "ABC123"}

    def test_generation_profile_is_forwarded_and_frozen_before_copy(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, info = self.make_input(root)
            state_path = root / "validation-state.json"
            with (
                mock.patch.object(validation.tripo_client, "upload_image", return_value="token"),
                mock.patch.object(validation.tripo_client, "create_image_task", return_value="task-1") as create,
                mock.patch.object(validation.shutil, "copy2", side_effect=OSError("disk full")),
            ):
                with self.assertRaisesRegex(OSError, "disk full"):
                    validation._create_or_resume_generation(
                        source,
                        info,
                        root,
                        state_path,
                        True,
                        300000,
                        (),
                        "performance",
                    )

            create.assert_called_once_with("token", 300000, "performance")
            state = json.loads(state_path.read_text(encoding="utf-8"))
            self.assertEqual(state["generation_profile"], "performance")
            self.assertEqual(state["generation_task_id"], "task-1")
            self.assertEqual(state["generation_status"], "submitted")

    def test_ambiguous_creation_is_persisted_and_never_replayed(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, info = self.make_input(root)
            state_path = root / "validation-state.json"
            with (
                mock.patch.object(validation.tripo_client, "upload_image", return_value="token"),
                mock.patch.object(
                    validation.tripo_client,
                    "create_image_task",
                    side_effect=validation.tripo_client.TripoError("Could not connect to Tripo."),
                ) as create,
            ):
                with self.assertRaises(validation.tripo_client.TripoError):
                    validation._create_or_resume_generation(
                        source, info, root, state_path, True, 1000000, (), "quality"
                    )
                with self.assertRaisesRegex(RuntimeError, "no task ID"):
                    validation._create_or_resume_generation(
                        source, info, root, state_path, True, 1000000, (), "quality"
                    )

            create.assert_called_once_with("token", 1000000, "quality")
            state = json.loads(state_path.read_text(encoding="utf-8"))
            self.assertEqual(state["generation_status"], "creation_failed_or_ambiguous")
            self.assertIsNone(state["generation_task_id"])

    def test_legacy_state_without_profile_resumes_as_quality(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            state_path = root / "validation-state.json"
            state_path.write_text(
                json.dumps({
                    "input_sha256": "ABC123",
                    "face_limit": 1000000,
                    "palette": [],
                    "generation_task_id": "legacy-task",
                }),
                encoding="utf-8",
            )

            state = validation._load_state(
                state_path, {"sha256": "ABC123"}, 1000000, (), "quality"
            )
            self.assertEqual(state["generation_task_id"], "legacy-task")
            with self.assertRaisesRegex(RuntimeError, "different generation settings"):
                validation._load_state(
                    state_path, {"sha256": "ABC123"}, 1000000, (), "performance"
                )


if __name__ == "__main__":
    unittest.main()
