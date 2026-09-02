#!/usr/bin/env python3
import json
import tempfile
import unittest
from pathlib import Path
import sys

from PIL import Image

TOOLS_AI = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS_AI))
from printable_visual_quality import review_model_visual_quality


OBJ = """\
v 0 0 0 1 0 0
v 10 0 0 0 1 0
v 0 10 0 0 0 1
v 0 0 10 1 1 1
f 1 3 2
f 1 2 4
f 1 4 3
f 2 3 4
"""


class PrintableVisualQualityTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.source = self.root / "model.obj"
        self.source.write_text(OBJ, encoding="ascii")

    def tearDown(self):
        self.temporary.cleanup()

    @staticmethod
    def passing_response(system, user, images):
        del system, user, images
        checks = {
            name: {"status": "pass", "score": 90, "reason": "外观正常"}
            for name in (
                "subject_complete", "semantic_coherence", "base_relationship", "detached_artifacts",
                "silhouette_readability", "color_region_clarity", "identity_likeness",
                "material_color_ownership",
            )
        }
        return json.dumps({"summary": "模型外观完整。", "score": 90, "confidence": 0.8, "checks": checks}, ensure_ascii=False)

    def test_pass_report_is_structured_and_cached(self):
        calls = []

        def completion(system, user, images):
            calls.append((system, user, images))
            return self.passing_response(system, user, images)

        first = review_model_visual_quality(self.source, self.root, description="玩具", completion=completion)
        second = review_model_visual_quality(self.source, self.root, description="玩具", completion=completion)

        self.assertEqual(first["status"], "pass")
        self.assertTrue(first["import_recommended"])
        self.assertEqual(second["status"], "pass")
        self.assertTrue(second["cached"])
        self.assertEqual(len(calls), 1)
        self.assertTrue((self.root / "visual-quality.json").is_file())

    def test_failed_check_is_advisory_review_never_reject(self):
        def completion(system, user, images):
            raw = json.loads(self.passing_response(system, user, images))
            raw["checks"]["detached_artifacts"] = {"status": "review", "score": 40, "reason": "疑似漂浮物"}
            return json.dumps(raw, ensure_ascii=False)

        report = review_model_visual_quality(self.source, self.root, completion=completion)

        self.assertEqual(report["status"], "review")
        self.assertEqual(report["warnings"], ["visual_detached_artifacts"])
        self.assertFalse(report["import_recommended"])

    def test_changing_reference_invalidates_cached_identity_review(self):
        first_reference = self.root / "first.png"
        second_reference = self.root / "second.png"
        first_reference.write_bytes(b"first-reference")
        second_reference.write_bytes(b"second-reference")
        calls = []

        def completion(system, user, images):
            calls.append(images)
            return self.passing_response(system, user, images)

        first = review_model_visual_quality(
            self.source, self.root, reference_path=first_reference, completion=completion
        )
        second = review_model_visual_quality(
            self.source, self.root, reference_path=second_reference, completion=completion
        )

        self.assertFalse(first["cached"])
        self.assertFalse(second["cached"])
        self.assertNotEqual(first["review_context_sha256"], second["review_context_sha256"])
        self.assertEqual(len(calls), 2)

    def test_approved_modeling_reference_defines_intentional_crop(self):
        original = self.root / "original.png"
        modeling = self.root / "modeling.png"
        Image.new("RGB", (320, 480), (210, 190, 170)).save(original)
        Image.new("RGB", (480, 480), (180, 180, 180)).save(modeling)

        def completion(system, user, images):
            self.assertIn("approved 3D modeling reference", system)
            self.assertIn("never mark arms, lower body", system)
            self.assertIn("已确认的实际建模参考", user)
            self.assertEqual(images[0], self.root / "visual-reference-sheet.png")
            self.assertEqual(images[1], self.root / "model-view-sheet.png")
            self.assertEqual(len(images), 2)
            with Image.open(images[0]) as reference_sheet:
                self.assertEqual(reference_sheet.size, (1536, 802))
            return self.passing_response(system, user, images)

        report = review_model_visual_quality(
            self.source,
            self.root,
            reference_path=original,
            modeling_reference_path=modeling,
            completion=completion,
        )

        self.assertEqual(report["status"], "pass")
        self.assertTrue(report["import_recommended"])

    def test_portrait_likeness_and_material_mixing_have_specific_warnings(self):
        def completion(system, user, images):
            self.assertIn("generic, doll-like", system)
            self.assertIn("肤色、衣物、头发和底座", user)
            raw = json.loads(self.passing_response(system, user, images))
            raw["checks"]["identity_likeness"] = {
                "status": "review", "score": 35, "reason": "人脸已泛化",
            }
            raw["checks"]["material_color_ownership"] = {
                "status": "review", "score": 40, "reason": "袖口出现肤色",
            }
            return json.dumps(raw, ensure_ascii=False)

        report = review_model_visual_quality(
            self.source,
            self.root,
            description="写实人像",
            reference_path=self.source,
            completion=completion,
        )

        self.assertEqual(report["status"], "review")
        self.assertEqual(
            report["warnings"],
            ["visual_identity_mismatch", "visual_material_color_mixing"],
        )
        self.assertEqual(report["blocking_warnings"], report["warnings"])
        self.assertFalse(report["import_recommended"])

    def test_low_scores_cannot_be_mislabeled_as_pass(self):
        def completion(system, user, images):
            raw = json.loads(self.passing_response(system, user, images))
            raw["checks"]["identity_likeness"] = {
                "status": "pass", "score": 68, "reason": "五官像另一个人",
            }
            raw["checks"]["material_color_ownership"] = {
                "status": "pass", "score": 58, "reason": "袖口存在肤色",
            }
            return json.dumps(raw, ensure_ascii=False)

        report = review_model_visual_quality(
            self.source,
            self.root,
            reference_path=self.source,
            completion=completion,
        )

        self.assertEqual(report["status"], "review")
        self.assertEqual(
            report["blocking_warnings"],
            ["visual_identity_mismatch", "visual_material_color_mixing"],
        )
        self.assertFalse(report["import_recommended"])

    def test_invalid_provider_response_degrades_without_raising(self):
        report = review_model_visual_quality(self.source, self.root, completion=lambda *_: "not json")

        self.assertEqual(report["status"], "unavailable")
        self.assertEqual(report["errors"], ["visual_review_unavailable"])
        self.assertTrue(report["import_recommended"])


if __name__ == "__main__":
    unittest.main()
