from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock


MODULE_PATH = Path(__file__).with_name("create_internal_defaults.py")
SPEC = importlib.util.spec_from_file_location("create_internal_defaults_tests", MODULE_PATH)
assert SPEC and SPEC.loader
GENERATOR = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GENERATOR)


class CreateInternalDefaultsTests(unittest.TestCase):
    def test_build_payload_only_copies_supported_nonempty_values(self) -> None:
        payload = GENERATOR.build_payload({
            "OPENAI_PRO_API": "pro-secret",
            "OPENAI_PRO_URL": "https://image.example/v1",
            "OPENAI_API_KEY": "openai-secret",
            "OPENAI_BASE_URL": "https://internal.example/v1",
            "TRIPO_API_KEY": "tripo-secret",
            "UNRELATED_SECRET": "must-not-copy",
        })
        self.assertEqual(payload["version"], 1)
        self.assertEqual(payload["mode"], "internal_locked")
        self.assertEqual(payload["OPENAI_PRO_API"], "pro-secret")
        self.assertEqual(payload["OPENAI_PRO_URL"], "https://image.example/v1")
        self.assertEqual(payload["OPENAI_API_KEY"], "openai-secret")
        self.assertEqual(payload["TRIPO_API_KEY"], "tripo-secret")
        self.assertEqual(payload["TRIPO_API_BASE"], GENERATOR.DEFAULT_TRIPO_API_BASE)
        self.assertNotIn("UNRELATED_SECRET", payload)

    def test_main_reports_only_count_and_writes_atomic_json(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            output_path = Path(directory) / "defaults.json"
            environment = {
                "OPENAI_PRO_API": "pro-secret",
                "OPENAI_PRO_URL": "https://image.example/v1",
                "OPENAI_API_KEY": "openai-secret",
                "OPENAI_BASE_URL": "https://internal.example/v1",
                "TRIPO_API_KEY": "tripo-secret",
            }
            with mock.patch.dict(GENERATOR.os.environ, environment, clear=True), \
                    mock.patch("builtins.print") as print_mock:
                self.assertEqual(GENERATOR.main([str(output_path)]), 0)

            payload = json.loads(output_path.read_text(encoding="utf-8"))
            self.assertEqual(payload["OPENAI_API_KEY"], "openai-secret")
            output = " ".join(str(argument) for call in print_mock.call_args_list for argument in call.args)
            self.assertNotIn("pro-secret", output)
            self.assertNotIn("openai-secret", output)
            self.assertNotIn("tripo-secret", output)
            self.assertIn("6 configured setting(s)", output)

    def test_incomplete_internal_configuration_is_rejected(self) -> None:
        with self.assertRaisesRegex(ValueError, "Missing required internal setting"):
            GENERATOR.build_payload({
                "OPENAI_API_KEY": "openai-secret",
                "OPENAI_BASE_URL": "https://internal.example/v1",
                "TRIPO_API_KEY": "tripo-secret",
            })

    def test_provider_base_urls_must_be_https_without_embedded_credentials(self) -> None:
        complete = {
            "OPENAI_PRO_API": "pro-secret",
            "OPENAI_PRO_URL": "https://image.example/v1",
            "OPENAI_API_KEY": "openai-secret",
            "OPENAI_BASE_URL": "https://text.example/v1",
            "TRIPO_API_KEY": "tripo-secret",
        }
        for name, value in (
            ("OPENAI_PRO_URL", "http://image.example/v1"),
            ("OPENAI_BASE_URL", "https://user:secret@text.example/v1"),
            ("TRIPO_API_BASE", "https://tripo.example/v3?key=secret"),
        ):
            with self.subTest(name=name):
                with self.assertRaisesRegex(ValueError, "absolute HTTPS URL"):
                    GENERATOR.build_payload({**complete, name: value})


if __name__ == "__main__":
    unittest.main()
