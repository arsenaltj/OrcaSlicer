#!/usr/bin/env python3
import contextlib
import os
import json
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from PIL import Image

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
            "q_cartoon",
        )

        self.assertIn("make it cartoon style", prompt)
        self.assertIn("designer-ready style preview", prompt)
        self.assertIn("recognizable identity", prompt)
        self.assertIn("clean product-shot reference for image-to-3D", prompt)
        self.assertIn("person, animal, statue", prompt)
        self.assertIn("inherently stable manufactured object", prompt)
        self.assertIn("do not add a pedestal", prompt)
        self.assertIn("finished bust or half-body collectible", prompt)
        self.assertIn("do not invent a pelvis, legs, or feet", prompt)
        self.assertIn("isolate exactly one requested or dominant subject", prompt)
        self.assertIn("Remove scenery, floor shadows, text, logos, watermarks, camera UI", prompt)
        self.assertIn("roughly 92-percent identity-preserving and 8-percent playful", prompt)
        self.assertIn("Do not make an adult childlike", prompt)
        self.assertIn("do not enlarge the eyes", prompt)
        self.assertIn("do not invent a white muzzle", prompt)
        self.assertIn("Do not return the unchanged source", prompt)
        self.assertIn("#FF0000, #00FF00", prompt)
        self.assertIn("allowed printable palette", prompt)
        self.assertIn("premium designer-toy collectible", prompt)
        self.assertIn("full-color designer toy", prompt)
        self.assertIn("Use at least 2 listed colors", prompt)
        self.assertIn("Cover at least 65 percent of the visible subject", prompt)
        self.assertIn("Never retain natural flesh tones", prompt)
        self.assertIn("outer silhouette, neck, limbs", prompt)
        self.assertIn("visibly joined by opaque palette-colored geometry", prompt)
        self.assertIn("Do not turn the chosen person, animal, statue, building, or object into a different subject", prompt)
        self.assertNotIn("Preserve the exact canvas, aspect ratio, crop, framing", prompt)
        self.assertNotIn("one fused connected object", prompt)

    def test_each_style_has_distinct_printable_direction(self):
        q_prompt = preprocessor._style_preview_prompt("preserve pose", ("#FFFFFF", "#000000"), "q_cartoon")
        low_poly_prompt = preprocessor._style_preview_prompt("preserve pose", ("#FFFFFF", "#000000"), "low_poly")
        sculpture_prompt = preprocessor._style_preview_prompt("preserve pose", ("#FFFFFF", "#000000"), "sculpture")
        cel_prompt = preprocessor._style_preview_prompt("preserve pose", ("#FFFFFF", "#000000"), "cel_shaded")
        enamel_prompt = preprocessor._style_preview_prompt("preserve pose", ("#FFFFFF", "#000000"), "enamel_inlay")

        self.assertIn("92-percent identity-preserving", q_prompt)
        self.assertIn("large intentional polygon facets", low_poly_prompt)
        self.assertIn("marble or plaster sculpture", sculpture_prompt)
        self.assertIn("two or three broad tone bands", cel_prompt)
        self.assertIn("shallow raised separators", enamel_prompt)
        self.assertIn("allowed printable palette", low_poly_prompt)
        self.assertIn("full-color designer toy", low_poly_prompt)
        self.assertIn("integrated round or softly polygonal display base", low_poly_prompt)
        self.assertIn("do not add a pedestal", low_poly_prompt)
        self.assertNotIn("otherwise do not invent one", sculpture_prompt)
        self.assertNotIn("Merge the torso", sculpture_prompt)
        self.assertNotIn("Give every listed color", low_poly_prompt)

    def test_natural_color_mode_omits_printable_palette_constraint(self):
        prompt = preprocessor._style_preview_prompt("preserve pose", (), "q_cartoon")

        self.assertIn("coherent natural colors", prompt)
        self.assertNotIn("allowed printable palette", prompt)
        self.assertNotIn("deterministic print-mapping", prompt)
        self.assertNotIn("full-color designer toy", prompt)

    def test_unknown_style_is_rejected(self):
        with self.assertRaises(preprocessor.OpenAIPreprocessorError):
            preprocessor._style_preview_prompt("preserve pose", ("#FFFFFF",), "unknown")

    def test_custom_style_is_wrapped_without_overriding_print_constraints(self):
        prompt = preprocessor._style_preview_prompt(
            "preserve pose",
            ("#FF0000", "#FFFFFF"),
            "custom",
            custom_style="复古木刻版画，粗轮廓和大块阴影",
        )

        self.assertIn("复古木刻版画，粗轮廓和大块阴影", prompt)
        self.assertIn("appearance and shape-language direction", prompt)
        self.assertIn("hard constraints in this request take priority", prompt)
        self.assertIn("allowed printable palette", prompt)

    def test_custom_style_requires_nonempty_bounded_description(self):
        with self.assertRaisesRegex(preprocessor.OpenAIPreprocessorError, "required"):
            preprocessor._text_image_prompt("a toy", (), "custom", custom_style="  ")
        with self.assertRaisesRegex(preprocessor.OpenAIPreprocessorError, "1000-byte"):
            preprocessor._text_image_prompt("a toy", (), "custom", custom_style="a" * 1001)


