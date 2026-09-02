import json
import tempfile
import unittest
from pathlib import Path

from PIL import Image

from tools.ai import build_image2_resource_catalog as catalog


class BuildImage2ResourceCatalogTests(unittest.TestCase):
    def test_build_catalog_combines_runs_and_keeps_traceability(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            run = root / "run"
            candidate_id = "case_one__relief__warm__r01"
            candidate = run / "candidates" / candidate_id
            candidate.mkdir(parents=True)
            (candidate / "image2-output.png").write_bytes(b"raw")
            (candidate / "model-reference.png").write_bytes(b"model")
            (run / "contact-sheets" / "model-reference").mkdir(parents=True)
            (run / "contact-sheets" / "model-reference" / "case_one__relief.jpg").write_bytes(b"sheet")
            (run / "overview-sheets" / "primary").mkdir(parents=True)
            (run / "overview-sheets" / "primary" / "page-01.jpg").write_bytes(b"overview")
            summary = {
                "candidate_count": 1,
                "paid_image2_calls": 1,
                "paid_tripo_calls": 0,
                "statuses": {"complete": 1},
                "by_style": {"relief": {"total": 1}},
                "quality": {"assessed": 1},
                "rows": [{
                    "candidate_id": candidate_id,
                    "case_id": "case_one",
                    "label": "用例一",
                    "category": "product",
                    "input_mode": "image",
                    "source": "generated_models/source.jpg",
                    "source_page": "https://example.test/source",
                    "license": "CC0",
                    "license_url": "https://example.test/license",
                    "attribution": "Author",
                    "instruction": "Preserve one object.",
                    "style": "relief",
                    "palette_id": "warm",
                    "palette": ["#FFFFFF", "#000000"],
                    "status": "complete",
                    "error": "",
                    "paid_calls": {"image2": 1, "tripo": 0},
                    "provider_prompt_sha256": "abc",
                    "quality": {"score": 88, "model_input_eligible": True, "flags": ["review"]},
                    "printable": {"model_reference": "model-reference.png"},
                }],
            }
            (run / "benchmark-summary.json").write_text(json.dumps(summary), encoding="utf-8")

            value = catalog.build_catalog([("v1", run)])

            self.assertEqual(value["totals"]["unique_cases"], 1)
            self.assertEqual(value["totals"]["complete_images"], 1)
            self.assertEqual(value["totals"]["paid_tripo_calls"], 0)
            self.assertEqual(value["source_resources"][0]["license"], "CC0")
            image = value["image_resources"][0]
            self.assertEqual(image["quality_score"], 88)
            self.assertTrue(image["raw_image"].endswith("image2-output.png"))
            self.assertTrue(image["contact_sheet"].endswith("case_one__relief.jpg"))

    def test_write_catalog_creates_json_csv_tsv_and_readme(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            value = {
                "runs": [],
                "source_resources": [],
                "image_resources": [],
                "totals": {
                    "runs": 0,
                    "unique_cases": 0,
                    "licensed_image_sources": 0,
                    "text_cases": 0,
                    "image_candidates": 0,
                    "complete_images": 0,
                    "failed_images": 0,
                    "paid_image2_calls": 0,
                    "paid_tripo_calls": 0,
                    "styles": {},
                },
            }

            catalog.write_catalog(output, value)

            self.assertTrue((output / "resource-catalog.json").is_file())
            self.assertTrue((output / "source-resources.csv").is_file())
            self.assertTrue((output / "image-resources.csv").is_file())
            self.assertTrue((output / "feishu-image-resources.tsv").is_file())
            self.assertTrue((output / "feishu-summary.tsv").is_file())
            self.assertIn("Tripo 调用：0", (output / "README.md").read_text(encoding="utf-8"))

    def test_fix_comparison_sheet_uses_shared_cases_from_two_runs(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            source = output / "source.png"
            before_relief = output / "before-relief.png"
            after_relief = output / "after-relief.png"
            before_diorama = output / "before-diorama.png"
            after_diorama = output / "after-diorama.png"
            for index, path in enumerate((source, before_relief, after_relief, before_diorama, after_diorama)):
                Image.new("RGB", (32, 32), (index * 30, 80, 120)).save(path)
            relative = lambda path: catalog._relative(path)
            value = {
                "runs": [{"run": "v1"}, {"run": "v2"}],
                "source_resources": [{"case_id": "case_one", "source": relative(source)}],
                "image_resources": [
                    {"run": "v1", "case_id": "case_one", "style": "relief", "model_reference": relative(before_relief)},
                    {"run": "v2", "case_id": "case_one", "style": "relief", "model_reference": relative(after_relief)},
                    {"run": "v1", "case_id": "case_one", "style": "diorama", "model_reference": relative(before_diorama)},
                    {"run": "v2", "case_id": "case_one", "style": "diorama", "model_reference": relative(after_diorama)},
                ],
            }

            pages = catalog.write_fix_comparison_sheets(output, value)

            self.assertEqual(pages, 1)
            with Image.open(output / "comparison-sheets" / "page-01.jpg") as comparison:
                self.assertEqual(comparison.size, (1100, 288))


if __name__ == "__main__":
    unittest.main()
