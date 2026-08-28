#!/usr/bin/env python3
import contextlib
from io import BytesIO
import os
import json
import tempfile
import unittest
import urllib.error
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

    def test_image_quality_defaults_to_high_and_rejects_unknown_values(self):
        with mock.patch.dict(os.environ, {}, clear=False):
            os.environ.pop("OPENAI_IMAGE_QUALITY", None)
            self.assertEqual(preprocessor._image_quality(), "high")
        with mock.patch.dict(os.environ, {"OPENAI_IMAGE_QUALITY": "ultra"}):
            with self.assertRaises(preprocessor.OpenAIPreprocessorError):
                preprocessor._image_quality()

    def test_provider_errors_expose_safe_retry_and_ambiguity_semantics(self):
        cases = (
            (429, "image_rate_limited", True, False),
            (400, "image_rejected", False, False),
            (503, "image_service_unavailable", True, True),
        )
        for status, code, retryable, ambiguous in cases:
            opener = mock.Mock()
            opener.open.side_effect = urllib.error.HTTPError("https://example.invalid", status, "error", None, None)
            with (
                self.subTest(status=status),
                configured_base_url("https://laotie.dev"),
                mock.patch.object(preprocessor.urllib.request, "build_opener", return_value=opener),
                self.assertRaises(preprocessor.OpenAIPreprocessorError) as raised,
            ):
                preprocessor._provider_request("/images/generations", b"{}", "application/json")
            self.assertEqual(raised.exception.code, code)
            self.assertEqual(raised.exception.retryable, retryable)
            self.assertEqual(raised.exception.ambiguous, ambiguous)

    def test_connection_failure_is_ambiguous_and_not_retried(self):
        opener = mock.Mock()
        opener.open.side_effect = urllib.error.URLError("offline")
        with (
            configured_base_url("https://laotie.dev"),
            mock.patch.object(preprocessor.urllib.request, "build_opener", return_value=opener),
            self.assertRaises(preprocessor.OpenAIPreprocessorError) as raised,
        ):
            preprocessor._provider_request("/images/generations", b"{}", "application/json")

        self.assertEqual(raised.exception.code, "image_connection_failed")
        self.assertTrue(raised.exception.retryable)
        self.assertTrue(raised.exception.ambiguous)
        opener.open.assert_called_once()


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
        self.assertIn("non-human", prompt)
        self.assertIn("base-free when the source is base-free", prompt)
        self.assertIn("mandatory portrait base rule", prompt)
        self.assertIn("always add exactly one low, simple, integrated display base", prompt)
        self.assertIn("only category-specific exception", prompt)
        self.assertIn("flat underside", prompt)
        self.assertIn("shared low base", prompt)
        self.assertIn("smallest integrated material bridge", prompt)
        self.assertIn("finished bust or half-body collectible", prompt)
        self.assertIn("fused to the required compact portrait base", prompt)
        self.assertIn("do not invent a pelvis, legs, or feet", prompt)
        self.assertIn("Changing palette mode", prompt)
        self.assertIn("must not change full-body versus bust framing", prompt)
        self.assertIn("Center the exact requested subject or explicitly requested subject group", prompt)
        self.assertIn("preserve the exact requested count, identities, left-right order", prompt)
        self.assertIn("did not explicitly request a pair, group, set, or exact subject count", prompt)
        self.assertIn("isolate exactly one requested or dominant subject", prompt)
        self.assertIn("Remove scenery, floor shadows, text, logos, watermarks, camera UI", prompt)
        self.assertIn("friendly cute cartoon collectible", prompt)
        self.assertIn("Preserve recognizable identity, age, expression", prompt)
        self.assertIn("do not enlarge eyes excessively", prompt)
        self.assertIn("do not add or remove elements", prompt)
        self.assertIn("do not enlarge the eyes", prompt)
        self.assertIn("do not invent a white muzzle", prompt)
        self.assertIn("Do not return the unchanged source", prompt)
        self.assertIn("#FF0000, #00FF00", prompt)
        self.assertIn("allowed printable palette", prompt)
        self.assertIn("friendly cute cartoon collectible", prompt)
        self.assertIn("full-color designer toy", prompt)
        self.assertIn("Use at least 2 listed colors", prompt)
        self.assertIn("Cover at least 65 percent of the visible subject", prompt)
        self.assertIn("Never retain natural flesh tones", prompt)
        self.assertIn("outer silhouette, neck, limbs", prompt)
        self.assertIn("visibly joined by opaque palette-colored geometry", prompt)
        self.assertIn("clearly separated from every listed palette color", prompt)
        self.assertIn("identity-defining engraved lines", prompt)
        self.assertIn("will disappear during exact-palette mapping", prompt)
        self.assertIn("Do not turn the chosen person, animal, statue, building, or object into a different subject", prompt)
        self.assertNotIn("Preserve the exact canvas, aspect ratio, crop, framing", prompt)
        self.assertNotIn("one fused connected object", prompt)

    def test_each_style_has_distinct_printable_direction(self):
        realistic = preprocessor._style_preview_prompt("preserve pose", ("#FFFFFF", "#000000"), "realistic")
        cartoon = preprocessor._style_preview_prompt("preserve pose", ("#FFFFFF", "#000000"), "cartoon")
        sculpture = preprocessor._style_preview_prompt("preserve pose", (), "sculpture")

        self.assertIn("changing as little as possible", realistic)
        self.assertIn("friendly cute cartoon collectible", cartoon)
        self.assertIn("one monochrome museum-quality", sculpture)
        for prompt in (realistic, cartoon, sculpture):
            self.assertIn("Preserve the chosen subject's recognizable identity", prompt)
            self.assertIn("Do not turn the chosen", prompt)
            self.assertIn("closed visual inventory", prompt)
            self.assertIn("exact viewpoint", prompt)
            self.assertIn("eyewear, headwear, visible hands", prompt)
            self.assertIn("wheels, handles, openings, windows, lenses, dials, buttons", prompt)
            self.assertIn("preserve tier and opening counts", prompt)
            self.assertIn("thicken it subtly instead of deleting or duplicating it", prompt)
        self.assertIn("allowed printable palette", realistic)
        self.assertIn("full-color designer toy", realistic)
        self.assertNotIn("allowed printable palette", sculpture)
        self.assertIn("monochrome means one material, not fewer components", sculpture)
        self.assertIn("rather than changing identity", cartoon)

    def test_legacy_styles_resolve_to_the_new_four_style_profiles(self):
        self.assertEqual(
            preprocessor._style_profile("q_cartoon"),
            preprocessor._style_profile("cartoon"),
        )
        self.assertEqual(
            preprocessor._style_profile("low_poly"),
            preprocessor._style_profile("realistic"),
        )

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
        self.assertIn("always add exactly one low, simple, integrated display base", prompt)
        self.assertIn("Do not apply this portrait exception to an animal, product, vehicle", prompt)

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
        self.assertIn("always add exactly one low, simple, integrated display base", prompt)
        self.assertIn("head-and-shoulders, chest, waist, or other cropped human portrait", prompt)
        self.assertIn("Do not apply this portrait exception to an animal, product, vehicle", prompt)
        self.assertIn("unless that exact shade is one of the listed printable colors", prompt)
        self.assertIn("closed component inventory", prompt)
        self.assertIn("never as lighting highlights", prompt)

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


