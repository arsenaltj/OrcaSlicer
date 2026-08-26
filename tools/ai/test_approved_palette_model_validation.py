from __future__ import annotations

import hashlib
import json
from pathlib import Path
import tempfile
import unittest

from tools.ai.approved_palette_model_validation import (
    ApprovedModelValidationError,
    preflight_candidate,
    run_validation,
)
from tools.ai.printable_palette_benchmark import (
    load_case_state,
    load_manifest,
    record_manual_review,
)


class ApprovedPaletteModelValidationTests(unittest.TestCase):
    def _fixture(self, root: Path, *, approved: bool = True) -> tuple[Path, Path, Path]:
        manifest_path = root / "manifest.json"
        manifest_path.write_text(json.dumps({
            "schema_version": 1,
            "benchmark_id": "approved-model-test-v1",
            "frozen_at": "2026-08-23",
            "cases": [{
                "id": "radio",
                "category": "stable_product",
                "style": "low_poly",
                "prompt": "一台结构稳定的四色收音机",
            }],
        }, ensure_ascii=False), encoding="utf-8")
        output = root / "output"
        case_root = output / "radio"
        manifest = load_manifest(manifest_path)
        case = manifest.cases[0]
        state = load_case_state(case_root, manifest, case)
        strict = case_root / "strict.png"
        visual = case_root / "visual.json"
        reference = case_root / "reference.png"
        strict.write_bytes(b"strict")
        visual.write_text('{"status":"review"}', encoding="utf-8")
        reference.write_bytes(b"reference")

        def record(path: Path) -> dict[str, str]:
            return {
                "path": path.relative_to(case_root).as_posix(),
                "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
            }

        state["stages"]["local_gate"]["status"] = "complete"
        state["stages"]["visual_review"]["status"] = "complete"
        state["metrics"] = {"palette_quality_ok": True}
        state["visual_review"] = {"status": "review", "score": 84}
        state["recommendation"] = {"colors": [
            {"hex": "#5A3E2B"},
            {"hex": "#2A1F1A"},
            {"hex": "#D8C7A8"},
            {"hex": "#C46A3A"},
        ]}
        state["artifacts"].update({
            "strict_preview": record(strict),
            "visual_review": record(visual),
            "model_reference": record(reference),
        })
        (case_root / "palette-case-state.json").write_text(
            json.dumps(state, ensure_ascii=False), encoding="utf-8"
        )
        if approved:
            record_manual_review(
                case_root,
                manifest,
                case,
                decision="approved",
                note="结构和大色块适合进入一次模型验证",
            )
        return manifest_path, output, case_root

    def test_preflight_rejects_case_without_explicit_approval(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            manifest, output, _ = self._fixture(Path(directory), approved=False)
            with self.assertRaisesRegex(ApprovedModelValidationError, "manual approval"):
                preflight_candidate(manifest, output, "radio")

    def test_preflight_returns_hash_bound_palette_and_reference(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            manifest, output, _ = self._fixture(Path(directory))
            result = preflight_candidate(manifest, output, "radio")
            self.assertEqual(result["case_id"], "radio")
            self.assertEqual(len(result["palette"]), 4)
            self.assertEqual(result["manual_decision"], "approved")
            self.assertEqual(len(result["model_reference"]["sha256"]), 64)

    def test_preflight_rejects_evidence_changed_after_approval(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            manifest, output, case_root = self._fixture(Path(directory))
            (case_root / "strict.png").write_bytes(b"changed")
            with self.assertRaises((ApprovedModelValidationError, ValueError)):
                preflight_candidate(manifest, output, "radio")

    def test_run_writes_v9_quality_report_and_passes_exact_palette(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            manifest, output, case_root = self._fixture(Path(directory))
            calls: list[tuple[Path, Path, bool, int, tuple[str, ...]]] = []

            def runner(input_path, output_root, confirm, face_limit, palette):
                calls.append((input_path, output_root, confirm, face_limit, palette))
                artifact = output_root / "task" / "model-vertex-color.obj"
                artifact.parent.mkdir(parents=True)
                artifact.write_text("v 0 0 0 1 0 0\n", encoding="utf-8")
                return artifact

            def analyzer(path, **kwargs):
                self.assertEqual(path.name, "model-vertex-color.obj")
                self.assertTrue(kwargs["allow_repairable_topology"])
                self.assertEqual(len(kwargs["target_palette"]), 4)
                return {"schema_version": 1, "gate_version": "structural-v9", "status": "review"}

            result = run_validation(
                manifest,
                output,
                "radio",
                confirm_paid_call=True,
                paid_runner=runner,
                quality_analyzer=analyzer,
            )
            self.assertEqual(len(calls), 1)
            self.assertEqual(calls[0][0], (case_root / "reference.png").resolve())
            self.assertEqual(calls[0][4], tuple(result["palette"]))
            self.assertTrue(result["accepted_for_review"])
            self.assertEqual(result["quality_report"]["gate_version"], "structural-v9")
            self.assertTrue((case_root / "tripo" / "approved-model-preflight.json").is_file())
            self.assertTrue((case_root / "tripo" / "final-model-quality.json").is_file())
            self.assertTrue((case_root / "tripo" / "approved-model-validation.json").is_file())


if __name__ == "__main__":
    unittest.main()
