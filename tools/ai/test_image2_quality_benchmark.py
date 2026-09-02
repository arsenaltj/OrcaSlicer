import json
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from PIL import Image

from tools.ai import image2_quality_benchmark as benchmark


class Image2QualityBenchmarkTests(unittest.TestCase):
    def test_read_json_retries_a_transient_windows_file_lock(self):
        with tempfile.TemporaryDirectory() as directory:
            target = Path(directory) / "state.json"
            target.write_text('{"status":"complete"}', encoding="utf-8")
            original_read_text = benchmark.Path.read_text
            attempts = 0

            def transient_lock(path, *args, **kwargs):
                nonlocal attempts
                if path == target:
                    attempts += 1
                    if attempts == 1:
                        raise PermissionError(13, "temporarily locked")
                return original_read_text(path, *args, **kwargs)

            with mock.patch.object(benchmark.Path, "read_text", autospec=True, side_effect=transient_lock), \
                    mock.patch.object(benchmark.time, "sleep"):
                value = benchmark._read_json(target)

        self.assertEqual(attempts, 2)
        self.assertEqual(value, {"status": "complete"})

    def test_atomic_json_retries_a_transient_windows_file_lock(self):
        with tempfile.TemporaryDirectory() as directory:
            target = Path(directory) / "state.json"
            original_replace = benchmark.os.replace
            attempts = 0

            def transient_lock(source, destination):
                nonlocal attempts
                attempts += 1
                if attempts == 1:
                    raise PermissionError(5, "temporarily locked")
                original_replace(source, destination)

            with mock.patch.object(benchmark.os, "replace", side_effect=transient_lock), \
                    mock.patch.object(benchmark.time, "sleep"):
                benchmark._atomic_json(target, {"status": "complete"})

            self.assertEqual(attempts, 2)
            self.assertEqual(benchmark._read_json(target), {"status": "complete"})
            self.assertFalse(target.with_name("state.json.part").exists())

    def manifest(self, root: Path) -> Path:
        source = root / "source.png"
        Image.new("RGB", (512, 512), (220, 80, 40)).save(source)
        path = root / "manifest.json"
        path.write_text(json.dumps({
            "custom_style": "minimal art-deco collectible",
            "palettes": {
                "warm": ["#C95B43", "#253B5E", "#F2E5C4", "#D6A72C"],
                "cool": ["#3B82C4", "#293241", "#E8F0F2", "#E76F51"],
            },
            "style_runs": {
                "sculpture": [{"palette": None, "repetitions": 4}],
                "realistic": [{"palette": "warm", "repetitions": 2}, {"palette": "cool", "repetitions": 2}],
                "cartoon": [{"palette": "warm", "repetitions": 2}, {"palette": "cool", "repetitions": 2}],
                "custom": [{"palette": "warm", "repetitions": 2}, {"palette": "cool", "repetitions": 2}],
            },
            "cases": [{
                "id": "subject",
                "source": "source.png",
                "instruction": "preserve the subject",
                "label": "有把手的测试物",
                "category": "product",
                "challenges": ["thin_parts", "negative_space"],
                "preserve": ["one handle", "two openings"],
                "community_use": "桌面收纳和实用配件",
                "source_page": "https://commons.wikimedia.org/wiki/File:Example.jpg",
                "license": "CC BY-SA 4.0",
                "license_url": "https://creativecommons.org/licenses/by-sa/4.0/",
                "attribution": "Example Author",
            }],
        }), encoding="utf-8")
        return path

    def test_manifest_expands_configured_candidates(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            candidates = benchmark.load_candidates(self.manifest(root), root)
        self.assertEqual(len(candidates), 16)
        self.assertEqual({candidate.style for candidate in candidates}, {"sculpture", "realistic", "cartoon", "custom"})
        self.assertEqual(sum(not candidate.palette for candidate in candidates), 4)
        self.assertEqual(candidates[0].case.category, "product")
        self.assertEqual(candidates[0].case.label, "有把手的测试物")
        self.assertEqual(candidates[0].case.challenges, ("thin_parts", "negative_space"))
        self.assertEqual(candidates[0].case.preserve, ("one handle", "two openings"))
        self.assertEqual(candidates[0].case.community_use, "桌面收纳和实用配件")
        self.assertEqual(candidates[0].case.license, "CC BY-SA 4.0")

    def test_manifest_supports_every_non_realistic_product_style(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest_path = self.manifest(root)
            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
            manifest.pop("custom_style")
            manifest["style_runs"] = {
                "sculpture": [{"palette": None, "repetitions": 1}],
                "cartoon": [{"palette": "warm", "repetitions": 1}],
                "low_poly": [{"palette": "warm", "repetitions": 1}],
                "relief": [{"palette": "warm", "repetitions": 1}],
                "diorama": [{"palette": "warm", "repetitions": 1}],
            }
            manifest_path.write_text(json.dumps(manifest), encoding="utf-8")

            candidates = benchmark.load_candidates(manifest_path, root)
            catalog = benchmark.write_validation_catalog(root / "output", candidates)
            markdown = (root / "output" / "validation-catalog.md").read_text(encoding="utf-8")

        self.assertEqual(
            [candidate.style for candidate in candidates],
            ["sculpture", "cartoon", "low_poly", "relief", "diorama"],
        )
        self.assertNotIn("realistic", catalog["styles"])
        self.assertIn("低多边形", markdown)
        self.assertIn("浮雕", markdown)
        self.assertIn("微缩场景", markdown)

    def test_manifest_accepts_explicit_palette_role_overrides(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest_path = self.manifest(root)
            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
            manifest["palette_roles"] = {
                "warm": {
                    "primary": "#F2E5C4",
                    "structure": "#253B5E",
                    "light": "#C95B43",
                    "accent": "#D6A72C",
                }
            }
            manifest_path.write_text(json.dumps(manifest), encoding="utf-8")

            candidates = benchmark.load_candidates(manifest_path, root)
            warm = next(candidate for candidate in candidates if candidate.palette_id == "warm")

        self.assertEqual(warm.palette_roles["primary"], "#F2E5C4")
        self.assertEqual(warm.palette_roles["structure"], "#253B5E")

    def test_manifest_rejects_invalid_palette_role_overrides(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest_path = self.manifest(root)
            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
            manifest["palette_roles"] = {"warm": {"primary": "#000000"}}
            manifest_path.write_text(json.dumps(manifest), encoding="utf-8")

            with self.assertRaisesRegex(benchmark.Image2QualityBenchmarkError, "Invalid palette roles"):
                benchmark.load_candidates(manifest_path, root)

    def test_manifest_can_limit_run_to_three_core_styles_without_custom_description(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest_path = self.manifest(root)
            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
            manifest.pop("custom_style")
            manifest["style_runs"].pop("custom")
            manifest_path.write_text(json.dumps(manifest), encoding="utf-8")

            candidates = benchmark.load_candidates(manifest_path, root)
            report = benchmark.build_dry_run_report(root / "output", candidates, candidates)

        self.assertEqual(len(candidates), 12)
        self.assertEqual({candidate.style for candidate in candidates}, {"sculpture", "realistic", "cartoon"})
        self.assertEqual(set(report["by_style"]), {"sculpture", "realistic", "cartoon"})

    def test_text_case_uses_generation_runner_without_a_source_image(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest_path = self.manifest(root)
            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
            manifest["style_runs"] = {"cartoon": [{"palette": "warm", "repetitions": 1}]}
            manifest["cases"] = [{
                "id": "group",
                "input_mode": "text",
                "instruction": "two fictional adult coworkers on one shared base",
            }]
            manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
            candidate = benchmark.load_candidates(manifest_path, root)[0]
            output = root / "output"
            image_calls = []
            text_calls = []

            def image_provider(*args, **kwargs):
                image_calls.append((args, kwargs))
                raise AssertionError("image editing must not be used")

            def text_provider(instruction, destination, palette, style, **kwargs):
                text_calls.append((instruction, palette, style, kwargs))
                image = Image.new("RGBA", (512, 512), (255, 255, 255, 0))
                for x in range(96, 416):
                    for y in range(80, 448):
                        image.putpixel((x, y), (220, 180, 60, 255))
                image.save(destination)
                return destination

            state = benchmark.run_candidate(
                candidate,
                output,
                image_runner=image_provider,
                text_image_runner=text_provider,
            )

        self.assertEqual(candidate.case.input_mode, "text")
        self.assertIsNone(candidate.case.source)
        self.assertEqual(image_calls, [])
        self.assertEqual(len(text_calls), 1)
        self.assertEqual(state["input_mode"], "text")
        self.assertEqual(state["source_sha256"], "")
        self.assertEqual(state["paid_calls"]["image2"], 1)

    def test_manifest_rejects_unknown_style_and_invalid_case_metadata(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest_path = self.manifest(root)
            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
            manifest["style_runs"]["anime"] = [{"palette": "warm", "repetitions": 1}]
            manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
            with self.assertRaisesRegex(benchmark.Image2QualityBenchmarkError, "unknown public styles"):
                benchmark.load_candidates(manifest_path, root)

            manifest["style_runs"].pop("anime")
            manifest["cases"][0]["category"] = "Human Portrait"
            manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
            with self.assertRaisesRegex(benchmark.Image2QualityBenchmarkError, "category"):
                benchmark.load_candidates(manifest_path, root)

    def test_successful_candidate_is_reused_without_a_second_paid_call(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            candidate = next(item for item in benchmark.load_candidates(self.manifest(root), root) if item.palette)
            output = root / "output"
            calls = []

            def provider(source, instruction, destination, palette, style, **kwargs):
                calls.append((source, instruction, palette, style, kwargs))
                image = Image.new("RGB", (512, 512), (40, 100, 180))
                for x in range(128, 384):
                    for y in range(128, 384):
                        image.putpixel((x, y), (220, 180, 60))
                image.save(destination)
                return destination

            first = benchmark.run_candidate(candidate, output, image_runner=provider)
            second = benchmark.run_candidate(candidate, output, image_runner=provider)

        self.assertEqual(first["status"], "complete")
        self.assertEqual(second["status"], "complete")
        self.assertTrue(second["resumed"])
        self.assertEqual(len(calls), 1)
        self.assertEqual(first["paid_calls"]["image2"], 1)

    def test_local_reprocess_rebuilds_reference_without_provider_call(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            candidate = next(item for item in benchmark.load_candidates(self.manifest(root), root) if item.palette)
            output = root / "output"
            calls = []

            def provider(source, instruction, destination, palette, style, **kwargs):
                calls.append(1)
                image = Image.new("RGBA", (512, 512), (255, 255, 255, 0))
                image.paste((220, 60, 40, 255), (80, 80, 432, 432))
                image.save(destination)
                return destination

            first = benchmark.run_candidate(candidate, output, image_runner=provider)
            reference = output / "candidates" / candidate.candidate_id / first["printable"]["model_reference"]
            first_hash = first["printable"]["model_reference_sha256"]
            reference.unlink()
            rebuilt = benchmark.run_candidate(
                candidate,
                output,
                image_runner=provider,
                reprocess_local=True,
            )
            self.assertTrue(reference.is_file())
            self.assertEqual(rebuilt["printable"]["model_reference_sha256"], first_hash)

        self.assertEqual(len(calls), 1)
        self.assertEqual(rebuilt["paid_calls"]["image2"], 1)

    def test_model_input_quality_includes_printable_pipeline_warnings(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            reference = root / "reference.png"
            image = Image.new("RGBA", (512, 512), (220, 180, 60, 255))
            image.paste((40, 70, 100, 255), (128, 128, 384, 384))
            image.save(reference)
            state = {
                "output": {"quality": {"score": 100.0, "model_input_eligible": True, "blockers": [], "warnings": []}},
                "printable": {
                    "model_reference": "reference.png",
                    "metrics": {
                        "palette_quality_ok": True,
                        "quality_warnings": ["palette_material_is_fragmented"],
                    },
                },
            }

            benchmark._set_model_input_quality(root, state)

        self.assertEqual(state["quality"]["score"], 92.0)
        self.assertEqual(state["quality"]["warnings"], ["palette_material_is_fragmented"])

    def test_failed_paid_call_is_not_repeated(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            candidate = benchmark.load_candidates(self.manifest(root), root)[0]
            output = root / "output"
            calls = []

            def provider(*args, **kwargs):
                calls.append(1)
                raise RuntimeError("provider failed")

            with self.assertRaises(RuntimeError):
                benchmark.run_candidate(candidate, output, image_runner=provider)
            with self.assertRaises(benchmark.Image2QualityBenchmarkError):
                benchmark.run_candidate(candidate, output, image_runner=provider)
            state = json.loads(
                (output / "candidates" / candidate.candidate_id / "state.json").read_text(encoding="utf-8")
            )

        self.assertEqual(len(calls), 1)
        self.assertEqual(state["paid_calls"]["image2"], 1)

    def test_dry_run_separates_planned_reusable_and_blocked_candidates(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            candidates = benchmark.load_candidates(self.manifest(root), root)
            output = root / "output"

            initial = benchmark.build_dry_run_report(output, candidates, candidates[:2])

            def provider(source, instruction, destination, palette, style, **kwargs):
                del source, instruction, palette, style, kwargs
                image = Image.new("RGBA", (512, 512), (255, 255, 255, 0))
                for x in range(128, 384):
                    for y in range(96, 448):
                        image.putpixel((x, y), (220, 180, 60, 255))
                image.save(destination)
                return destination

            benchmark.run_candidate(candidates[0], output, image_runner=provider)

            def failed_provider(*args, **kwargs):
                del args, kwargs
                raise RuntimeError("provider failed")

            with self.assertRaises(RuntimeError):
                benchmark.run_candidate(candidates[1], output, image_runner=failed_provider)
            after = benchmark.build_dry_run_report(output, candidates, candidates[:2])
            resumable, skipped = benchmark.skip_blocked_candidates(output, candidates, candidates[:2])

        self.assertEqual(initial["planned_paid_calls"], 2)
        self.assertEqual(after["classifications"]["reusable"], 1)
        self.assertEqual(after["classifications"]["blocked_after_paid_attempt"], 1)
        self.assertEqual(after["planned_paid_calls"], 0)
        self.assertEqual([candidate.candidate_id for candidate in resumable], [candidates[0].candidate_id])
        self.assertEqual(skipped, [candidates[1].candidate_id])

    def test_summary_reports_model_input_quality(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            candidates = benchmark.load_candidates(self.manifest(root), root)
            output = root / "output"

            def provider(source, instruction, destination, palette, style, **kwargs):
                del source, instruction, palette, style, kwargs
                image = Image.new("RGBA", (512, 512), (255, 255, 255, 0))
                for x in range(128, 384):
                    for y in range(96, 448):
                        image.putpixel((x, y), (220, 180, 60, 255))
                image.save(destination)
                return destination

            state = benchmark.run_candidate(candidates[0], output, image_runner=provider)
            summary = benchmark.write_summary(output, candidates)

        self.assertTrue(state["quality"]["model_input_eligible"])
        self.assertEqual(summary["quality"]["assessed"], 1)
        self.assertEqual(summary["quality"]["eligible"], 1)
        self.assertEqual(summary["by_style"]["sculpture"]["quality_assessed"], 1)
        self.assertEqual(summary["by_category"]["product"]["quality_eligible"], 1)
        first_row = summary["rows"][0]
        self.assertEqual(first_row["challenges"], ["thin_parts", "negative_space"])
        self.assertEqual(first_row["preserve"], ["one handle", "two openings"])
        self.assertEqual(first_row["source_page"], "https://commons.wikimedia.org/wiki/File:Example.jpg")
        self.assertEqual(first_row["license"], "CC BY-SA 4.0")

    def test_validation_catalog_is_generated_from_case_metadata(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            candidates = benchmark.load_candidates(self.manifest(root), root)
            payload = benchmark.write_validation_catalog(root / "output", candidates)
            markdown = (root / "output" / "validation-catalog.md").read_text(encoding="utf-8")

        self.assertEqual(payload["case_count"], 1)
        self.assertEqual(payload["candidate_count"], 16)
        self.assertIn("有把手的测试物", markdown)
        self.assertIn("one handle", markdown)
        self.assertIn("单色雕塑", markdown)
        self.assertIn("CC BY-SA 4.0", markdown)
        self.assertIn("人物/人像必须使用一个低矮一体底座", markdown)
        self.assertIn("半身像不补造腿脚", markdown)
        self.assertIn("不能仅靠阴影接触或出现悬浮", markdown)

    def test_primary_and_palette_overview_sheets_follow_journey_layout(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest_path = self.manifest(root)
            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
            manifest.pop("custom_style")
            manifest["style_runs"] = {
                "sculpture": [{"palette": None, "repetitions": 1}],
                "realistic": [{"palette": "warm", "repetitions": 1}, {"palette": "cool", "repetitions": 1}],
                "cartoon": [{"palette": "warm", "repetitions": 1}, {"palette": "cool", "repetitions": 1}],
            }
            manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
            candidates = benchmark.load_candidates(manifest_path, root)
            output = root / "output"
            for index, candidate in enumerate(candidates):
                candidate_root = output / "candidates" / candidate.candidate_id
                printable_root = candidate_root / "printable"
                printable_root.mkdir(parents=True)
                Image.new("RGB", (512, 512), (40 + index * 20, 80, 120)).save(
                    candidate_root / benchmark.OUTPUT_FILENAME
                )
                Image.new("RGB", (512, 512), (120, 80, 40 + index * 20)).save(
                    printable_root / "model-reference.png"
                )
                (candidate_root / benchmark.STATE_FILENAME).write_text(json.dumps({
                    "status": "complete",
                    "printable": {"model_reference": "printable/model-reference.png"},
                }), encoding="utf-8")

            sheets = benchmark.create_journey_summary_sheets(output, candidates, cases_per_page=10)

            self.assertEqual(sheets, {"primary": 1, "palette": 1})
            with Image.open(output / "overview-sheets" / "primary" / "page-01.jpg") as primary:
                self.assertEqual(primary.width, 6 * 240)
            with Image.open(output / "overview-sheets" / "palette" / "page-01.jpg") as palette:
                self.assertEqual(palette.width, 5 * 240)

    def test_extended_style_overview_uses_one_column_per_active_style(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest_path = self.manifest(root)
            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
            manifest["style_runs"] = {
                "sculpture": [{"palette": None, "repetitions": 1}],
                "cartoon": [{"palette": "warm", "repetitions": 1}],
                "low_poly": [{"palette": "warm", "repetitions": 1}],
                "relief": [{"palette": "warm", "repetitions": 1}],
                "diorama": [{"palette": "warm", "repetitions": 1}],
            }
            manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
            candidates = benchmark.load_candidates(manifest_path, root)
            output = root / "output"

            sheets = benchmark.create_journey_summary_sheets(output, candidates, cases_per_page=10)

            self.assertEqual(sheets, {"primary": 1, "palette": 0})
            with Image.open(output / "overview-sheets" / "primary" / "page-01.jpg") as primary:
                self.assertEqual(primary.width, 6 * 240)

    def test_contact_sheet_renders_transparency_as_checkerboard(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "transparent.png"
            image = Image.new("RGBA", (32, 32), (245, 230, 190, 0))
            for x in range(8, 24):
                for y in range(8, 24):
                    image.putpixel((x, y), (220, 60, 40, 255))
            image.save(path)
            tile = benchmark._sheet_tile(path, "alpha")

        transparent_pixel = tile.getpixel((134, 134))
        subject_pixel = tile.getpixel((150, 150))
        self.assertIn(transparent_pixel[0], {226, 248})
        self.assertNotEqual(transparent_pixel, subject_pixel)


if __name__ == "__main__":
    unittest.main()
