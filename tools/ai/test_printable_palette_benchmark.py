from __future__ import annotations

import json
import hashlib
from pathlib import Path
import tempfile
import unittest
from types import SimpleNamespace

from tools.ai.openai_preprocessor import (
    PrintablePaletteRecommendation,
    PrintablePaletteRecommendationColor,
)
from tools.ai.printable_palette_benchmark import (
    PaletteBenchmarkError,
    collect_results,
    load_case_state,
    load_manifest,
    parse_args,
    prepare_case,
    record_manual_review,
    review_case,
    select_cases,
)


def _manifest(cases: list[dict[str, object]]) -> dict[str, object]:
    return {
        "schema_version": 1,
        "benchmark_id": "palette-test-v1",
        "frozen_at": "2026-08-23",
        "cases": cases,
    }


class PrintablePaletteBenchmarkManifestTests(unittest.TestCase):
    def test_load_manifest_normalizes_cases_and_has_stable_fingerprint(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            payload = _manifest([
                {
                    "id": "toy_robot",
                    "category": "character",
                    "style": "q_cartoon",
                    "prompt": "  一个粗壮完整的玩具机器人  ",
                },
                {
                    "id": "desk_lamp",
                    "category": "stable_product",
                    "style": "low_poly",
                    "prompt": "一盏稳定落地的收藏台灯",
                },
            ])
            path = root / "manifest.json"
            path.write_text(json.dumps(payload, ensure_ascii=False), encoding="utf-8")

            first = load_manifest(path)
            second = load_manifest(path)

            self.assertEqual(first.benchmark_id, "palette-test-v1")
            self.assertEqual(first.cases[0].prompt, "一个粗壮完整的玩具机器人")
            self.assertEqual(first.fingerprint, second.fingerprint)
            self.assertEqual(len(first.fingerprint), 64)

    def test_load_manifest_rejects_duplicate_case_ids(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "manifest.json"
            case = {
                "id": "duplicate",
                "category": "character",
                "style": "q_cartoon",
                "prompt": "一个玩具",
            }
            path.write_text(json.dumps(_manifest([case, dict(case)])), encoding="utf-8")
            with self.assertRaisesRegex(PaletteBenchmarkError, "unique"):
                load_manifest(path)

    def test_load_manifest_rejects_unknown_style_and_empty_prompt(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            unknown = root / "unknown.json"
            unknown.write_text(json.dumps(_manifest([{
                "id": "bad_style", "category": "x", "style": "photo", "prompt": "x",
            }])), encoding="utf-8")
            with self.assertRaisesRegex(PaletteBenchmarkError, "style"):
                load_manifest(unknown)

            empty = root / "empty.json"
            empty.write_text(json.dumps(_manifest([{
                "id": "empty", "category": "x", "style": "sculpture", "prompt": "  ",
            }])), encoding="utf-8")
            with self.assertRaisesRegex(PaletteBenchmarkError, "prompt"):
                load_manifest(empty)

    def test_reference_image_must_stay_inside_manifest_directory(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            outside = root.parent / "outside-reference.png"
            outside.write_bytes(b"outside")
            path = root / "manifest.json"
            path.write_text(json.dumps(_manifest([{
                "id": "escape",
                "category": "image",
                "style": "enamel_inlay",
                "prompt": "保持主体",
                "reference_image": "../outside-reference.png",
            }])), encoding="utf-8")
            try:
                with self.assertRaisesRegex(PaletteBenchmarkError, "inside"):
                    load_manifest(path)
            finally:
                outside.unlink(missing_ok=True)

    def test_reference_image_is_resolved_and_must_exist(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            image = root / "reference.png"
            image.write_bytes(b"image")
            path = root / "manifest.json"
            path.write_text(json.dumps(_manifest([{
                "id": "image_case",
                "category": "image",
                "style": "cel_shaded",
                "prompt": "保留主体身份",
                "reference_image": "reference.png",
            }])), encoding="utf-8")

            manifest = load_manifest(path)

            self.assertEqual(manifest.cases[0].reference_image, image.resolve())

    def test_select_cases_preserves_manifest_order_and_rejects_unknown_ids(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "manifest.json"
            path.write_text(json.dumps(_manifest([
                {"id": "first", "category": "x", "style": "q_cartoon", "prompt": "a"},
                {"id": "second", "category": "x", "style": "low_poly", "prompt": "b"},
            ])), encoding="utf-8")
            manifest = load_manifest(path)

            selected = select_cases(manifest.cases, ["second", "first"])

            self.assertEqual([case.case_id for case in selected], ["first", "second"])
            with self.assertRaisesRegex(PaletteBenchmarkError, "unknown"):
                select_cases(manifest.cases, ["missing"])


def _recommendation() -> PrintablePaletteRecommendation:
    colors = (
        ("#C95D3A", "陶土橙", "primary"),
        ("#263238", "炭黑", "structure"),
        ("#F2E8D5", "暖白", "light"),
        ("#2A8C78", "青绿", "accent"),
    )
    return PrintablePaletteRecommendation(
        "稳重且适合大块拼色的收藏玩具配色。",
        tuple(
            PrintablePaletteRecommendationColor(color, name, role, "大块区域", "增强结构可读性")
            for color, name, role in colors
        ),
    )


class PrintablePaletteBenchmarkStateTests(unittest.TestCase):
    def _loaded(self, root: Path):
        path = root / "manifest.json"
        path.write_text(json.dumps(_manifest([{
            "id": "robot", "category": "character", "style": "q_cartoon", "prompt": "一个完整机器人",
        }])), encoding="utf-8")
        manifest = load_manifest(path)
        return manifest, manifest.cases[0]

    def test_new_state_is_persisted_and_fingerprint_mismatch_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest, case = self._loaded(root)
            output = root / "output"

            state = load_case_state(output, manifest, case)

            self.assertEqual(state["case_id"], "robot")
            self.assertEqual(state["stages"]["recommendation"]["status"], "pending")
            saved = json.loads((output / "palette-case-state.json").read_text(encoding="utf-8"))
            self.assertEqual(saved["manifest_fingerprint"], manifest.fingerprint)
            saved["manifest_fingerprint"] = "different"
            (output / "palette-case-state.json").write_text(json.dumps(saved), encoding="utf-8")
            with self.assertRaisesRegex(PaletteBenchmarkError, "fingerprint"):
                load_case_state(output, manifest, case)

    def test_prepare_requires_explicit_paid_confirmation(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest, case = self._loaded(root)
            with self.assertRaisesRegex(PaletteBenchmarkError, "confirm-recommendation"):
                prepare_case(root / "output", manifest, case)

    def test_successful_recommendation_is_reused_without_duplicate_call(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest, case = self._loaded(root)
            calls: list[str] = []

            def recommend(*args, **kwargs):
                calls.append("recommend")
                return _recommendation()

            state = prepare_case(
                root / "output", manifest, case,
                confirm_recommendation_call=True,
                stop_after="recommendation",
                recommender=recommend,
            )
            resumed = prepare_case(
                root / "output", manifest, case,
                stop_after="recommendation",
                recommender=recommend,
            )

            self.assertEqual(calls, ["recommend"])
            self.assertEqual(state["paid_calls"]["recommendation"], 1)
            self.assertEqual(resumed["recommendation"]["colors"][0]["role"], "primary")

    def test_uncertain_recommendation_refuses_automatic_retry(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest, case = self._loaded(root)

            def fail(*args, **kwargs):
                raise RuntimeError("provider connection ended")

            with self.assertRaisesRegex(RuntimeError, "provider connection"):
                prepare_case(
                    root / "output", manifest, case,
                    confirm_recommendation_call=True,
                    stop_after="recommendation",
                    recommender=fail,
                )
            with self.assertRaisesRegex(PaletteBenchmarkError, "uncertain"):
                prepare_case(
                    root / "output", manifest, case,
                    confirm_recommendation_call=True,
                    stop_after="recommendation",
                    recommender=lambda *args, **kwargs: _recommendation(),
                )

    def test_preview_and_local_gate_are_hashed_and_reused(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest, case = self._loaded(root)
            image_calls: list[str] = []
            process_calls: list[str] = []

            def generate(*args, **kwargs):
                destination = Path(args[1])
                destination.write_bytes(b"provider-image")
                image_calls.append(destination.name)
                return destination

            def process(source, output, palette, settings, roles):
                process_calls.append(Path(source).name)
                output = Path(output)
                strict = output / "strict.png"
                clean = output / "clean.png"
                model = output / "model.png"
                metadata = output / "preview-colors.json"
                for path, payload in ((strict, b"strict"), (clean, b"clean"), (model, b"model")):
                    path.write_bytes(payload)
                metadata.write_text("{}", encoding="utf-8")
                return SimpleNamespace(
                    strict_preview=strict,
                    clean_preview=clean,
                    model_reference=model,
                    metadata=metadata,
                    metrics={"palette_quality_ok": True, "meaningful_subject_color_count": 4},
                    palette_usage={"#C95D3A": 100},
                )

            state = prepare_case(
                root / "output", manifest, case,
                confirm_recommendation_call=True,
                confirm_image_call=True,
                recommender=lambda *args, **kwargs: _recommendation(),
                image_generator=generate,
                processor=process,
            )
            resumed = prepare_case(
                root / "output", manifest, case,
                recommender=lambda *args, **kwargs: _recommendation(),
                image_generator=generate,
                processor=process,
            )

            self.assertEqual(image_calls, ["provider-preview.png"])
            self.assertEqual(process_calls, ["provider-preview.png"])
            self.assertEqual(state["paid_calls"]["image2"], 1)
            self.assertEqual(len(state["artifacts"]["provider_preview"]["sha256"]), 64)
            self.assertTrue(resumed["metrics"]["palette_quality_ok"])

    def test_visual_review_requires_confirmation_and_is_reused(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest, case = self._loaded(root)
            output = root / "output"

            def generate(*args, **kwargs):
                destination = Path(args[1])
                destination.write_bytes(b"provider-image")
                return destination

            def process(source, output, palette, settings, roles):
                output = Path(output)
                strict = output / "strict.png"
                clean = output / "clean.png"
                model = output / "model.png"
                metadata = output / "preview-colors.json"
                for path in (strict, clean, model):
                    path.write_bytes(path.name.encode())
                metadata.write_text("{}", encoding="utf-8")
                return SimpleNamespace(
                    strict_preview=strict, clean_preview=clean, model_reference=model, metadata=metadata,
                    metrics={"palette_quality_ok": True}, palette_usage={},
                )

            prepare_case(
                output, manifest, case,
                confirm_recommendation_call=True,
                confirm_image_call=True,
                recommender=lambda *args, **kwargs: _recommendation(),
                image_generator=generate,
                processor=process,
            )
            with self.assertRaisesRegex(PaletteBenchmarkError, "confirm-visual"):
                review_case(output, manifest, case)

            calls: list[str] = []

            def reviewer(strict, review_output, **kwargs):
                calls.append(Path(strict).name)
                report = {
                    "status": "pass", "score": 91, "warnings": [], "errors": [],
                    "strict_preview_sha256": "ignored-by-runner",
                }
                destination = Path(review_output) / "palette-visual-quality.json"
                destination.write_text(json.dumps(report), encoding="utf-8")
                return report

            state = review_case(
                output, manifest, case,
                confirm_visual_call=True,
                reviewer=reviewer,
            )
            resumed = review_case(output, manifest, case, reviewer=reviewer)

            self.assertEqual(calls, ["strict.png"])
            self.assertEqual(state["paid_calls"]["visual_review"], 1)
            self.assertEqual(state["visual_review"]["status"], "pass")
            self.assertEqual(resumed["visual_review"]["score"], 91)


class PrintablePaletteBenchmarkSummaryTests(unittest.TestCase):
    def _loaded(self, root: Path):
        path = root / "manifest.json"
        path.write_text(json.dumps(_manifest([
            {"id": "first", "category": "character", "style": "q_cartoon", "prompt": "第一个玩具"},
            {"id": "second", "category": "product", "style": "low_poly", "prompt": "第二个玩具"},
        ])), encoding="utf-8")
        return load_manifest(path)

    @staticmethod
    def _sha(payload: bytes) -> str:
        return hashlib.sha256(payload).hexdigest()

    def test_manual_approval_and_summary_require_matching_complete_evidence(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "output"
            manifest = self._loaded(Path(directory))
            case = manifest.cases[0]
            case_root = output / case.case_id
            state = load_case_state(case_root, manifest, case)
            strict_payload = b"strict"
            strict = case_root / "strict.png"
            strict.write_bytes(strict_payload)
            visual_payload = json.dumps({"status": "pass", "score": 90}).encode()
            visual = case_root / "visual.json"
            visual.write_bytes(visual_payload)
            state["artifacts"].update({
                "strict_preview": {"path": "strict.png", "sha256": self._sha(strict_payload)},
                "visual_review": {"path": "visual.json", "sha256": self._sha(visual_payload)},
            })
            state["stages"]["local_gate"]["status"] = "complete"
            state["stages"]["visual_review"]["status"] = "complete"
            state["metrics"] = {"palette_quality_ok": True, "meaningful_subject_color_count": 4}
            state["visual_review"] = {"status": "review", "score": 82}
            (case_root / "palette-case-state.json").write_text(json.dumps(state), encoding="utf-8")

            recorded = record_manual_review(
                case_root, manifest, case, decision="approved", note="轮廓和大色块清楚"
            )
            summary = collect_results(output, manifest)

            self.assertEqual(recorded["manual_review"]["decision"], "approved")
            self.assertTrue(summary["cases"][0]["tripo_candidate"])
            self.assertFalse(summary["cases"][1]["tripo_candidate"])
            self.assertEqual(summary["totals"]["tripo_candidates"], 1)
            self.assertTrue((output / "benchmark-summary.json").is_file())
            self.assertTrue((output / "benchmark-summary.csv").is_file())

            strict.write_bytes(b"changed")
            changed = collect_results(output, manifest)
            self.assertFalse(changed["cases"][0]["tripo_candidate"])

    def test_manual_review_rejects_invalid_decision_or_missing_strict_preview(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest = self._loaded(root)
            case = manifest.cases[0]
            case_root = root / "output" / case.case_id
            load_case_state(case_root, manifest, case)
            with self.assertRaisesRegex(PaletteBenchmarkError, "decision"):
                record_manual_review(case_root, manifest, case, decision="maybe", note="x")
            with self.assertRaisesRegex(PaletteBenchmarkError, "strict preview"):
                record_manual_review(case_root, manifest, case, decision="rejected", note="轮廓缺失")

    def test_parse_args_supports_case_filter_and_explicit_confirmations(self) -> None:
        args = parse_args([
            "prepare", "--case", "first", "--case", "second",
            "--stop-after", "preview", "--confirm-recommendation-call", "--confirm-image-call",
        ])
        self.assertEqual(args.action, "prepare")
        self.assertEqual(args.case, ["first", "second"])
        self.assertTrue(args.confirm_recommendation_call)
        self.assertTrue(args.confirm_image_call)


if __name__ == "__main__":
    unittest.main()
