import json
import tempfile
import unittest
from pathlib import Path

from PIL import Image

from tools.ai import run_tripo_quality_report as report


class TripoQualityReportTests(unittest.TestCase):
    def test_overview_uses_each_frozen_view_sheet(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            previous_root = report.REPOSITORY_ROOT
            report.REPOSITORY_ROOT = root
            self.addCleanup(setattr, report, "REPOSITORY_ROOT", previous_root)
            rows = []
            for index in range(2):
                sheet = root / f"sheet-{index}.png"
                Image.new("RGB", (300, 200), (220 - index * 20, 230, 235)).save(sheet)
                rows.append({
                    "id": f"case-{index}",
                    "profile": "quality",
                    "face_count": 100000,
                    "quality_status": "review",
                    "view_sheet": sheet.name,
                })

            output = report.write_overview(rows, root / "overview.png")

            with Image.open(output) as overview:
                self.assertEqual(overview.size, (680, 250))

    def test_manifest_requires_exact_unique_approved_cases(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "manifest.json"
            path.write_text(json.dumps({
                "expected_paid_generation_tasks": 2,
                "cases": [
                    {"id": "a", "sha256": "A" * 64, "palette": [], "manual_approved": True},
                    {"id": "b", "sha256": "B" * 64, "palette": ["#FFFFFF"], "manual_approved": True},
                ],
            }), encoding="utf-8")
            cases = report.load_manifest(path)
            self.assertEqual([case["id"] for case in cases], ["a", "b"])

            value = json.loads(path.read_text(encoding="utf-8"))
            value["cases"][1]["sha256"] = "A" * 64
            path.write_text(json.dumps(value), encoding="utf-8")
            with self.assertRaises(report.TripoQualityReportError):
                report.load_manifest(path)

    def test_root_states_match_only_one_level_and_reject_duplicate_hashes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            first = root / "first"
            first.mkdir()
            (first / "validation-state.json").write_text(json.dumps({"input_sha256": "ABC"}), encoding="utf-8")
            nested = first / "task"
            nested.mkdir()
            (nested / "validation-state.json").write_text(json.dumps({"input_sha256": "ABC"}), encoding="utf-8")
            states = report._root_states(root)
            self.assertEqual(set(states), {"ABC"})

            second = root / "second"
            second.mkdir()
            (second / "validation-state.json").write_text(json.dumps({"input_sha256": "ABC"}), encoding="utf-8")
            with self.assertRaises(report.TripoQualityReportError):
                report._root_states(root)


if __name__ == "__main__":
    unittest.main()
