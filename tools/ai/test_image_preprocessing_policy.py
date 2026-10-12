import tempfile
import unittest
from io import BytesIO
from pathlib import Path
from unittest import mock

from PIL import Image

from tools.ai.image_preprocessing_policy import classify_reference_subject
from tools.ai.model_input_image_quality import recommend_printable_style
from tools.ai import openai_preprocessor as api


class ImagePreprocessingPolicyTests(unittest.TestCase):
    def test_missing_subjects_and_material_risks_share_style_routing(self):
        buffer = BytesIO()
        Image.new("RGB", (256, 256), "gray").save(buffer, format="PNG")
        cases = {
            "轮式挖掘机": "hard_surface", "雨伞": "hard_surface", "椅子": "hard_surface",
            "金毛犬": "animal", "golden-retriever": "animal", "玻璃花瓶": "hard_surface",
            "transparent cat": "animal", "透明玻璃和水花": "effects", "树枝盆景": "organic",
            "埃菲尔塔": "architecture", "logo": "flat_graphic", "多物体场景": "scene",
        }
        for instruction, category in cases.items():
            with self.subTest(instruction=instruction):
                self.assertEqual(classify_reference_subject(instruction)["subject"], category)
                self.assertEqual(recommend_printable_style(buffer.getvalue(), prompt=instruction)["subject"], category)
        self.assertEqual(classify_reference_subject("玻璃花瓶")["material_risks"], ["transparent"])

    def test_filename_is_fallback_and_latin_substrings_are_not_subjects(self):
        self.assertEqual(classify_reference_subject("保留主体", filename="excavator.png")["evidence"], "filename")
        self.assertEqual(classify_reference_subject("真实人像", filename="vase.png")["subject"], "portrait")
        self.assertEqual(classify_reference_subject("education bookmark") ["subject"], "unknown")
        self.assertEqual(classify_reference_subject("保留猫，移除背景树")["subject"], "animal")
        self.assertEqual(classify_reference_subject("remove background trees, keep a chair")["subject"], "hard_surface")
        self.assertEqual(classify_reference_subject("do not remove the logo")["subject"], "flat_graphic")
        self.assertEqual(classify_reference_subject("logo of a cat")["subject"], "flat_graphic")
        self.assertEqual(classify_reference_subject("camera with logo preserved")["subject"], "hard_surface")

    def test_selected_rules_preserve_parts_without_portrait_or_plant_rules(self):
        prompt = api.build_geometry_reference_prompt("轮式挖掘机", "realistic")
        self.assertIn("positive-volume overlap", prompt)
        self.assertIn("bucket/blade joint", prompt)
        self.assertIn("exact viewpoint", prompt)
        self.assertNotIn("IDENTITY-FIRST PORTRAIT LOCK", prompt)
        self.assertNotIn("leaf/crown clusters", prompt)
        self.assertNotIn("locked facial", prompt)
        self.assertLess(len(prompt), 6500)
        plant = api.build_geometry_reference_prompt("树枝盆景", "cartoon")
        self.assertIn("major branch topology", plant)
        self.assertNotIn("axles", plant)
        umbrella = api.build_geometry_reference_prompt("雨伞", "realistic")
        self.assertIn("visible finite thickness", umbrella)
        self.assertNotIn("bucket/blade", umbrella)

    def test_subject_lettering_and_background_watermarks_are_distinct(self):
        for instruction, expected in (("logo", "preserve"), ("logo, remove background text", "preserve"),
                                      ("保留主体 logo，删除文字", "remove"),
                                      ("logo, do not remove lettering", "preserve")):
            with self.subTest(instruction=instruction):
                self.assertEqual(api.image_preprocessing_policy(instruction, "ink_relief")["subject_text"], expected)
        relief = api.build_geometry_reference_prompt("logo", "ink_relief")
        self.assertIn("spelling and negative spaces", relief)
        self.assertIn("backing plaque", relief)
        self.assertNotIn("remain base-free", relief)
        self.assertNotIn("natural colors, continuous gradients", relief)

    def test_print_parameters_are_real_validated_values_and_do_not_claim_measurement(self):
        settings = {"width_mm": 96, "nozzle_mm": 0.6, "line_width_mm": 0.7, "minimum_feature_mm": 2}
        prompt = api.build_geometry_reference_prompt("雨伞", "sculpture", print_settings=settings)
        self.assertIn("96 mm; nozzle 0.6 mm", prompt)
        self.assertIn("line width 0.7 mm", prompt)
        self.assertIn("minimum printable feature 2 mm", prompt)
        self.assertIn("not measured dimensions", prompt)
        self.assertIn("Preserve important holes and gaps", prompt)
        self.assertNotIn("Preserve natural colors", prompt)

    def test_material_proxy_is_explicit_and_portrait_unknown_retain_identity_contract(self):
        ordinary = api.build_geometry_reference_prompt("玻璃花瓶", "realistic")
        proxy = api.build_geometry_reference_prompt("玻璃花瓶", "realistic", preprocessing_options={"material_proxy": True})
        self.assertNotIn("EXPLICIT MATTE GEOMETRY MODE", ordinary)
        self.assertIn("EXPLICIT MATTE GEOMETRY MODE", proxy)
        for subject in ("真实人像", "保留主体"):
            with self.subTest(subject=subject):
                prompt = api.build_geometry_reference_prompt(subject, "realistic")
                self.assertIn("IDENTITY-FIRST PORTRAIT LOCK", prompt)
                self.assertFalse(api.image_preprocessing_policy(subject, "realistic")["specialized"])
                with self.assertRaises(api.OpenAIPreprocessorError):
                    api.build_geometry_reference_prompt(subject, "realistic", preprocessing_options={"material_proxy": True})

    def test_invalid_policy_fails_before_any_provider_call(self):
        cases = ({"preprocessing_options": "bad"}, {"preprocessing_options": {"material_proxy": "yes"}},
                 {"preprocessing_options": {"category_hint": "invalid"}}, {"print_settings": {"width_mm": float("nan")}},
                 {"print_settings": {"line_width_mm": 0.6, "minimum_feature_mm": 0.3}})
        with mock.patch.object(api, "edit_image") as edit:
            for options in cases:
                with self.subTest(options=options), self.assertRaises((ValueError, RuntimeError)):
                    api.preprocess_image("input.png", "雨伞", "prepared.png", (), **options)
            edit.assert_not_called()

    def test_exactly_one_edit_preserves_provider_bytes_and_both_prompt_boundaries(self):
        with tempfile.TemporaryDirectory() as directory:
            source, prepared, geometry = (Path(directory)/name for name in ("excavator.png", "prepared.png", "geometry.png"))
            Image.new("RGB", (256, 256), "gray").save(source)
            payload = source.read_bytes()
            def generate(_source, _prompt, destination, **_kwargs):
                Path(destination).write_bytes(payload)
                return Path(destination)
            settings = {"width_mm": 120}
            with mock.patch.object(api, "edit_image", side_effect=generate) as edit, mock.patch.object(api, "_portrait_face_lock_mask") as face:
                api.preprocess_image(source, "轮式挖掘机", prepared, (), "realistic", geometry_output_path=geometry,
                                     print_settings=settings)
            edit.assert_called_once()
            face.assert_not_called()
            self.assertEqual(edit.call_args.kwargs, {"background": "opaque"})
            self.assertEqual(prepared.read_bytes(), payload)
            self.assertEqual(geometry.read_bytes(), payload)
            expected = api.build_geometry_reference_prompt("轮式挖掘机", "realistic", print_settings=settings,
                                                         preprocessing_options={"filename": source.name})
            self.assertEqual(edit.call_args.args[1], expected)
            self.assertEqual(api.build_style_preview_prompt("轮式挖掘机", (), "realistic", print_settings=settings,
                                                           preprocessing_options={"filename": source.name}), expected)

    def test_opt_in_vision_does_not_use_retry_transport(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)/"source.png"
            Image.new("RGB", (64, 64), "red").save(path)
            with mock.patch.object(api, "_config", return_value=("https://example.invalid", "key", "vision-model", "image-model")), \
                 mock.patch.object(api, "_provider_request") as retry, \
                 mock.patch.object(api, "_request_with_provider", side_effect=api.OpenAIPreprocessorError("ambiguous")) as request:
                with self.assertRaises(api.OpenAIPreprocessorError):
                    api.complete_vision_once("system", "compare", (path,))
                request.assert_called_once()
                retry.assert_not_called()


if __name__ == "__main__":
    unittest.main()
