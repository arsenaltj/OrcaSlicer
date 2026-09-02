import json
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from PIL import Image

from tools.ai import printable_reference_visual_quality as quality


review_prepared_reference = quality.review_prepared_reference


class ReferenceVisualQualityTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.original = self.root / "original.png"
        self.natural = self.root / "natural.png"
        self.printable = self.root / "printable.png"
        for index, path in enumerate((self.original, self.natural, self.printable)):
            Image.new("RGB", (32, 48), (80 + index * 20, 100, 120)).save(path)

    @staticmethod
    def response(identity=92, material=94):
        scores = {
            "identity_likeness": identity,
            "face_geometry": identity,
            "age_expression": identity,
            "pose_clothing": 94,
            "material_ownership": material,
            "base_integrity": material,
            "modeling_reference": 91,
        }
        return json.dumps({
            "summary": "可用",
            "score": min(scores.values()),
            "confidence": 0.9,
            "checks": {
                name: {"status": "pass", "score": score, "reason": "检查完成"}
                for name, score in scores.items()
            },
        })

    def test_passes_a_high_scoring_reference_and_reuses_cache(self):
        calls = []

        def complete(*args):
            calls.append(args)
            self.assertEqual(len(args[2]), 2)
            self.assertEqual(args[2][0], self.original)
            self.assertTrue(args[2][1].name.endswith("reference-review-sheet.png"))
            with Image.open(args[2][1]) as sheet:
                self.assertEqual(sheet.size, (3072, 1536))
            return self.response()

        first = review_prepared_reference(
            self.original, self.natural, self.printable, self.root / "report", completion=complete
        )
        second = review_prepared_reference(
            self.original, self.natural, self.printable, self.root / "report", completion=complete
        )

        self.assertEqual(first["status"], "pass")
        self.assertTrue(first["model_generation_recommended"])
        self.assertTrue(second["cached"])
        self.assertEqual(len(calls), 1)

    def test_score_floor_blocks_provider_pass_for_identity_or_material_drift(self):
        report = review_prepared_reference(
            self.original,
            self.natural,
            self.printable,
            self.root / "report",
            completion=lambda *_args: self.response(identity=82, material=60),
        )

        self.assertEqual(report["status"], "review")
        self.assertFalse(report["model_generation_recommended"])
        self.assertIn("preview_identity_mismatch", report["blocking_warnings"])
        self.assertIn("preview_material_mixing", report["blocking_warnings"])

    def test_source_locked_face_prevents_a_false_visual_identity_rejection(self):
        fidelity = {
            "available": True,
            "source_locked": True,
            "mean_absolute_error": 0.5,
            "exact_pixel_ratio": 0.94,
            "sample_count": 12000,
        }
        with mock.patch.object(quality, "_face_pixel_fidelity", return_value=fidelity):
            report = review_prepared_reference(
                self.original,
                self.natural,
                self.printable,
                self.root / "report",
                completion=lambda *_args: self.response(identity=70, material=94),
            )

        self.assertEqual(report["status"], "pass")
        self.assertTrue(report["model_generation_recommended"])
        self.assertGreaterEqual(report["checks"]["identity_likeness"]["score"], 95)
        self.assertEqual(report["deterministic_face_fidelity"], fidelity)


if __name__ == "__main__":
    unittest.main()
