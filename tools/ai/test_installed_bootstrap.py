from __future__ import annotations

import importlib.util
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock


MODULE_PATH = Path(__file__).with_name("orca_ai_installed_bootstrap.py")
SPEC = importlib.util.spec_from_file_location("orca_ai_installed_bootstrap_tests", MODULE_PATH)
assert SPEC and SPEC.loader
BOOTSTRAP = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BOOTSTRAP)


class InstalledBootstrapTests(unittest.TestCase):
    def test_internal_locked_defaults_override_allowlisted_environment_values(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            defaults_path = Path(directory) / "defaults.json"
            defaults_path.write_text(json.dumps({
                "version": 1,
                "mode": "internal_locked",
                "OPENAI_API_KEY": "packaged-openai",
                "OPENAI_BASE_URL": "https://internal.example/v1",
                "TRIPO_API_KEY": "packaged-tripo",
            }), encoding="utf-8")
            environment = {
                "OPENAI_API_KEY": "explicit-openai",
            }
            with mock.patch.dict(os.environ, environment, clear=True):
                loaded = BOOTSTRAP.load_internal_defaults(defaults_path)
                self.assertEqual(os.environ["OPENAI_API_KEY"], "packaged-openai")
                self.assertEqual(os.environ["OPENAI_BASE_URL"], "https://internal.example/v1")
                self.assertEqual(os.environ["TRIPO_API_KEY"], "packaged-tripo")
                self.assertEqual(os.environ["ORCASLICER_AI_CONFIG_MODE"], "internal_locked")
                self.assertEqual(loaded, ("OPENAI_API_KEY", "OPENAI_BASE_URL", "TRIPO_API_KEY"))

    def test_internal_defaults_reject_unknown_malformed_and_oversized_payloads(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            defaults_path = Path(directory) / "defaults.json"
            invalid_payloads = (
                "not-json",
                json.dumps({"version": True, "mode": "internal_locked", "OPENAI_API_KEY": "secret"}),
                json.dumps({"version": 1, "OPENAI_API_KEY": "secret"}),
                json.dumps({"version": 1, "mode": "fallback", "OPENAI_API_KEY": "secret"}),
                json.dumps({"version": 1, "mode": "internal_locked", "OPENAI_API_KEY": "secret", "UNEXPECTED": "value"}),
                json.dumps({"version": 1, "mode": "internal_locked", "OPENAI_API_KEY": "secret\nvalue"}),
                json.dumps({"version": 1, "mode": "internal_locked", "OPENAI_API_KEY": "secret"}),
                " " * (BOOTSTRAP.MAX_INTERNAL_DEFAULTS_BYTES + 1),
            )
            for payload in invalid_payloads:
                with self.subTest(payload_size=len(payload)):
                    defaults_path.write_text(payload, encoding="utf-8")
                    with mock.patch.dict(os.environ, {}, clear=True):
                        self.assertEqual(BOOTSTRAP.load_internal_defaults(defaults_path), ())
                        self.assertNotIn("OPENAI_API_KEY", os.environ)
                        self.assertNotIn("ORCASLICER_AI_CONFIG_MODE", os.environ)

    def test_configure_runtime_uses_data_directory_and_preserves_override(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            override = str(Path(directory) / "custom-output")
            with mock.patch.dict(os.environ, {"ORCASLICER_AI_OUTPUT_DIR": override}, clear=False):
                output_dir, log_path = BOOTSTRAP.configure_runtime(directory)
                self.assertEqual(os.environ.get("ORCASLICER_AI_OUTPUT_DIR"), override)

            self.assertEqual(output_dir, Path(directory).resolve() / "generated_models")
            self.assertEqual(log_path, Path(directory).resolve() / "log" / "orca-ai-sidecar.log")
            self.assertTrue(output_dir.is_dir())
            self.assertTrue(log_path.parent.is_dir())

    def test_main_rejects_missing_data_directory(self) -> None:
        self.assertEqual(BOOTSTRAP.main([]), 2)

    def test_rotate_log_keeps_three_bounded_backups(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            log_path = Path(directory) / "orca-ai-sidecar.log"
            log_path.write_text("current", encoding="utf-8")
            for index in range(1, 4):
                log_path.with_name(f"{log_path.name}.{index}").write_text(str(index), encoding="utf-8")

            BOOTSTRAP.rotate_log(log_path, max_bytes=1, backups=3)

            self.assertFalse(log_path.exists())
            self.assertEqual(log_path.with_name(f"{log_path.name}.1").read_text(encoding="utf-8"), "current")
            self.assertEqual(log_path.with_name(f"{log_path.name}.2").read_text(encoding="utf-8"), "1")
            self.assertEqual(log_path.with_name(f"{log_path.name}.3").read_text(encoding="utf-8"), "2")

    def test_run_installed_sidecar_sets_path_and_executes_production_script(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            captured: dict[str, object] = {}

            def fake_run_path(path: str, *, run_name: str) -> None:
                captured["path"] = path
                captured["run_name"] = run_name

            original_stdout, original_stderr, original_argv = sys.stdout, sys.stderr, sys.argv
            try:
                with mock.patch.object(BOOTSTRAP.runpy, "run_path", side_effect=fake_run_path):
                    BOOTSTRAP.run_installed_sidecar(directory)
                sidecar_argv = list(sys.argv)
            finally:
                redirected = sys.stdout
                sys.stdout, sys.stderr = original_stdout, original_stderr
                sys.argv = original_argv
                redirected.close()

            self.assertEqual(Path(str(captured["path"])), MODULE_PATH.with_name("orca_ai_sidecar.py").resolve())
            self.assertEqual(captured["run_name"], "__main__")
            self.assertEqual(sidecar_argv, [str(MODULE_PATH.with_name("orca_ai_sidecar.py").resolve())])


if __name__ == "__main__":
    unittest.main()