class TextImagePromptTests(unittest.TestCase):
    def test_printable_text_image_prompt_uses_large_regions_and_rejects_dithering(self):
        prompt = preprocessor._text_image_prompt(
            "一只正在奔跑的机械麒麟",
            ("#D93632", "#3B8C54", "#315CA8", "#F2F1EA"),
            "q_cartoon",
        )

        self.assertIn("一只正在奔跑的机械麒麟", prompt)
        self.assertIn("#D93632, #3B8C54, #315CA8, #F2F1EA", prompt)
        self.assertIn("large closed color regions", prompt)
        self.assertIn("Do not use gradients", prompt)
        self.assertIn("dithering", prompt)
        self.assertIn("deterministic print pipeline", prompt)
        self.assertIn("structure=#315CA8", prompt)
        self.assertIn("primary=#D93632", prompt)
        self.assertIn("full-color designer toy", prompt)
        self.assertIn("Use at least 3 listed colors", prompt)
        self.assertIn("transparent background", prompt)
        self.assertIn("load-bearing connections, base contact", prompt)
        self.assertIn("unless that exact shade is one of the listed printable colors", prompt)

    def test_text_image_prompt_omits_palette_language_in_natural_mode(self):
        prompt = preprocessor._text_image_prompt("a toy dragon", (), "low_poly")
        self.assertIn("coherent natural colors", prompt)
        self.assertNotIn("physical filament palette", prompt)


class VisionCompletionTests(unittest.TestCase):
    def test_sends_text_and_inline_png_to_compatible_chat_endpoint(self):
        with tempfile.TemporaryDirectory() as directory:
            image_path = Path(directory) / "sheet.png"
            Image.new("RGB", (8, 8), "red").save(image_path)
            captured = {}

            def provider(path, body, content_type):
                captured.update(path=path, payload=json.loads(body), content_type=content_type)
                return {"choices": [{"message": {"content": '{"score":88}'}}]}

            with configured_base_url("https://laotie.dev"), mock.patch.object(preprocessor, "_provider_request", side_effect=provider):
                response = preprocessor.complete_vision("review system", "review this model", (image_path,))

        self.assertEqual(response, '{"score":88}')
        self.assertEqual(captured["path"], "/chat/completions")
        parts = captured["payload"]["messages"][1]["content"]
        self.assertEqual(parts[0], {"type": "text", "text": "review this model"})
        self.assertTrue(parts[1]["image_url"]["url"].startswith("data:image/png;base64,"))

    def test_rejects_more_than_two_review_images(self):
        path = Path("unused.png")
        with self.assertRaises(preprocessor.OpenAIPreprocessorError):
            preprocessor.complete_vision("system", "user", (path, path, path))


class ExactImageEditTests(unittest.TestCase):
    def test_exact_prompt_is_sent_without_style_wrapper(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "source.png"
            destination = Path(directory) / "result.png"
            Image.new("RGB", (8, 8), "red").save(source)
            captured = {}

            def provider(path, body, content_type):
                captured.update(path=path, body=body, content_type=content_type)
                encoded = __import__("base64").b64encode(source.read_bytes()).decode("ascii")
                return {"data": [{"b64_json": encoded}]}

            with configured_base_url("https://laotie.dev"), mock.patch.object(preprocessor, "_provider_request", side_effect=provider):
                preprocessor.edit_image(source, "EXACT MULTIVIEW PROMPT", destination)

            self.assertTrue(destination.is_file())

        self.assertEqual(captured["path"], "/images/edits")
        self.assertIn(b"EXACT MULTIVIEW PROMPT", captured["body"])
        self.assertNotIn(b"designer-ready style preview", captured["body"])


if __name__ == "__main__":
    unittest.main()
