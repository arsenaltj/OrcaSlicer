#!/usr/bin/env python3
import json
import tempfile
import unittest
from pathlib import Path
import sys

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
                "silhouette_readability", "color_region_clarity",
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

    def test_invalid_provider_response_degrades_without_raising(self):
        report = review_model_visual_quality(self.source, self.root, completion=lambda *_: "not json")

        self.assertEqual(report["status"], "unavailable")
        self.assertEqual(report["errors"], ["visual_review_unavailable"])


if __name__ == "__main__":
    unittest.main()