class PrintablePaletteRecommendationTests(unittest.TestCase):
    def recommendation_json(self, **overrides):
        value = {
            "summary": "温暖主体配合深色结构和冷色点缀",
            "colors": [
                {"hex": "#D96B43", "name": "陶土橙", "role": "primary", "usage": "主体服装", "reason": "形成视觉中心"},
                {"hex": "#2B2422", "name": "深棕", "role": "structure", "usage": "头发与轮廓", "reason": "稳定结构边界"},
                {"hex": "#F2D7B5", "name": "暖白", "role": "light", "usage": "面部与高光", "reason": "保持明暗层次"},
                {"hex": "#2F6B5F", "name": "墨绿", "role": "accent", "usage": "配件与底座", "reason": "增加冷暖对比"},
            ],
        }
        value.update(overrides)
        return json.dumps(value, ensure_ascii=False)

    def test_text_recommendation_accepts_fenced_json_and_preserves_roles(self):
        response = "```json\n" + self.recommendation_json() + "\n```"
        with mock.patch.object(preprocessor, "complete_text", return_value=response) as complete:
            result = preprocessor.recommend_printable_palette("一只机械麒麟", "q_cartoon")

        self.assertEqual(result.summary, "温暖主体配合深色结构和冷色点缀")
        self.assertEqual([color.hex for color in result.colors], ["#D96B43", "#2B2422", "#F2D7B5", "#2F6B5F"])
        self.assertEqual([color.role for color in result.colors], ["primary", "structure", "light", "accent"])
        self.assertIn("four-color palette", complete.call_args.args[0])
        self.assertIn("different hue family from primary", complete.call_args.args[0])
        self.assertIn("overrides any monochrome", complete.call_args.args[0])
        self.assertIn("structure visibly dark", complete.call_args.args[0])

    def test_image_recommendation_uses_vision_completion(self):
        with tempfile.TemporaryDirectory() as directory:
            image_path = Path(directory) / "reference.png"
            Image.new("RGB", (8, 8), "red").save(image_path)
            with mock.patch.object(preprocessor, "complete_vision", return_value=self.recommendation_json()) as complete:
                result = preprocessor.recommend_printable_palette(
                    "保留角色身份", "cel_shaded", image_path=image_path
                )

        self.assertEqual(len(result.colors), 4)
        self.assertEqual(complete.call_args.args[2], (image_path,))

    def test_rejects_invalid_provider_responses(self):
        valid = json.loads(self.recommendation_json())
        cases = {
            "malformed": "not json",
            "duplicate_color": json.dumps({**valid, "colors": [*valid["colors"][:3], {**valid["colors"][3], "hex": "#D96B43"}]}),
            "invalid_hex": json.dumps({**valid, "colors": [{**valid["colors"][0], "hex": "orange"}, *valid["colors"][1:]]}),
            "duplicate_role": json.dumps({**valid, "colors": [*valid["colors"][:3], {**valid["colors"][3], "role": "primary"}]}),
            "missing_role": json.dumps({**valid, "colors": valid["colors"][:3]}),
            "long_reason": json.dumps({**valid, "colors": [{**valid["colors"][0], "reason": "x" * 401}, *valid["colors"][1:]]}),
        }
        for name, response in cases.items():
            with self.subTest(name=name), mock.patch.object(preprocessor, "complete_text", return_value=response):
                with self.assertRaises(preprocessor.OpenAIPreprocessorError):
                    preprocessor.recommend_printable_palette("a toy", "low_poly")

    def test_rejects_low_contrast_recommendation_without_retry(self):
        value = json.loads(self.recommendation_json())
        for color, hex_value in zip(value["colors"], ("#777777", "#787878", "#797979", "#7A7A7A")):
            color["hex"] = hex_value
        with mock.patch.object(preprocessor, "complete_text", return_value=json.dumps(value)) as complete:
            with self.assertRaisesRegex(preprocessor.OpenAIPreprocessorError, "contrast"):
                preprocessor.recommend_printable_palette("a toy", "low_poly")
        complete.assert_called_once()


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
        self.assertIn(b'name="quality"\r\n\r\nhigh', captured["body"])
        self.assertIn(b'name="size"\r\n\r\n1024x1024', captured["body"])
        self.assertNotIn(b"designer-ready style preview", captured["body"])


