from __future__ import annotations

import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from tools.ai.openai_preprocessor import OpenAIPreprocessorError
from tools.ai.printable_palette_visual_quality import (
    CHECK_IDS,
    review_printable_palette_visual_quality,
)


def _recommendation() -> dict[str, object]:
    return {
        "summary": "清晰的大块玩具配色。",
        "colors": [
            {"hex": "#C95D3A", "name": "陶土橙", "role": "primary", "usage": "主体", "reason": "视觉中心"},
            {"hex": "#263238", "name": "炭黑", "role": "structure", "usage": "结构", "reason": "轮廓"},
            {"hex": "#F2E8D5", "name": "暖白", "role": "light", "usage": "亮部", "reason": "对比"},
            {"hex": "#2A8C78", "name": "青绿", "role": "accent", "usage": "强调", "reason": "识别"},
        ],
    }


def _response(*, score: int = 88, review_check: str = "") -> str:
    checks = {
        check_id: {
            "status": "review" if check_id == review_check else "pass",
            "score": 68 if check_id == review_check else 90,
            "reason": "需要复核" if check_id == review_check else "表现清晰",
        }
        for check_id in CHECK_IDS
    }
    return json.dumps({
        "summary": "四色角色清晰，适合进入后续建模。",
        "score": score,
        "confidence": 0.91,
        "checks": checks,
    }, ensure_ascii=False)


class PrintablePaletteVisualQualityTests(unittest.TestCase):
    def test_review_prompt_uses_the_actual_dynamic_palette_count(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            preview = root / "strict.png"
            preview.write_bytes(b"strict-preview")
            recommendation = _recommendation()
            recommendation["colors"].extend([
                {"hex": "#3267A8", "name": "钴蓝", "role": "secondary", "usage": "次要区域", "reason": "分区"},
                {"hex": "#9B3F77", "name": "莓紫", "role": "detail", "usage": "识别细节", "reason": "识别"},
            ])
            prompts: list[str] = []

            def complete(system: str, user: str, _images: tuple[Path, ...]) -> str:
                prompts.extend((system, user))
                return _response()

            report = review_printable_palette_visual_quality(
                preview,
                root,
                prompt="一个玩具机器人",
                style="q_cartoon",
                recommendation=recommendation,
                completion=complete,
            )

            self.assertEqual(report["status"], "pass")
            self.assertIn("6-color", prompts[0])
            self.assertIn("推荐6色", prompts[1])

    def test_valid_fenced_json_is_normalized(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            preview = root / "strict.png"
            preview.write_bytes(b"strict-preview")

            report = review_printable_palette_visual_quality(
                preview,
                root,
                prompt="一个玩具机器人",
                style="q_cartoon",
                recommendation=_recommendation(),
                completion=lambda *args: "```json\n" + _response() + "\n```",
            )

            self.assertEqual(report["status"], "pass")
            self.assertEqual(report["score"], 88)
            self.assertEqual(set(report["checks"]), set(CHECK_IDS))
            self.assertEqual(len(report["strict_preview_sha256"]), 64)

    def test_missing_check_fails_closed_as_unavailable(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            preview = root / "strict.png"
            preview.write_bytes(b"strict-preview")
            raw = json.loads(_response())
            raw["checks"].pop(CHECK_IDS[-1])

            report = review_printable_palette_visual_quality(
                preview,
                root,
                prompt="一个玩具",
                style="low_poly",
                recommendation=_recommendation(),
                completion=lambda *args: json.dumps(raw),
            )

            self.assertEqual(report["status"], "unavailable")
            self.assertIn("visual_review_unavailable", report["errors"])

    def test_review_check_or_score_below_80_requires_review(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            preview = root / "strict.png"
            preview.write_bytes(b"strict-preview")

            check_report = review_printable_palette_visual_quality(
                preview, root / "check", prompt="x", style="q_cartoon",
                recommendation=_recommendation(),
                completion=lambda *args: _response(score=92, review_check="role_usage"),
            )
            score_report = review_printable_palette_visual_quality(
                preview, root / "score", prompt="x", style="q_cartoon",
                recommendation=_recommendation(),
                completion=lambda *args: _response(score=79),
            )

            self.assertEqual(check_report["status"], "review")
            self.assertIn("palette_role_usage_unclear", check_report["warnings"])
            self.assertEqual(score_report["status"], "review")
            self.assertIn("palette_visual_score_low", score_report["warnings"])

    def test_provider_error_degrades_without_raising(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            preview = root / "strict.png"
            preview.write_bytes(b"strict-preview")

            def fail(*args):
                raise OpenAIPreprocessorError("provider unavailable")

            report = review_printable_palette_visual_quality(
                preview, root, prompt="x", style="sculpture",
                recommendation=_recommendation(), completion=fail,
            )

            self.assertEqual(report["status"], "unavailable")
            self.assertIn("provider unavailable", report["diagnostic"])

    def test_matching_report_is_cached_by_preview_reference_and_model(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            preview = root / "strict.png"
            reference = root / "reference.png"
            preview.write_bytes(b"strict-preview")
            reference.write_bytes(b"reference")
            calls: list[tuple[Path, ...]] = []

            def complete(system, user, images):
                calls.append(images)
                return _response()

            with patch.dict(os.environ, {"OPENAI_TEXT_MODEL": "quality-model"}, clear=False):
                first = review_printable_palette_visual_quality(
                    preview, root / "review", prompt="x", style="q_cartoon",
                    recommendation=_recommendation(), reference_path=reference, completion=complete,
                )
                second = review_printable_palette_visual_quality(
                    preview, root / "review", prompt="x", style="q_cartoon",
                    recommendation=_recommendation(), reference_path=reference, completion=complete,
                )

            self.assertFalse(first["cached"])
            self.assertTrue(second["cached"])
            self.assertEqual(calls, [(reference, preview)])


if __name__ == "__main__":
    unittest.main()
