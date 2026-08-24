from __future__ import annotations

import json
from pathlib import Path
from types import SimpleNamespace
import tempfile
import unittest
from unittest.mock import patch

from tools.ai import model_blind_benchmark as benchmark
from tools.ai import run_paid_tripo_validation as paid_tripo


class ModelBlindBenchmarkTests(unittest.TestCase):
    def _manifest(self, root: Path, *, cases: list[dict] | None = None) -> Path:
        value = {
            "schema_version": 1,
            "benchmark_id": "blind-v1",
            "frozen_at": "2026-08-16",
            "face_limit": 300000,
            "cases": cases or [{
                "id": "toy",
                "category": "character",
                "style": "q_cartoon",
                "prompt": "one printable toy",
                "palette": ["#FF0000", "#00FF00", "#0000FF", "#FFFFFF"],
            }],
        }
        path = root / "manifest.json"
        path.write_text(json.dumps(value), encoding="utf-8")
        return path

    def test_manifest_is_stable_and_requires_unique_cases(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = self._manifest(root)
            first = benchmark.load_manifest(path)
            second = benchmark.load_manifest(path)
            self.assertEqual(first.fingerprint, second.fingerprint)
            self.assertEqual(first.cases[0].palette, ("#FF0000", "#00FF00", "#0000FF", "#FFFFFF"))
            duplicate = [{
                "id": "same", "category": "x", "style": "q_cartoon", "prompt": "x",
                "palette": ["#100000", "#200000", "#300000", "#400000"],
            }] * 2
            with self.assertRaisesRegex(benchmark.BlindBenchmarkError, "unique"):
                benchmark.load_manifest(self._manifest(root, cases=duplicate))

    def test_reference_requires_confirmation_and_resumes_without_second_call(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest = benchmark.load_manifest(self._manifest(root))
            case = manifest.cases[0]
            case_root = root / "output" / case.case_id
            calls = []

            def generator(prompt, output, palette, style):
                calls.append((prompt, palette, style))
                Path(output).write_bytes(b"image")
                return Path(output)

            def processor(raw, output, palette, settings):
                output = Path(output)
                model = output / "model_reference.png"
                metadata = output / "printable-image.json"
                model.write_bytes(b"model")
                metadata.write_text("{}", encoding="utf-8")
                return SimpleNamespace(model_reference=model, metadata=metadata, metrics={"palette_quality_ok": True})

            with self.assertRaisesRegex(benchmark.BlindBenchmarkError, "confirm-image"):
                benchmark.prepare_reference(
                    case_root, manifest, case, confirm_paid_call=False,
                    image_generator=generator, processor=processor,
                )
            benchmark.prepare_reference(
                case_root, manifest, case, confirm_paid_call=True,
                image_generator=generator, processor=processor,
            )
            benchmark.prepare_reference(
                case_root, manifest, case, confirm_paid_call=True,
                image_generator=generator, processor=processor,
            )
            state = json.loads((case_root / "case-state.json").read_text(encoding="utf-8"))
            self.assertEqual(len(calls), 1)
            self.assertEqual(state["paid_calls"]["image2"], 1)
            self.assertTrue(state["stages"]["reference"]["resumed"])

    def test_failed_image_attempt_is_not_repeated_automatically(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest = benchmark.load_manifest(self._manifest(root))
            case = manifest.cases[0]
            case_root = root / "output" / case.case_id

            def failing(*args, **kwargs):
                raise RuntimeError("provider stopped")

            with self.assertRaisesRegex(RuntimeError, "provider stopped"):
                benchmark.prepare_reference(
                    case_root, manifest, case, confirm_paid_call=True, image_generator=failing,
                )
            with self.assertRaisesRegex(benchmark.BlindBenchmarkError, "refusing an automatic repeat"):
                benchmark.prepare_reference(case_root, manifest, case, confirm_paid_call=True)

    def test_reference_quality_review_blocks_tripo(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest = benchmark.load_manifest(self._manifest(root))
            case = manifest.cases[0]
            case_root = root / "output" / case.case_id

            def generator(prompt, output, palette, style):
                Path(output).write_bytes(b"image")
                return Path(output)

            def processor(raw, output, palette, settings):
                output = Path(output)
                model = output / "model_reference.png"
                metadata = output / "metadata.json"
                model.write_bytes(b"model")
                metadata.write_text("{}", encoding="utf-8")
                return SimpleNamespace(model_reference=model, metadata=metadata, metrics={"palette_quality_ok": False})

            state = benchmark.prepare_reference(
                case_root, manifest, case, confirm_paid_call=True,
                image_generator=generator, processor=processor,
            )
            self.assertEqual(state["stages"]["reference"]["status"], "review")
            with self.assertRaisesRegex(benchmark.BlindBenchmarkError, "quality gate"):
                benchmark.run_tripo(case_root, manifest, case, confirm_paid_call=True)

    def test_tripo_confirmation_and_paid_task_accounting(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest = benchmark.load_manifest(self._manifest(root))
            case = manifest.cases[0]
            case_root = root / "output" / case.case_id
            state = benchmark.load_case_state(case_root, manifest, case)
            reference = case_root / "reference.png"
            reference.parent.mkdir(parents=True, exist_ok=True)
            reference.write_bytes(b"reference")
            state["artifacts"]["model_reference"] = str(reference)
            state["stages"]["reference"] = {"status": "success"}
            benchmark._save_state(case_root, state)
            with self.assertRaisesRegex(benchmark.BlindBenchmarkError, "confirm-tripo"):
                benchmark.run_tripo(case_root, manifest, case, confirm_paid_call=False)

            def runner(input_path, output_root, confirmed, face_limit, palette):
                self.assertTrue(confirmed)
                task = Path(output_root) / "task-1"
                task.mkdir(parents=True)
                artifact = task / "model-vertex-color.obj"
                artifact.write_text("v 0 0 0 1 0 0\n", encoding="utf-8")
                (Path(output_root) / "validation-state.json").write_text(json.dumps({
                    "generation_task_id": "task-1", "conversion_task_id": "convert-1",
                }), encoding="utf-8")
                return artifact

            result = benchmark.run_tripo(
                case_root, manifest, case, confirm_paid_call=True, runner=runner,
            )
            self.assertEqual(result["paid_calls"]["tripo_generation"], 1)
            self.assertEqual(result["paid_calls"]["tripo_conversion"], 1)

    def test_collect_preserves_human_and_automatic_results(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest = benchmark.load_manifest(self._manifest(root))
            case = manifest.cases[0]
            case_root = root / case.case_id
            state = benchmark.load_case_state(case_root, manifest, case)
            review = case_root / "review"
            review.mkdir(parents=True)
            structural = review / "model-quality.json"
            visual = review / "visual-quality.json"
            structural.write_text('{"status":"pass"}', encoding="utf-8")
            visual.write_text('{"status":"pass"}', encoding="utf-8")
            state["artifacts"].update({"model_quality": str(structural), "visual_quality": str(visual)})
            state["paid_calls"].update({"image2": 1, "tripo_generation": 1, "tripo_conversion": 1, "visual_review": 1})
            benchmark._save_state(case_root, state)
            human = benchmark.ensure_human_review(case_root)
            human["status"] = "pass"
            benchmark._write_json(case_root / "human-review.json", human)
            summary = benchmark.collect_results(root, manifest)
            self.assertEqual(summary["paid_calls"]["tripo_generation"], 1)
            self.assertEqual(summary["exact_agreement_rate"], 1.0)
            self.assertEqual(summary["cases"][0]["automatic_status"], "pass")

    def test_unavailable_visual_report_is_not_retried(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest = benchmark.load_manifest(self._manifest(root))
            case = manifest.cases[0]
            case_root = root / case.case_id
            state = benchmark.load_case_state(case_root, manifest, case)
            obj = case_root / "model.obj"
            obj.parent.mkdir(parents=True, exist_ok=True)
            obj.write_text(
                "v 0 0 0\nv 1 0 0\nv 0 1 0\nv 0 0 1\n"
                "f 1 3 2\nf 1 2 4\nf 2 3 4\nf 3 1 4\n",
                encoding="utf-8",
            )
            review = case_root / "review"
            review.mkdir()
            visual_path = review / "visual-quality.json"
            visual_path.write_text('{"status":"unavailable"}', encoding="utf-8")
            state["artifacts"].update({"model_obj": str(obj), "visual_quality": str(visual_path)})
            state["paid_calls"]["visual_review"] = 1
            benchmark._save_state(case_root, state)
            calls = []

            def reviewer(*args, **kwargs):
                calls.append(True)
                return {"status": "pass"}

            result = benchmark.review_case(
                case_root, manifest, case, confirm_visual_call=True, visual_reviewer=reviewer,
            )
            self.assertEqual(calls, [])
            self.assertEqual(result["stages"]["visual"]["status"], "unavailable")

    def test_paid_tripo_task_id_is_saved_before_input_copy(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            state_path = root / "validation-state.json"
            input_path = root / "input.png"
            input_path.write_bytes(b"image")
            info = {"sha256": "ABC"}
            with patch.object(paid_tripo.tripo_client, "upload_image", return_value="token"), patch.object(
                paid_tripo.tripo_client, "create_image_task", return_value="paid-task"
            ), patch.object(paid_tripo.shutil, "copy2", side_effect=OSError("disk full")):
                with self.assertRaisesRegex(OSError, "disk full"):
                    paid_tripo._create_or_resume_generation(
                        input_path, info, root, state_path, True, 300000,
                        ("#FF0000", "#00FF00", "#0000FF", "#FFFFFF"),
                    )
            saved = json.loads(state_path.read_text(encoding="utf-8"))
            self.assertEqual(saved["generation_task_id"], "paid-task")


if __name__ == "__main__":
    unittest.main()
