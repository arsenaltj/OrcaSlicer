from __future__ import annotations

import json
from pathlib import Path
import tempfile
import unittest

from PIL import Image

from tools.ai import printable_multiview_reference as multiview


PALETTE = ("#D94F4F", "#222222", "#E8E2D0", "#3A7CA5")


class PrintableMultiviewReferenceTests(unittest.TestCase):
    def portrait_metrics(self, severe_roles=()):
        return {
            "portrait_skin_cleanup": {
                "activated": 1,
                "garment_color": PALETTE[2],
                "skin_color": PALETTE[0],
                "accent_color": PALETTE[3],
            },
            "meaningful_subject_color_count": 4,
            "palette_diversity_ok": True,
            "printable_subject_area_ratio": 0.22,
            "largest_subject_component_ratio": 0.991,
            "largest_detached_subject_diagonal_ratio": 0.081,
            "small_region_ratio_after": 0.0,
            "severe_fragmented_palette_roles": list(severe_roles),
            "secondary_subject_color_component_ratio": {
                PALETTE[2]: 0.03,
                PALETTE[0]: 0.02,
                PALETTE[3]: 0.005,
            },
        }

    @property
    def portrait_roles(self):
        return {
            "primary": PALETTE[2], "structure": PALETTE[1],
            "light": PALETTE[0], "accent": PALETTE[3],
        }

    def test_portrait_gate_allows_cleaned_primary_garment_split(self) -> None:
        report = multiview.evaluate_portrait_material_gate(
            self.portrait_metrics(("primary",)), self.portrait_roles,
        )
        self.assertEqual(report["status"], "pass")
        self.assertTrue(report["accepted_primary_fragmentation"])

    def test_portrait_gate_keeps_skin_and_accent_fragmentation_blocking(self) -> None:
        for role in ("light", "accent"):
            with self.subTest(role=role):
                report = multiview.evaluate_portrait_material_gate(
                    self.portrait_metrics((role,)), self.portrait_roles,
                )
                self.assertEqual(report["status"], "reject")
                self.assertIn("skin_or_accent_is_fragmented", report["reasons"])

    def test_portrait_gate_rejects_large_detached_structure(self) -> None:
        metrics = self.portrait_metrics()
        metrics["largest_detached_subject_diagonal_ratio"] = 0.14
        report = multiview.evaluate_portrait_material_gate(metrics, self.portrait_roles)
        self.assertEqual(report["status"], "reject")
        self.assertIn("large_detached_structure", report["reasons"])

    def test_portrait_gate_allows_safe_back_view_without_face_cleanup(self) -> None:
        metrics = self.portrait_metrics()
        metrics["portrait_skin_cleanup"]["activated"] = 0
        report = multiview.evaluate_portrait_material_gate(
            metrics, self.portrait_roles, view_name="back",
        )
        self.assertEqual(report["status"], "pass")
        self.assertTrue(report["accepted_back_without_face_cleanup"])

    def test_portrait_gate_allows_two_material_back_when_hidden_colors_are_absent(self) -> None:
        metrics = self.portrait_metrics()
        metrics["meaningful_subject_color_count"] = 2
        metrics["palette_diversity_ok"] = False
        report = multiview.evaluate_portrait_material_gate(
            metrics, self.portrait_roles, view_name="back",
        )
        self.assertEqual(report["status"], "pass")
        self.assertNotIn("insufficient_material_colors", report["reasons"])
        self.assertEqual(report["minimum_meaningful_colors"], 2)

    def test_portrait_gate_still_requires_cleanup_for_front_view(self) -> None:
        metrics = self.portrait_metrics()
        metrics["portrait_skin_cleanup"]["activated"] = 0
        for view in ("front", ""):
            with self.subTest(view=view):
                report = multiview.evaluate_portrait_material_gate(
                    metrics, self.portrait_roles, view_name=view,
                )
                self.assertEqual(report["status"], "reject")
                self.assertIn("portrait_cleanup_not_active", report["reasons"])

    def test_portrait_gate_defers_clean_profile_without_face_cleanup(self) -> None:
        metrics = self.portrait_metrics(("primary", "accent"))
        metrics["portrait_skin_cleanup"] = {"activated": 0}
        metrics["secondary_subject_color_component_ratio"][PALETTE[2]] = 0.08
        metrics["secondary_subject_color_component_ratio"][PALETTE[3]] = 0.04
        for view in ("left", "right"):
            with self.subTest(view=view):
                report = multiview.evaluate_portrait_material_gate(
                    metrics, self.portrait_roles, view_name=view,
                )
                self.assertEqual(report["status"], "review")
                self.assertTrue(report["requires_visual_material_review"])
                self.assertEqual(report["reasons"], [])

    def test_portrait_gate_rejects_unsafe_profile_fragmentation(self) -> None:
        metrics = self.portrait_metrics(("accent",))
        metrics["portrait_skin_cleanup"] = {"activated": 0}
        metrics["secondary_subject_color_component_ratio"][PALETTE[3]] = 0.09
        report = multiview.evaluate_portrait_material_gate(
            metrics, self.portrait_roles, view_name="right",
        )
        self.assertEqual(report["status"], "reject")
        self.assertIn("accent_material_is_fragmented", report["reasons"])

    def test_portrait_gate_does_not_hide_unsafe_back_view_materials(self) -> None:
        metrics = self.portrait_metrics(("light",))
        metrics["portrait_skin_cleanup"]["activated"] = 0
        report = multiview.evaluate_portrait_material_gate(
            metrics, self.portrait_roles, view_name="back",
        )
        self.assertEqual(report["status"], "reject")
        self.assertIn("skin_or_accent_is_fragmented", report["reasons"])
        self.assertFalse(report["accepted_back_without_face_cleanup"])

    def test_review_accepts_only_visual_left_right_naming_uncertainty(self) -> None:
        checks = {check: {"status": "pass", "reason": "ok"} for check in multiview.CHECK_IDS}
        checks["view_order"] = {
            "status": "review",
            "reason": "两个侧面明显相反，仅凭图像无法完全确认左/右命名。",
        }
        report = {
            "status": "review", "score": 91, "warnings": ["view_order"], "checks": checks,
        }
        acceptance = multiview.evaluate_multiview_review_acceptance(report)
        self.assertEqual(acceptance["status"], "pass")
        self.assertTrue(acceptance["accepted_view_order_ambiguity"])

    def test_review_rejects_an_actual_left_right_swap(self) -> None:
        checks = {check: {"status": "pass", "reason": "ok"} for check in multiview.CHECK_IDS}
        checks["view_order"] = {
            "status": "review",
            "reason": "左右视图顺序错误，两个面已调换。",
        }
        report = {
            "status": "review", "score": 92, "warnings": ["view_order"], "checks": checks,
        }
        self.assertEqual(multiview.evaluate_multiview_review_acceptance(report)["status"], "reject")

    def test_review_accepts_hidden_watch_visibility_uncertainty(self) -> None:
        checks = {check: {"status": "pass", "reason": "ok"} for check in multiview.CHECK_IDS}
        checks["view_order"] = {
            "status": "review",
            "reason": "两个侧面相反且一致，仅凭图像无法完全确认左右命名。",
        }
        checks["completeness"] = {
            "status": "review",
            "reason": "主体完整且未被明显裁切；背面因遮挡无法验证手表，侧面露出程度略有差异。",
        }
        report = {
            "status": "review", "score": 90,
            "warnings": ["view_order", "completeness"], "checks": checks,
        }
        acceptance = multiview.evaluate_multiview_review_acceptance(report)
        self.assertEqual(acceptance["status"], "pass")
        self.assertTrue(acceptance["accepted_view_order_ambiguity"])

    def test_review_rejects_actual_missing_or_cropped_view(self) -> None:
        checks = {check: {"status": "pass", "reason": "ok"} for check in multiview.CHECK_IDS}
        checks["completeness"] = {
            "status": "review",
            "reason": "缺少视图且主体明显裁切，无法验证手表。",
        }
        report = {
            "status": "review", "score": 92,
            "warnings": ["completeness"], "checks": checks,
        }
        self.assertEqual(multiview.evaluate_multiview_review_acceptance(report)["status"], "reject")

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

    def test_review_prompt_rejects_flat_portrait_backing(self) -> None:
        prompt = multiview._review_system_prompt()
        self.assertIn("person-shaped wall", prompt)
        self.assertIn("mark geometry and completeness as review", prompt)

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

    def test_chroma_cleanup_accepts_a_gradient_key_with_low_exact_dominance(self) -> None:
        source = Image.new("RGB", (64, 64))
        for x in range(source.width):
            red = (x % 16) * 8
            source.paste((red, 255, 255), (x, 0, x + 1, source.height))
        source.paste((137, 82, 61), (20, 12, 44, 56))
        cleaned, report = multiview._remove_provider_chroma_key(
            source, Image.new("L", source.size, 255), PALETTE
        )
        self.assertEqual(report["activated"], 1)
        self.assertGreater(report["removed_pixels"], 0)
        self.assertEqual(cleaned.getpixel((2, 2)), 0)
        self.assertEqual(cleaned.getpixel((32, 32)), 255)

    def test_chroma_cleanup_removes_non_key_segmentation_dust(self) -> None:
        source = Image.new("RGB", (64, 64), (0, 255, 255))
        source.paste((137, 82, 61), (16, 8, 48, 60))
        source.paste((160, 40, 180), (3, 3, 6, 6))
        mask = Image.new("L", source.size, 0)
        mask.paste(255, (16, 8, 48, 60))
        mask.paste(255, (3, 3, 6, 6))
        cleaned, report = multiview._remove_provider_chroma_key(source, mask, PALETTE)
        self.assertEqual(cleaned.getpixel((32, 32)), 255)
        self.assertEqual(cleaned.getpixel((4, 4)), 0)
        self.assertEqual(report["component_cleanup"]["removed_components"], 1)

    def test_component_cleanup_removes_large_thin_border_strip(self) -> None:
        mask = Image.new("L", (80, 80), 0)
        mask.paste(255, (20, 10, 60, 70))
        # This strip is larger than the generic dust threshold, but it touches
        # the complete frame height and cannot be part of a margin-safe model
        # reference. It previously shifted normalization and falsely rejected
        # an otherwise clean side portrait.
        mask.paste(255, (0, 0, 1, 80))

        cleaned, report = multiview._remove_small_mask_components(mask)

        self.assertEqual(cleaned.getpixel((0, 40)), 0)
        self.assertEqual(cleaned.getpixel((40, 40)), 255)
        self.assertEqual(report["removed_border_components"], 1)
        self.assertEqual(report["removed_border_pixels"], 80)
        self.assertEqual(report["largest_component_ratio_after"], 1.0)
        self.assertEqual(report["largest_detached_diagonal_ratio_after"], 0.0)
        self.assertAlmostEqual(report["subject_area_ratio_after"], 0.375, places=6)

    def test_processed_views_keep_raw_sculptural_shading_for_generation(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            crops = {}
            for index, view in enumerate(multiview.VIEW_ORDER):
                path = root / f"{view}.png"
                image = Image.new("RGB", (128, 128), (0, 255, 255))
                # This deliberately does not match the printable palette. The
                # generation reference must retain it as geometric shading.
                # A near-key antialiasing rim and detached key-colour speck must
                # be cleared before either reference is sent downstream.
                image.paste((72, 220, 220), (22, 18, 106, 118))
                image.paste((137 + index, 82, 61), (24, 20, 104, 116))
                image.paste((5, 250, 250), (6, 6, 9, 9))
                image.save(path)
                crops[view] = path

            preview, generation, metrics = multiview.process_multiview_crops(
                crops,
                root / "processed",
                PALETTE,
                palette_roles={
                    "primary": "#E8E2D0", "structure": "#222222",
                    "light": "#D94F4F", "accent": "#3A7CA5",
                },
            )

            self.assertEqual(set(preview), set(multiview.VIEW_ORDER))
            self.assertEqual(set(generation), set(multiview.VIEW_ORDER))
            self.assertTrue(all(metrics[view]["palette_quality_ok"] for view in multiview.VIEW_ORDER))
            metadata = json.loads(
                (preview["front"].parent / "metadata.json").read_text(encoding="utf-8")
            )
            self.assertEqual(metadata["palette_roles"]["color_by_role"]["primary"], "#E8E2D0")
            self.assertEqual(metadata["palette_roles"]["color_by_role"]["light"], "#D94F4F")
            with Image.open(generation["front"]) as opened:
                rgba = opened.convert("RGBA")
                self.assertEqual(rgba.getpixel((64, 64)), (137, 82, 61, 255))
                self.assertEqual(rgba.getpixel((4, 4))[3], 0)
                self.assertEqual(rgba.getpixel((22, 64))[3], 0)
                self.assertFalse(any(
                    pixel[3] > 0 and pixel[1] > 180 and pixel[2] > 180 and pixel[0] < 100
                    for pixel in rgba.getdata()
                ))
            with Image.open(preview["front"]) as opened:
                self.assertEqual(opened.convert("RGBA").getpixel((22, 64))[3], 0)
            self.assertGreater(
                metrics["front"]["generation_chroma_cleanup"]["removed_pixels"], 0
            )

    def test_normalization_locks_front_adds_equal_margin_and_clears_hidden_chroma(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            material = {}
            generation = {}
            for index, view in enumerate(multiview.VIEW_ORDER):
                generation_path = root / view / "generation_reference.png"
                material_path = root / view / "clean_preview.png"
                generation_path.parent.mkdir(parents=True)
                # Hidden cyan RGB deliberately remains under alpha=0, matching
                # the provider fallback that previously produced a base plate.
                image = Image.new("RGBA", (200, 200), (0, 255, 255, 0))
                top = 2 + index * 7
                image.paste((20, 80, 140, 255), (65, top, 135, 200 - index * 5))
                image.save(generation_path)
                Image.new("RGBA", (200, 200), (244, 244, 241, 0)).save(material_path)
                material_image = Image.open(material_path).convert("RGBA")
                material_image.paste((244, 244, 241, 255), (65, top, 135, 200 - index * 5))
                material_image.save(material_path)
                material_image.close()
                generation[view] = generation_path
                material[view] = material_path

            locked_generation = root / "locked-front-generation.png"
            locked_material = root / "locked-front-material.png"
            locked = Image.new("RGBA", (120, 300), (0, 0, 0, 0))
            locked.paste((210, 60, 50, 255), (20, 10, 100, 295))
            # Simulate the real portrait failure: light jacket pixels were
            # removed by the source foreground mask even though the reviewed
            # provider turntable has a complete silhouette.
            locked.paste((0, 0, 0, 0), (20, 120, 48, 220))
            locked.save(locked_generation)
            locked_material_image = Image.new("RGBA", (120, 300), (0, 0, 0, 0))
            locked_material_image.paste((217, 79, 79, 255), (20, 10, 100, 295))
            locked_material_image.save(locked_material)

            report = multiview.normalize_multiview_inputs(
                material,
                generation,
                locked_front_material=locked_material,
                locked_front_generation=locked_generation,
                target_canvas_size=multiview.HIGH_QUALITY_PORTRAIT_CANVAS_SIZE,
            )

            self.assertEqual(report["version"], "multiview-normalization-v3")
            self.assertEqual(report["canvas_size"], [1024, 1024])
            self.assertTrue(report["views"]["front"]["source_locked"])
            self.assertGreater(
                report["views"]["front"]["locked_composition"]["generation"]["provider_hole_fill_pixels"],
                0,
            )
            boxes = []
            for view in multiview.VIEW_ORDER:
                with Image.open(generation[view]) as opened:
                    rgba = opened.convert("RGBA")
                    self.assertEqual(rgba.size, (1024, 1024))
                    box = rgba.getchannel("A").getbbox()
                    self.assertIsNotNone(box)
                    assert box is not None
                    boxes.append(box)
                    self.assertGreater(box[1], 0)
                    self.assertLess(box[3], rgba.height)
                    self.assertEqual(rgba.getpixel((0, 0)), (0, 0, 0, 0))
            self.assertLessEqual(max(box[3] - box[1] for box in boxes) - min(box[3] - box[1] for box in boxes), 1)
            with Image.open(generation["front"]) as opened:
                opaque = [pixel for pixel in opened.convert("RGBA").getdata() if pixel[3] == 255]
                self.assertIn((210, 60, 50, 255), opaque)
                self.assertIn((20, 80, 140, 255), opaque)

            review_sheet = multiview.build_multiview_input_sheet(
                generation, root / "multiview-input-sheet.png"
            )
            with Image.open(review_sheet) as opened:
                self.assertEqual(opened.size, (2048, 2048))

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
