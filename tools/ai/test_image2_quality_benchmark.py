import json
import tempfile
import unittest
from pathlib import Path

from PIL import Image

from tools.ai import image2_quality_benchmark as benchmark


class Image2QualityBenchmarkTests(unittest.TestCase):
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
            "cases": [{"id": "subject", "source": "source.png", "instruction": "preserve the subject"}],
        }), encoding="utf-8")
        return path

    def test_manifest_expands_four_candidates_per_style(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            candidates = benchmark.load_candidates(self.manifest(root), root)
        self.assertEqual(len(candidates), 16)
        self.assertEqual({candidate.style for candidate in candidates}, set(benchmark.PUBLIC_STYLES))
        self.assertEqual(sum(not candidate.palette for candidate in candidates), 4)

    def test_successful_candidate_is_reused_without_a_second_paid_call(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            candidate = benchmark.load_candidates(self.manifest(root), root)[0]
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
            state = json.loads((output / "candidates" / candidate.candidate_id / "state.json").read_text())

        self.assertEqual(len(calls), 1)
        self.assertEqual(state["paid_calls"]["image2"], 1)

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