class ImageDownloadTests(unittest.TestCase):
    class Response(BytesIO):
        headers = {}

        def __enter__(self):
            return self

        def __exit__(self, *_args):
            self.close()

    def test_transient_download_failure_retries_only_the_artifact_get(self):
        with tempfile.TemporaryDirectory() as directory:
            destination = Path(directory) / "result.png"
            opener = mock.Mock()
            opener.open.side_effect = [urllib.error.URLError("temporary"), self.Response(b"png-bytes")]
            with (
                mock.patch.object(preprocessor, "_validate_artifact_url"),
                mock.patch.object(preprocessor.urllib.request, "build_opener", return_value=opener),
            ):
                result = preprocessor._download_image("https://cdn.example/result.png", destination)

            self.assertEqual(result, destination)
            self.assertEqual(destination.read_bytes(), b"png-bytes")
        self.assertEqual(opener.open.call_count, 2)

    def test_download_failure_is_actionable_after_one_safe_retry(self):
        with tempfile.TemporaryDirectory() as directory:
            destination = Path(directory) / "result.png"
            opener = mock.Mock()
            opener.open.side_effect = urllib.error.URLError("offline")
            with (
                mock.patch.object(preprocessor, "_validate_artifact_url"),
                mock.patch.object(preprocessor.urllib.request, "build_opener", return_value=opener),
                self.assertRaises(preprocessor.OpenAIPreprocessorError) as raised,
            ):
                preprocessor._download_image("https://cdn.example/result.png", destination)

        self.assertEqual(raised.exception.code, "image_download_failed")
        self.assertTrue(raised.exception.retryable)
        self.assertFalse(raised.exception.ambiguous)
        self.assertEqual(opener.open.call_count, 2)

    def test_download_does_not_retry_a_permanent_client_error(self):
        with tempfile.TemporaryDirectory() as directory:
            destination = Path(directory) / "result.png"
            opener = mock.Mock()
            opener.open.side_effect = urllib.error.HTTPError(
                "https://cdn.example/result.png", 404, "missing", None, None
            )
            with (
                mock.patch.object(preprocessor, "_validate_artifact_url"),
                mock.patch.object(preprocessor.urllib.request, "build_opener", return_value=opener),
                self.assertRaises(preprocessor.OpenAIPreprocessorError),
            ):
                preprocessor._download_image("https://cdn.example/result.png", destination)

        opener.open.assert_called_once()


if __name__ == "__main__":
    unittest.main()
