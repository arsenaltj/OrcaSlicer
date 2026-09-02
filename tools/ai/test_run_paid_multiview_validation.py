from __future__ import annotations

import json
import hashlib
from pathlib import Path
import tempfile
import unittest

from PIL import Image

from tools.ai import run_paid_multiview_validation as runner


class PaidMultiviewValidationTests(unittest.TestCase):
    def _baseline(self, root: Path) -> Path:
        case = root / "case"
        reference = case / "reference" / "model_reference.png"
        reference.parent.mkdir(parents=True)
        Image.new("RGBA", (512, 512), (255, 0, 0, 255)).save(reference)
        (case / "case-state.json").write_text(json.dumps({
            "case_id": "case",
            "prompt": "one robot",
            "style": "q_cartoon",
            "palette": ["#FF0000", "#00FF00", "#0000FF", "#FFFFFF"],
            "palette_roles": {
                "primary": "#FFFFFF", "structure": "#0000FF",
                "light": "#FF0000", "accent": "#00FF00",
            },
            "artifacts": {"model_reference": str(reference)},
        }), encoding="utf-8")
        return case

    def test_baseline_preserves_validated_palette_roles(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            baseline = runner.load_baseline(self._baseline(Path(directory)))
            self.assertEqual(baseline["palette_roles"]["primary"], "#FFFFFF")
            self.assertEqual(baseline["palette_roles"]["light"], "#FF0000")

    def test_baseline_reference_must_stay_inside_case(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            case = root / "case"
            case.mkdir()
            outside = root / "outside.png"
            Image.new("RGB", (8, 8)).save(outside)
            (case / "case-state.json").write_text(json.dumps({
                "prompt": "x", "palette": ["#100000", "#200000", "#300000", "#400000"],
                "artifacts": {"model_reference": str(outside)},
            }), encoding="utf-8")
            with self.assertRaisesRegex(runner.MultiviewValidationError, "inside"):
                runner.load_baseline(case)

    def test_generation_id_is_saved_before_input_copy(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            baseline = runner.load_baseline(self._baseline(root))
            output = root / "output"
            output.mkdir()
            state = runner.load_or_create_state(output, baseline, 300000)
            views = {}
            for view in ("front", "left", "back", "right"):
                path = output / f"{view}.png"
                Image.new("RGB", (8, 8)).save(path)
                views[view] = str(path)
            state["views"] = views
            generation_views = {}
            for view in ("front", "left", "back", "right"):
                path = output / f"{view}-generation.png"
                Image.new("RGB", (8, 8), "red").save(path)
                generation_views[view] = str(path)
            state["generation_views"] = generation_views
            runner._write_json(output / "multiview-state.json", state)

            def creator(tokens, face_limit):
                self.assertEqual(list(tokens), ["front", "left", "back", "right"])
                return "paid-multiview-task"

            saved, task_directory = runner.create_or_resume_generation(
                output,
                state,
                confirm_paid_call=True,
                uploader=lambda path: "token-" + path.stem,
                creator=creator,
            )
            persisted = json.loads((output / "multiview-state.json").read_text(encoding="utf-8"))
            self.assertEqual(saved["generation_task_id"], "paid-multiview-task")
            self.assertEqual(persisted["generation_task_id"], "paid-multiview-task")
            self.assertTrue((task_directory / "input-front.png").is_file())
            self.assertEqual(
                Image.open(task_directory / "input-front.png").convert("RGB").getpixel((0, 0)),
                (255, 0, 0),
            )

    def test_generation_requires_explicit_confirmation(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            baseline = runner.load_baseline(self._baseline(root))
            output = root / "output"
            output.mkdir()
            state = runner.load_or_create_state(output, baseline, 300000)
            state["views"] = {view: "unused.png" for view in ("front", "left", "back", "right")}
            with self.assertRaisesRegex(runner.MultiviewValidationError, "confirm-tripo"):
                runner.create_or_resume_generation(output, state, confirm_paid_call=False)

    def test_reviewed_views_require_a_manual_approval_note(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            case = self._baseline(root)
            output = root / "output"
            baseline = runner.load_baseline(case)
            state = runner.load_or_create_state(output, baseline, 300000)
            state["prepare_status"] = "review"
            runner._write_json(output / "multiview-state.json", state)
            with self.assertRaisesRegex(runner.MultiviewValidationError, "manual-approval-note"):
                runner.run_multiview(case, output, allow_reviewed_views=True)

    def test_manual_view_rejection_blocks_generation_until_regeneration(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            case = self._baseline(root)
            output = root / "output"
            baseline = runner.load_baseline(case)
            state = runner.load_or_create_state(output, baseline, 300000)
            state["sheet_sha256"] = "SHEET-A"
            state["prepare_status"] = "review"
            runner._write_json(output / "multiview-state.json", state)
            rejection = runner.record_manual_rejection(case, output, scope="views", note="wrong view count")
            self.assertEqual(rejection["sheet_sha256"], "SHEET-A")
            with self.assertRaisesRegex(runner.MultiviewValidationError, "manually rejected"):
                runner.run_multiview(
                    case, output, allow_reviewed_views=True, manual_approval_note="override",
                )

    def test_manual_result_rejection_updates_comparison(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            case = self._baseline(root)
            output = root / "output"
            baseline = runner.load_baseline(case)
            state = runner.load_or_create_state(output, baseline, 300000)
            artifact = output / "model.obj"
            artifact.write_text("v 0 0 0\n", encoding="utf-8")
            state["artifact"] = str(artifact)
            runner._write_json(output / "multiview-state.json", state)
            review = output / "review"
            review.mkdir()
            (review / "comparison.json").write_text(json.dumps({"outcome": "improved"}), encoding="utf-8")
            rejection = runner.record_manual_rejection(case, output, scope="result", note="floating ring")
            persisted = json.loads((output / "multiview-state.json").read_text(encoding="utf-8"))
            comparison = json.loads((review / "comparison.json").read_text(encoding="utf-8"))
            self.assertEqual(persisted["final_review"]["manual_status"], "rejected")
            self.assertEqual(comparison["manual_outcome"], "rejected")
            self.assertEqual(rejection["note"], "floating ring")

    def test_comparison_reports_visual_and_structure_deltas(self) -> None:
        baseline_quality = {"status": "pass", "metrics": {"face_count": 100, "component_count": 1, "tiny_component_count": 0}}
        baseline_visual = {"status": "review", "score": 72}
        current_quality = {"status": "review", "metrics": {"face_count": 120, "component_count": 2, "tiny_component_count": 1}}
        current_visual = {"status": "review", "score": 84}
        report = runner.build_baseline_comparison(
            "camera", baseline_quality, baseline_visual, current_quality, current_visual,
        )
        self.assertEqual(report["delta"], {
            "visual_score": 12, "face_count": 20, "component_count": 1, "tiny_component_count": 1,
        })
        self.assertEqual(report["outcome"], "improved")

    def test_final_review_requires_confirmation_then_reuses_cache(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            case = self._baseline(root)
            baseline_review = case / "review"
            baseline_review.mkdir()
            (baseline_review / "model-quality.json").write_text(json.dumps({
                "status": "pass", "metrics": {"face_count": 4, "component_count": 1, "tiny_component_count": 0},
            }), encoding="utf-8")
            (baseline_review / "visual-quality.json").write_text(json.dumps({
                "status": "review", "score": 70,
            }), encoding="utf-8")
            output = root / "output"
            baseline = runner.load_baseline(case)
            state = runner.load_or_create_state(output, baseline, 300000)
            artifact = output / "tripo" / "task" / "model.obj"
            artifact.parent.mkdir(parents=True)
            artifact.write_text("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n", encoding="utf-8")
            state["artifact"] = str(artifact)
            runner._write_json(output / "multiview-state.json", state)
            quality = {"status": "pass", "metrics": {"face_count": 1, "component_count": 1, "tiny_component_count": 0}}

            with self.assertRaisesRegex(runner.MultiviewValidationError, "confirm-result"):
                runner.review_multiview_result(case, output, quality_analyzer=lambda path: quality)

            calls = []
            artifact_sha = hashlib.sha256(artifact.read_bytes()).hexdigest()

            def reviewer(*args, **kwargs):
                calls.append(1)
                report = {
                    "status": "review", "score": 82, "review_version": runner.REVIEW_VERSION,
                    "obj_sha256": artifact_sha, "model": "gpt-5.4",
                }
                runner._write_json(output / "review" / "visual-quality.json", report)
                return report

            first = runner.review_multiview_result(
                case, output, confirm_visual_call=True,
                quality_analyzer=lambda path: quality, visual_reviewer=reviewer,
            )
            second = runner.review_multiview_result(
                case, output, quality_analyzer=lambda path: quality,
                visual_reviewer=lambda *args, **kwargs: self.fail("cached review should be reused"),
            )
            self.assertEqual(calls, [1])
            self.assertEqual(first["comparison"]["delta"]["visual_score"], 12)
            self.assertTrue(second["visual"]["cached"])
            runner.record_manual_rejection(case, output, scope="result", note="floating ring")
            third = runner.review_multiview_result(
                case, output, quality_analyzer=lambda path: quality,
                visual_reviewer=lambda *args, **kwargs: self.fail("cached review should be reused"),
            )
            persisted = json.loads((output / "multiview-state.json").read_text(encoding="utf-8"))
            self.assertEqual(third["comparison"]["manual_outcome"], "rejected")
            self.assertEqual(persisted["final_review"]["manual_status"], "rejected")

    def test_final_artifact_must_stay_inside_output(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            case = self._baseline(root)
            output = root / "output"
            baseline = runner.load_baseline(case)
            state = runner.load_or_create_state(output, baseline, 300000)
            outside = root / "outside.obj"
            outside.write_text("v 0 0 0\n", encoding="utf-8")
            state["artifact"] = str(outside)
            runner._write_json(output / "multiview-state.json", state)
            with self.assertRaisesRegex(runner.MultiviewValidationError, "inside"):
                runner.review_multiview_result(case, output)

    def test_regeneration_requires_explicit_repeat_and_archives_previous_sheet(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            case = self._baseline(root)
            output = root / "output"
            baseline = runner.load_baseline(case)
            state = runner.load_or_create_state(output, baseline, 300000)
            state["image2_calls"] = 1
            runner._write_json(output / "multiview-state.json", state)
            sheet = output / "multiview-sheet.png"
            Image.new("RGB", (512, 512), "white").save(sheet)

            with self.assertRaisesRegex(runner.MultiviewValidationError, "allow-repeat"):
                runner.prepare_multiview(case, output, regenerate_sheet=True)

            def recreate(source, destination, description, palette):
                Image.new("RGB", (512, 512), "red").save(destination)
                return destination

            review = {
                "status": "pass", "score": 90, "confidence": 0.9, "summary": "ok",
                "warnings": [], "checks": {},
            }
            result = runner.prepare_multiview(
                case,
                output,
                confirm_image_call=True,
                confirm_visual_call=True,
                allow_repeat_image_call=True,
                regenerate_sheet=True,
                sheet_creator=recreate,
                sheet_reviewer=lambda *args, **kwargs: dict(review),
            )
            self.assertEqual(result["image2_calls"], 2)
            self.assertTrue((output / "attempts" / "image2-attempt-01" / "multiview-sheet.png").is_file())

    def test_generation_guidance_is_forwarded_and_requires_regeneration_when_changed(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            case = self._baseline(root)
            output = root / "output"
            received = []

            def create(source, destination, description, palette):
                received.append(description)
                image = Image.new("RGB", (512, 512), "white")
                colors = [tuple(int(color[index:index + 2], 16) for index in (1, 3, 5)) for color in palette]
                for top in (0, 256):
                    for left in (0, 256):
                        for band, color in enumerate(colors):
                            image.paste(color, (left + band * 64, top, left + (band + 1) * 64, top + 256))
                image.save(destination)
                return destination

            review = {
                "status": "pass", "score": 90, "confidence": 0.9, "summary": "ok",
                "warnings": [], "checks": {},
            }
            result = runner.prepare_multiview(
                case,
                output,
                confirm_image_call=True,
                confirm_visual_call=True,
                generation_guidance="preserve exactly three roofs",
                sheet_creator=create,
                sheet_reviewer=lambda sheet, description, **kwargs: received.append(description) or dict(review),
            )
            self.assertEqual(result["sheet_generation_guidance"], "preserve exactly three roofs")
            self.assertEqual(len(received), 2)
            self.assertTrue(all("preserve exactly three roofs" in value for value in received))
            with self.assertRaisesRegex(runner.MultiviewValidationError, "regenerate-sheet"):
                runner.prepare_multiview(case, output, generation_guidance="two roofs")


if __name__ == "__main__":
    unittest.main()
