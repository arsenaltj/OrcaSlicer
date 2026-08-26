#!/usr/bin/env python3
import json
from pathlib import Path
import tempfile
import unittest

from PIL import Image

from tools.ai import printable_image_pipeline as pipeline


RGBW = ("#D93632", "#3B8C54", "#315CA8", "#F2F1EA")


class PrintSettingsTests(unittest.TestCase):
    def test_defaults_follow_two_line_minimum(self):
        settings = pipeline.PrintSettings.from_mapping(None)
        self.assertEqual(settings.minimum_feature_mm, 0.8)
        self.assertEqual(settings.line_width_mm, 0.4)
        self.assertEqual(settings.print_mode, "solid_regions")

    def test_rejects_non_solid_mode_and_too_small_feature(self):
        with self.assertRaises(pipeline.PrintableImageError):
            pipeline.PrintSettings.from_mapping({"print_mode": "halftone"})
        with self.assertRaises(pipeline.PrintableImageError):
            pipeline.PrintSettings.from_mapping({"line_width_mm": 0.5, "minimum_feature_mm": 0.4})


class PrintableImagePipelineTests(unittest.TestCase):
    def run_pipeline(self, image, **settings):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        source = root / "source.png"
        image.save(source)
        result = pipeline.process_printable_image(source, root / "result", RGBW, settings or None)
        return result

    def test_outputs_exact_palette_and_complete_exclusive_masks(self):
        image = Image.new("RGB", (80, 60), (245, 245, 238))
        for x in range(10, 70):
            for y in range(8, 52):
                image.putpixel((x, y), ((x * 3) % 255, (y * 5) % 255, ((x + y) * 2) % 255))

        result = self.run_pipeline(image)

        with Image.open(result.clean_preview).convert("RGB") as clean:
            self.assertLessEqual(set(clean.getdata()), {pipeline._hex_rgb(color) for color in RGBW})
        masks = [Image.open(path).convert("L") for path in result.masks.values()]
        try:
            for values in zip(*(mask.getdata() for mask in masks)):
                self.assertEqual(sum(value == 255 for value in values), 1)
        finally:
            for mask in masks:
                mask.close()
        self.assertEqual(set(result.masks), {"primary", "structure", "light", "accent"})
        self.assertTrue(result.background_mask.is_file())
        self.assertTrue(result.subject_mask.is_file())
        self.assertTrue(result.heatmap.is_file())
        self.assertTrue(result.model_reference.is_file())
        with Image.open(result.model_reference) as reference:
            self.assertEqual(reference.mode, "RGBA")
            self.assertEqual(reference.getpixel((0, 0))[3], 0)
        with Image.open(result.clean_preview) as clean:
            self.assertEqual(clean.mode, "RGBA")
            self.assertEqual(clean.getpixel((0, 0))[3], 0)

    def test_minimum_feature_merges_tiny_island_into_neighbor(self):
        image = Image.new("RGB", (100, 100), pipeline._hex_rgb(RGBW[3]))
        for x in range(20, 80):
            for y in range(20, 80):
                image.putpixel((x, y), pipeline._hex_rgb(RGBW[0]))
        for x in range(49, 52):
            for y in range(49, 52):
                image.putpixel((x, y), pipeline._hex_rgb(RGBW[2]))

        result = self.run_pipeline(image, width_mm=100, minimum_feature_mm=4.0)

        with Image.open(result.strict_preview) as strict, Image.open(result.clean_preview).convert("RGB") as clean:
            self.assertEqual(strict.getpixel((50, 50)), pipeline._hex_rgb(RGBW[2]))
            self.assertEqual(clean.getpixel((50, 50)), pipeline._hex_rgb(RGBW[0]))
        self.assertGreater(result.metrics["changed_pixel_ratio"], 0)
        self.assertEqual(result.metrics["minimum_feature_px"], 4)

    def test_physical_print_size_changes_cleanup_result(self):
        image = Image.new("RGB", (200, 100), pipeline._hex_rgb(RGBW[3]))
        for x in range(20, 180):
            for y in range(40, 43):
                image.putpixel((x, y), pipeline._hex_rgb(RGBW[1]))

        wide = self.run_pipeline(image, width_mm=400, minimum_feature_mm=0.8)
        narrow = self.run_pipeline(image, width_mm=40, minimum_feature_mm=0.8)

        self.assertLess(wide.metrics["minimum_feature_px"], narrow.metrics["minimum_feature_px"])
        self.assertLess(wide.metrics["changed_pixel_ratio"], narrow.metrics["changed_pixel_ratio"])

    def test_cleanup_removes_long_line_thinner_than_printable_feature(self):
        image = Image.new("RGB", (120, 100), pipeline._hex_rgb(RGBW[3]))
        for x in range(15, 105):
            for y in range(20, 80):
                image.putpixel((x, y), pipeline._hex_rgb(RGBW[2]))
        for x in range(20, 100):
            for y in range(49, 51):
                image.putpixel((x, y), pipeline._hex_rgb(RGBW[1]))

        result = self.run_pipeline(image, width_mm=24, minimum_feature_mm=1.2)

        with Image.open(result.strict_preview) as strict, Image.open(result.clean_preview).convert("RGB") as clean:
            self.assertEqual(strict.getpixel((60, 50)), pipeline._hex_rgb(RGBW[1]))
            self.assertEqual(clean.getpixel((60, 50)), pipeline._hex_rgb(RGBW[2]))

    def test_metadata_records_palette_print_settings_and_metrics(self):
        result = self.run_pipeline(Image.new("RGB", (40, 30), (220, 60, 55)), width_mm=120, nozzle_mm=0.6,
                                   line_width_mm=0.6, minimum_feature_mm=1.2)

        metadata = json.loads(result.metadata.read_text(encoding="utf-8"))
        self.assertEqual(metadata["schema_version"], 2)
        self.assertEqual(set(metadata["palette_roles"]["color_by_role"]), {"primary", "structure", "light", "accent"})
        self.assertEqual(metadata["outputs"]["model_reference"], "model_reference.png")
        self.assertEqual([entry["hex"] for entry in metadata["palette"]], list(RGBW))
        self.assertEqual(metadata["print"]["width_mm"], 120.0)
        self.assertEqual(metadata["metrics"]["minimum_feature_px"], result.metrics["minimum_feature_px"])
        self.assertEqual(metadata["outputs"]["masks"].keys(), result.masks.keys())

    def test_transparency_is_composited_to_brightest_filament(self):
        image = Image.new("RGBA", (30, 30), (0, 0, 0, 0))
        image.paste((210, 45, 45, 255), (10, 10, 20, 20))
        result = self.run_pipeline(image)
        with Image.open(result.clean_preview) as clean:
            self.assertEqual(clean.getpixel((0, 0))[3], 0)

    def test_light_gray_studio_background_maps_to_brightest_filament(self):
        image = Image.new("RGB", (100, 80), (155, 161, 167))
        for x, color in ((15, RGBW[0]), (40, RGBW[1]), (65, RGBW[2])):
            for px in range(x, x + 20):
                for py in range(15, 65):
                    image.putpixel((px, py), pipeline._hex_rgb(color))

        result = self.run_pipeline(image)

        with Image.open(result.clean_preview) as clean:
            self.assertEqual(clean.getpixel((0, 0))[3], 0)
            self.assertEqual(clean.getpixel((99, 79))[3], 0)

    def test_painted_checkerboard_does_not_swallow_white_filament_subject(self):
        image = Image.new("RGB", (160, 140), (255, 255, 255))
        for y in range(140):
            for x in range(160):
                shade = 246 if (x // 10 + y // 10) % 2 else 255
                image.putpixel((x, y), (shade, shade, shade))
        # A red outline separates a white printable face from the painted
        # checkerboard, matching the light-subject failure seen in Image2.
        for x in range(35, 125):
            for y in range(20, 120):
                image.putpixel((x, y), pipeline._hex_rgb(RGBW[0]))
        for x in range(43, 117):
            for y in range(28, 112):
                image.putpixel((x, y), pipeline._hex_rgb(RGBW[3]))
        for x in range(50, 75):
            for y in range(75, 105):
                image.putpixel((x, y), pipeline._hex_rgb(RGBW[1]))
        for x in range(85, 110):
            for y in range(75, 105):
                image.putpixel((x, y), pipeline._hex_rgb(RGBW[2]))

        result = self.run_pipeline(image)

        with Image.open(result.clean_preview) as clean:
            self.assertEqual(clean.getpixel((0, 0))[3], 0)
            self.assertEqual(clean.getpixel((80, 50)), pipeline._hex_rgb(RGBW[3]) + (255,))
        self.assertTrue(result.metrics["palette_quality_ok"])
        self.assertEqual(result.metrics["meaningful_subject_color_count"], 4)

    def test_quality_gate_allows_legitimate_single_color_subject_with_diversity_warning(self):
        image = Image.new("RGB", (120, 100), pipeline._hex_rgb(RGBW[3]))
        for x in range(25, 95):
            for y in range(20, 85):
                image.putpixel((x, y), pipeline._hex_rgb(RGBW[2]))

        result = self.run_pipeline(image)

        self.assertTrue(result.metrics["palette_quality_ok"])
        self.assertFalse(result.metrics["palette_diversity_ok"])
        # The white studio background is transparent in the 3D reference and no longer counts as a model color.
        self.assertEqual(result.metrics["meaningful_palette_count"], 1)
        self.assertIn("too_few_meaningful_palette_colors", result.metrics["quality_warnings"])

    def test_single_filament_preserves_subject_silhouette_and_uses_exactly_one_color(self):
        image = Image.new("RGB", (120, 100), (250, 250, 248))
        for x in range(20, 101):
            for y in range(15, 86):
                image.putpixel((x, y), (45 + x, 35 + y, 90))
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        source = root / "source.png"
        image.save(source)

        result = pipeline.process_printable_image(source, root / "result", ("#237B45",), None)

        with Image.open(result.clean_preview) as clean:
            alpha = clean.getchannel("A")
            self.assertEqual(alpha.getbbox(), (20, 15, 101, 86))
            opaque_colors = {pixel[:3] for pixel in clean.getdata() if pixel[3] == 255}
            self.assertEqual(opaque_colors, {(0x23, 0x7B, 0x45)})
        self.assertEqual(result.metrics["meaningful_subject_color_count"], 1)
        self.assertTrue(result.metrics["palette_quality_ok"])

    def test_quality_gate_accepts_three_meaningful_designer_toy_colors(self):
        image = Image.new("RGB", (120, 100), pipeline._hex_rgb(RGBW[3]))
        for x, color in ((20, RGBW[0]), (45, RGBW[1]), (70, RGBW[2])):
            for px in range(x, x + 25):
                for py in range(20, 85):
                    image.putpixel((px, py), pipeline._hex_rgb(color))

        result = self.run_pipeline(image)

        self.assertTrue(result.metrics["palette_quality_ok"])
        self.assertGreaterEqual(result.metrics["meaningful_palette_count"], 3)
        self.assertGreaterEqual(result.metrics["meaningful_subject_color_count"], 2)
        self.assertGreaterEqual(result.metrics["largest_subject_component_ratio"], 0.90)

    def test_quality_gate_rejects_subject_split_by_background_color(self):
        image = Image.new("RGB", (120, 120), pipeline._hex_rgb(RGBW[3]))
        for x in range(20, 100):
            for y in range(10, 55):
                image.putpixel((x, y), pipeline._hex_rgb(RGBW[0]))
        for x in range(45, 75):
            for y in range(20, 45):
                image.putpixel((x, y), pipeline._hex_rgb(RGBW[1]))
        for x in range(25, 95):
            for y in range(80, 110):
                image.putpixel((x, y), pipeline._hex_rgb(RGBW[2]))

        result = self.run_pipeline(image)

        self.assertFalse(result.metrics["palette_quality_ok"])
        self.assertEqual(result.metrics["meaningful_subject_component_count"], 2)
        self.assertLess(result.metrics["largest_subject_component_ratio"], 0.90)
        self.assertIn("printable_subject_is_disconnected", result.metrics["quality_warnings"])

    def test_quality_gate_rejects_background_color_that_swallows_subject(self):
        image = Image.new("RGB", (120, 100), pipeline._hex_rgb(RGBW[3]))
        for x, color, width in ((35, RGBW[0], 12), (47, RGBW[1], 8), (55, RGBW[2], 14)):
            for px in range(x, x + width):
                for py in range(25, 65):
                    image.putpixel((px, py), pipeline._hex_rgb(color))

        result = self.run_pipeline(image)

        # Only subject colors count; the boundary-connected white background is excluded.
        self.assertEqual(result.metrics["meaningful_palette_count"], 3)
        self.assertFalse(result.metrics["palette_quality_ok"])
        self.assertLess(result.metrics["printable_subject_area_ratio"], 0.18)
        self.assertIn("printable_subject_area_below_18_percent", result.metrics["quality_warnings"])

    def test_dominant_red_toy_keeps_smaller_green_and_blue_materials_separate(self):
        image = Image.new("RGB", (200, 120), (254, 254, 254))
        for x in range(20, 100):
            red = 180 + (x % 12) * 6
            for y in range(15, 110):
                image.putpixel((x, y), (red, 12 + y % 14, 12 + x % 9))
        for x in range(100, 125):
            for y in range(20, 100):
                image.putpixel((x, y), (5, 130 + y % 35, 45 + x % 20))
        for x in range(125, 150):
            for y in range(20, 100):
                image.putpixel((x, y), (5 + x % 15, 75 + y % 30, 175 + x % 45))

        result = self.run_pipeline(image)

        self.assertGreater(result.palette_usage[RGBW[0]], 0)
        self.assertGreater(result.palette_usage[RGBW[1]], 0)
        self.assertGreater(result.palette_usage[RGBW[2]], 0)


if __name__ == "__main__":
    unittest.main()
