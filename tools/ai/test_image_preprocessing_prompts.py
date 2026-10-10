import hashlib
import json
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from PIL import Image, ImageDraw

from tools.ai import image_preprocessing_debug as debug
from tools.ai import image_preprocessing_prompts as rules
from tools.ai import openai_preprocessor as api


class ImagePreprocessingPromptTests(unittest.TestCase):
    def test_editing_one_subject_rule_does_not_change_other_routes(self):
        sources = {"animal": "rabbit", "organic": "bonsai", "portrait": "adult portrait"}
        before = {key: api.build_geometry_reference_prompt(value, "realistic") for key, value in sources.items()}
        with mock.patch.dict(rules.SUBJECT_RULES, {"animal": "ANIMAL_RULE_EDIT_SENTINEL"}):
            after = {key: api.build_geometry_reference_prompt(value, "realistic") for key, value in sources.items()}
        self.assertIn("ANIMAL_RULE_EDIT_SENTINEL", after["animal"])
        self.assertNotEqual(before["animal"], after["animal"])
        self.assertEqual(before["organic"], after["organic"])
        self.assertEqual(before["portrait"], after["portrait"])

    def test_portrait_surface_edit_respects_style_gate_and_nonportrait_route(self):
        animal = api.build_geometry_reference_prompt("rabbit", "realistic")
        with mock.patch.dict(rules.PORTRAIT_RULES, {"surface": "PORTRAIT_SURFACE_EDIT_SENTINEL"}):
            self.assertIn("PORTRAIT_SURFACE_EDIT_SENTINEL", api.build_geometry_reference_prompt("adult portrait", "realistic"))
            self.assertIn("PORTRAIT_SURFACE_EDIT_SENTINEL", api.build_geometry_reference_prompt("adult portrait", "portrait_sketch"))
            self.assertNotIn("PORTRAIT_SURFACE_EDIT_SENTINEL", api.build_geometry_reference_prompt("adult portrait", "sculpture"))
            self.assertEqual(animal, api.build_geometry_reference_prompt("rabbit", "realistic"))

    def test_feature_edit_only_applies_to_matching_subject_feature(self):
        with mock.patch.dict(rules.FEATURE_RULES, {"branching": "BRANCHING_RULE_EDIT_SENTINEL"}):
            self.assertIn("BRANCHING_RULE_EDIT_SENTINEL", api.build_geometry_reference_prompt("bonsai tree", "realistic"))
            self.assertNotIn("BRANCHING_RULE_EDIT_SENTINEL", api.build_geometry_reference_prompt("gear with tree engraving", "realistic"))
            self.assertNotIn("BRANCHING_RULE_EDIT_SENTINEL", api.build_geometry_reference_prompt("rabbit", "realistic"))

    def test_layers_identify_the_actual_style_support_material_and_text_entry(self):
        layers = api.build_geometry_reference_prompt_layers("logo badge", "ink_relief")
        selected = {layer["id"]: layer["rule"] for layer in layers}
        self.assertEqual(selected["style.profile"], "STYLE_PROFILES.ink_relief")
        self.assertEqual(selected["style.support"], "SUPPORT_RULES.relief")
        self.assertEqual(selected["material.reference"], "MATERIAL_RULES.nonportrait_ink_relief")
        self.assertEqual(selected["subject.text"], "TEXT_RULES.preserve")
        matte = api.build_geometry_reference_prompt_layers("glass vase", "realistic", preprocessing_options={"material_proxy": True})
        self.assertEqual(next(layer["rule"] for layer in matte if layer["id"] == "material.reference"), "MATERIAL_RULES.matte_proxy")

    def test_layer_composition_covers_routes_aliases_custom_and_portrait_crop(self):
        for subject in ("adult portrait", "rabbit", "castle", "umbrella", "bonsai", "logo", "scene", "smoke", "unspecified subject"):
            for style in (*api.STYLE_PROFILES, "custom"):
                with self.subTest(subject=subject, style=style):
                    custom = "hand-carved clay" if style == "custom" else ""
                    layers = api.build_geometry_reference_prompt_layers(subject, style, custom)
                    self.assertEqual("".join(layer["text"] for layer in layers), api.build_geometry_reference_prompt(subject, style, custom))
                    self.assertEqual(len({layer["id"] for layer in layers}), len(layers))
        portrait = {layer["id"]: layer for layer in api.build_geometry_reference_prompt_layers("adult portrait", "realistic")}
        self.assertEqual(portrait["portrait.source_crop"]["rule"], "PORTRAIT_RULES.source_crop")
        self.assertIn("do not invent", portrait["portrait.source_crop"]["text"])

    def test_print_scale_is_an_explicit_nonportrait_layer(self):
        settings = {"width_mm": 150, "nozzle_mm": .4, "line_width_mm": .4, "minimum_feature_mm": .8}
        layers = api.build_geometry_reference_prompt_layers("umbrella", "realistic", print_settings=settings)
        scale = next(layer for layer in layers if layer["id"] == "print.scale")
        self.assertEqual(scale["rule"], "PRINT_SCALE_TEMPLATE")
        self.assertIn("width is 150 mm", scale["text"])
        self.assertIn("not measured dimensions", scale["text"])
        portrait = api.build_geometry_reference_prompt_layers("adult portrait", "realistic", print_settings=settings)
        self.assertNotIn("print.scale", [layer["id"] for layer in portrait])

    def test_dry_run_exports_complete_trace_without_provider_calls(self):
        with tempfile.TemporaryDirectory() as temp:
            folder = Path(temp)
            image = Image.new("RGB", (160, 160), "gray")
            ImageDraw.Draw(image).ellipse((40, 20, 120, 140), fill="teal")
            source = folder/"umbrella.png"
            image.save(source)
            output = folder/"output"
            with mock.patch.object(debug, "preprocess_image", side_effect=AssertionError("Unexpected image request")), mock.patch.object(debug, "complete_vision_once", side_effect=AssertionError("Unexpected vision request")):
                self.assertEqual(debug.main(["--input", str(source), "--instruction", "umbrella", "--style", "realistic", "--output", str(output)]), 0)
            text = (output/"prompt.txt").read_text(encoding="utf-8")
            trace = json.loads((output/"prompt-layers.json").read_text(encoding="utf-8"))
            self.assertEqual(trace["prompt_sha256"], hashlib.sha256(text.encode("utf-8")).hexdigest())
            self.assertEqual("".join(layer["text"] for layer in trace["layers"]), text)
            for layer in trace["layers"]:
                self.assertEqual(text[layer["start_char"]:layer["end_char"]], layer["text"])
            self.assertTrue(trace["print_constraints_applied"])
            self.assertEqual(trace["layers"][-1]["end_char"], len(text))
            self.assertFalse(json.loads((output/"preprocessing.json").read_text(encoding="utf-8"))["submits_3d_task"])

    def test_selected_editable_rule_reaches_exactly_one_production_image_edit(self):
        with tempfile.TemporaryDirectory() as temp:
            folder=Path(temp)
            source=folder/"rabbit.png"
            Image.new("RGB", (160, 160), "teal").save(source)
            destination=folder/"prepared.png"
            calls=[]
            def edit(image_path, prompt, output_path, **kwargs):
                calls.append(prompt)
                output_path.write_bytes(source.read_bytes())
                return output_path
            with mock.patch.dict(rules.SUBJECT_RULES, {"animal": "PRODUCTION_ANIMAL_RULE_SENTINEL"}), mock.patch.object(api, "edit_image", side_effect=edit):
                api.preprocess_image(source, "rabbit", destination, (), "realistic")
                self.assertEqual(calls, [api.build_geometry_reference_prompt("rabbit", "realistic", preprocessing_options={"filename": "rabbit.png"})])
            self.assertIn("PRODUCTION_ANIMAL_RULE_SENTINEL", calls[0])

    def test_custom_style_debug_trace_and_production_edit_use_the_same_description(self):
        with tempfile.TemporaryDirectory() as temp:
            folder=Path(temp)
            source=folder/"bonsai.png"
            Image.new("RGB", (160,160), "teal").save(source)
            output=folder/"output"
            captured=[]
            def preprocess(image_path, instruction, destination, palette, style, **kwargs):
                captured.append((style, kwargs["custom_style"]))
                destination.write_bytes(source.read_bytes())
                return destination
            with mock.patch.object(debug, "preprocess_image", side_effect=preprocess):
                self.assertEqual(debug.main(["--input",str(source),"--instruction","bonsai","--style","custom","--custom-style","hand-carved matte clay","--output",str(output),"--generate"]),0)
            self.assertEqual(captured, [("custom", "hand-carved matte clay")])
            trace=json.loads((output/"prompt-layers.json").read_text(encoding="utf-8"))
            style=next(layer for layer in trace["layers"] if layer["id"]=="style.profile")
            self.assertEqual(style["rule"], "user.custom_style")
            self.assertIn("hand-carved matte clay",style["text"])
            self.assertEqual(json.loads((output/"preprocessing.json").read_text(encoding="utf-8"))["policy"]["custom_style"], "hand-carved matte clay")

    def test_invalid_custom_style_fails_before_image_edit(self):
        with tempfile.TemporaryDirectory() as temp:
            source=Path(temp)/"rabbit.png"
            Image.new("RGB",(160,160),"teal").save(source)
            output=Path(temp)/"output"
            with mock.patch.object(debug,"preprocess_image", side_effect=AssertionError("Unexpected image request")) as edit:
                with self.assertRaises(SystemExit):
                    debug.main(["--input",str(source),"--instruction","rabbit","--style","custom","--output",str(output),"--generate"])
            edit.assert_not_called()
            self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
