#!/usr/bin/env python3
import contextlib
import os
import unittest

from tools.ai import openai_preprocessor as preprocessor


@contextlib.contextmanager
def configured_base_url(value):
    names = ("OPENAI_API_KEY", "OPENAI_BASE_URL")
    original = {name: os.environ.get(name) for name in names}
    try:
        os.environ["OPENAI_API_KEY"] = "test-key"
        os.environ["OPENAI_BASE_URL"] = value
        yield
    finally:
        for name, previous in original.items():
            if previous is None:
                os.environ.pop(name, None)
            else:
                os.environ[name] = previous


class OpenAIBaseUrlTests(unittest.TestCase):
    def configured_url(self, value):
        with configured_base_url(value):
            base, _, _, _ = preprocessor._config()
        return base

    def test_domain_root_uses_standard_v1_prefix(self):
        self.assertEqual(self.configured_url("https://laotie.dev"), "https://laotie.dev/v1")

    def test_domain_root_with_trailing_slash_uses_standard_v1_prefix(self):
        self.assertEqual(self.configured_url("https://laotie.dev/"), "https://laotie.dev/v1")

    def test_existing_v1_prefix_is_preserved(self):
        self.assertEqual(self.configured_url("https://laotie.dev/v1/"), "https://laotie.dev/v1")

    def test_custom_compatible_prefix_is_preserved(self):
        self.assertEqual(
            self.configured_url("https://gateway.example/openai/v1/"),
            "https://gateway.example/openai/v1",
        )

    def test_query_string_is_rejected(self):
        with configured_base_url("https://laotie.dev?token=unsafe"):
            with self.assertRaises(preprocessor.OpenAIPreprocessorError):
                preprocessor._config()


class StylePreviewPromptTests(unittest.TestCase):
    def test_short_user_instruction_is_wrapped_with_preview_constraints(self):
        prompt = preprocessor._style_preview_prompt(
            "  make it cartoon style  ",
            ("#FF0000", "#00FF00"),
        )

        self.assertIn("make it cartoon style", prompt)
        self.assertIn("clearly transformed", prompt)
        self.assertIn("recognizable identity", prompt)
        self.assertIn("sole authority for depicted content", prompt)
        self.assertIn("Preserve the exact canvas, aspect ratio, crop, framing", prompt)
        self.assertIn("Do not outpaint, extend the canvas", prompt)
        self.assertIn("reveal hidden or occluded regions", prompt)
        self.assertIn("reconstruct missing body parts or object regions", prompt)
        self.assertIn("must remain cut off at the same boundary", prompt)
        self.assertIn("anything occluded must remain occluded", prompt)
        self.assertIn("Do not add, remove, replace, or duplicate", prompt)
        self.assertIn("otherwise do not invent one", prompt)
        self.assertIn("directly visible counterpart in the source", prompt)
        self.assertIn("Do not return the unchanged source", prompt)
        self.assertIn("#FF0000, #00FF00", prompt)
        self.assertIn("allowed printable palette", prompt)
        self.assertIn("marble or plaster sculpture", prompt)
        self.assertNotIn("full subject visible", prompt)
        self.assertNotIn("uniform solid-color background", prompt)
        self.assertNotIn("one fused connected object", prompt)
        self.assertNotIn("stable flat base", prompt)

    def test_each_style_has_distinct_printable_direction(self):
        q_prompt = preprocessor._style_preview_prompt("preserve pose", ("#FFFFFF", "#000000"), "q_cartoon")
        low_poly_prompt = preprocessor._style_preview_prompt("preserve pose", ("#FFFFFF", "#000000"), "low_poly")
        sculpture_prompt = preprocessor._style_preview_prompt("preserve pose", ("#FFFFFF", "#000000"), "sculpture")

        self.assertIn("only visible regions", q_prompt)
        self.assertIn("large intentional polygon facets", low_poly_prompt)
        self.assertIn("marble or plaster sculpture", sculpture_prompt)
        self.assertIn("allowed printable palette", low_poly_prompt)
        self.assertIn("coherent subset", low_poly_prompt)
        self.assertIn("Do not add a base", low_poly_prompt)
        self.assertIn("otherwise do not invent one", sculpture_prompt)
        self.assertNotIn("integrated polygonal base", low_poly_prompt)
        self.assertNotIn("Merge the torso", sculpture_prompt)
        self.assertNotIn("Give every listed color", low_poly_prompt)

    def test_natural_color_mode_omits_printable_palette_constraint(self):
        prompt = preprocessor._style_preview_prompt("preserve pose", (), "q_cartoon")

        self.assertIn("coherent natural colors", prompt)
        self.assertNotIn("allowed printable palette", prompt)
        self.assertNotIn("deterministic print-mapping", prompt)

    def test_unknown_style_is_rejected(self):
        with self.assertRaises(preprocessor.OpenAIPreprocessorError):
            preprocessor._style_preview_prompt("preserve pose", ("#FFFFFF",), "unknown")


if __name__ == "__main__":
    unittest.main()
