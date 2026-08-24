from __future__ import annotations

import json
from pathlib import Path
import tempfile
import unittest

from PIL import Image

from tools.ai import printable_multiview_reference as multiview


PALETTE = ("#D94F4F", "#222222", "#E8E2D0", "#3A7CA5")


class PrintableMultiviewReferenceTests(unittest.TestCase):
    def test_prompt_freezes_layout_identity_and_palette(self) -> None:
        prompt = multiview.build_multiview_sheet_prompt("one camera", PALETTE)
        self.assertIn("top-left FRONT", prompt)
        self.assertIn("top-right LEFT SIDE", prompt)
        self.assertIn("bottom-left BACK", prompt)
        self.assertIn("bottom-right RIGHT SIDE", prompt)
        self.assertIn("Do not redesign", prompt)
        self.assertIn("one single coherent solid 3D sculpture", prompt)
        self.assertIn("0, +90, 180, and -90", prompt)
        self.assertIn("same depth, thickness, attachment and elevation", prompt)
        for color in PALETTE:
            self.assertIn(color, prompt)

    def test_split_uses_fixed_quadrants(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            sheet = Image.new("RGB", (600, 800))
            colors = {
                "front": (255, 0, 0), "left": (0, 255, 0),
                "back": (0, 0, 255), "right": (255, 255, 255),
            }
            sheet.paste(colors["front"], (0, 0, 300, 400))
            sheet.paste(colors["left"], (300, 0, 600, 400))
            sheet.paste(colors["back"], (0, 400, 300, 800))
            sheet.paste(colors["right"], (300, 400, 600, 800))
            path = root / "sheet.png"
            sheet.save(path)
            crops = multiview.split_multiview_sheet(path, root / "crops")
            for view, crop in crops.items():
                with Image.open(crop) as opened:
                    self.assertEqual(opened.size, (300, 400))
                    self.assertEqual(opened.convert("RGB").getpixel((10, 10)), colors[view])

    def test_small_sheet_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "small.png"
            Image.new("RGB", (511, 512), "white").save(path)
            with self.assertRaisesRegex(multiview.MultiviewReferenceError, "at least"):
                multiview.split_multiview_sheet(path, Path(directory) / "output")

    def test_review_requires_every_check_to_pass(self) -> None:
        response = json.dumps({
            "summary": "总体一致",
            "score": 91,
            "confidence": 0.9,
            "checks": {check: {"status": "pass", "reason": "ok"} for check in multiview.CHECK_IDS},
        })
        report = multiview.review_multiview_sheet(
            Path("unused.png"), "camera",
            completion=lambda system, user, images: response,
        )
        self.assertEqual(report["status"], "pass")
        self.assertEqual(report["warnings"], [])

        changed = json.loads(response)
        changed["checks"]["geometry"]["status"] = "review"
        report = multiview.review_multiview_sheet(
            Path("unused.png"), "camera",
            completion=lambda system, user, images: json.dumps(changed),
        )
        self.assertEqual(report["status"], "review")
        self.assertEqual(report["warnings"], ["geometry"])

    def test_review_can_compare_source_before_sheet(self) -> None:
        response = json.dumps({
            "summary": "一致", "score": 90, "confidence": 0.9,
            "checks": {check: {"status": "pass", "reason": "ok"} for check in multiview.CHECK_IDS},
        })

        def completion(system, user, images):
            self.assertEqual(images, (Path("source.png"), Path("sheet.png")))
            self.assertIn("supersedes any three-quarter", system)
            return response

        report = multiview.review_multiview_sheet(
            Path("sheet.png"), "camera", source_path=Path("source.png"), completion=completion,
        )
        self.assertEqual(report["status"], "pass")


if __name__ == "__main__":
    unittest.main()
