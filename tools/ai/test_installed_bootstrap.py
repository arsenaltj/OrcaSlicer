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
    def test_build_info_sets_only_valid_non_secret_identity(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            build_info_path = Path(directory) / "build-info.json"
            payload = {
                "schema_version": 1,
                "application_version": "2.5.0-dev",
                "application_commit": "a" * 40,
                "package_revision": "commercial-20260827.1",
                "distribution_channel": "commercial",
                "sidecar_protocol_version": 2,
                "sidecar_version": "orcaslicer-ai-sidecar-v9",
            }
            build_info_path.write_text(json.dumps(payload), encoding="utf-8")
            with mock.patch.dict(os.environ, {}, clear=True):
                self.assertEqual(BOOTSTRAP.load_build_info(build_info_path), payload)
                self.assertEqual(os.environ["ORCASLICER_AI_APP_COMMIT"], "a" * 40)
                self.assertEqual(os.environ["ORCASLICER_AI_PACKAGE_REVISION"], "commercial-20260827.1")
                self.assertEqual(os.environ["ORCASLICER_AI_DISTRIBUTION_CHANNEL"], "commercial")

    def test_build_info_rejects_unknown_fields_and_invalid_identity(self) -> None:
        valid = {
            "schema_version": 1,
            "application_version": "2.5.0-dev",
            "application_commit": "b" * 40,
            "package_revision": "test",
            "distribution_channel": "internal",
            "sidecar_protocol_version": 2,
            "sidecar_version": "orcaslicer-ai-sidecar-v9",
        }
        invalid_payloads = (
            {**valid, "secret": "must-not-be-accepted"},
            {**valid, "application_commit": "short"},
            {**valid, "package_revision": "bad revision"},
            {**valid, "distribution_channel": "public-with-keys"},
            {**valid, "sidecar_version": "orcaslicer-ai-sidecar-v6"},
        )
        with tempfile.TemporaryDirectory() as directory:
            build_info_path = Path(directory) / "build-info.json"
            for payload in invalid_payloads:
                with self.subTest(payload=payload):
                    build_info_path.write_text(json.dumps(payload), encoding="utf-8")
                    with mock.patch.dict(os.environ, {}, clear=True):
                        self.assertEqual(BOOTSTRAP.load_build_info(build_info_path), {})
                        self.assertNotIn("ORCASLICER_AI_APP_COMMIT", os.environ)

    def test_internal_locked_defaults_override_allowlisted_environment_values(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            defaults_path = Path(directory) / "defaults.json"
            defaults_path.write_text(json.dumps({
                "version": 1,
                "mode": "internal_locked",
                "OPENAI_PRO_API": "packaged-pro",
                "OPENAI_PRO_URL": "https://image.example/v1",
                "OPENAI_API_KEY": "packaged-openai",
                "OPENAI_BASE_URL": "https://internal.example/v1",
                "TRIPO_API_KEY": "packaged-tripo",
            }), encoding="utf-8")
            environment = {
                "OPENAI_PRO_API": "explicit-pro",
                "OPENAI_PRO_URL": "https://stale.example/v1",
                "OPENAI_API_KEY": "explicit-openai",
            }
            with mock.patch.dict(os.environ, environment, clear=True):
                loaded = BOOTSTRAP.load_internal_defaults(defaults_path)
                self.assertEqual(os.environ["OPENAI_PRO_API"], "packaged-pro")
                self.assertEqual(os.environ["OPENAI_PRO_URL"], "https://image.example/v1")
                self.assertEqual(os.environ["OPENAI_API_KEY"], "packaged-openai")
                self.assertEqual(os.environ["OPENAI_BASE_URL"], "https://internal.example/v1")
                self.assertEqual(os.environ["TRIPO_API_KEY"], "packaged-tripo")
                self.assertEqual(os.environ["ORCASLICER_AI_CONFIG_MODE"], "internal_locked")
                self.assertEqual(loaded, (
                    "OPENAI_API_KEY", "OPENAI_BASE_URL", "OPENAI_PRO_API",
                    "OPENAI_PRO_URL", "TRIPO_API_KEY",
                ))

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
                json.dumps({
                    "version": 1,
                    "mode": "internal_locked",
                    "OPENAI_PRO_API": "pro-secret",
                    "OPENAI_PRO_URL": "http://image.example/v1",
                    "OPENAI_API_KEY": "openai-secret",
                    "OPENAI_BASE_URL": "https://text.example/v1",
                    "TRIPO_API_KEY": "tripo-secret",
                }),
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

    def test_offline_install_verification_requires_locked_internal_defaults(self) -> None:
        build_info = {
            "schema_version": 1,
            "application_version": "2.5.0-dev",
            "application_commit": "a" * 40,
            "package_revision": "internal-test",
            "distribution_channel": "internal",
            "sidecar_protocol_version": 2,
            "sidecar_version": "orcaslicer-ai-sidecar-v9",
        }
        defaults = {
            "version": 1,
            "mode": "internal_locked",
            "OPENAI_PRO_API": "packaged-pro",
            "OPENAI_PRO_URL": "https://image.example/v1",
            "OPENAI_API_KEY": "packaged-openai",
            "OPENAI_BASE_URL": "https://text.example/v1",
            "TRIPO_API_KEY": "packaged-tripo",
            "TRIPO_API_BASE": "https://tripo.example/v3",
        }
        with tempfile.TemporaryDirectory() as directory:
            build_info_path = Path(directory) / "build-info.json"
            defaults_path = Path(directory) / "defaults.json"
            build_info_path.write_text(json.dumps(build_info), encoding="utf-8")
            defaults_path.write_text(json.dumps(defaults), encoding="utf-8")
            with mock.patch.dict(os.environ, {}, clear=True):
                report = BOOTSTRAP.verify_installed_configuration(build_info_path, defaults_path)
            self.assertEqual(report["distribution_channel"], "internal")
            self.assertEqual(report["configuration_mode"], "internal_locked")
            self.assertEqual(report["configured_count"], 6)

            defaults_path.unlink()
            with self.assertRaisesRegex(RuntimeError, "locked provider configuration"):
                BOOTSTRAP.verify_installed_configuration(build_info_path, defaults_path)

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
                valid_build_info = {
                    "schema_version": 1,
                    "application_version": "2.5.0-dev",
                    "application_commit": "d" * 40,
                    "package_revision": "installed-test",
                    "distribution_channel": "internal",
                    "sidecar_protocol_version": 2,
                    "sidecar_version": "orcaslicer-ai-sidecar-v9",
                }
                with mock.patch.dict(os.environ, {"ORCASLICER_AI_PARENT_PID": str(os.getpid())}, clear=False), \
                     mock.patch.object(BOOTSTRAP, "load_build_info", return_value=valid_build_info), \
                     mock.patch.object(BOOTSTRAP, "load_internal_defaults", return_value=()), \
                     mock.patch.object(BOOTSTRAP.runpy, "run_path", side_effect=fake_run_path):
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

    def test_internal_installed_sidecar_preserves_machine_or_user_provider_environment(self) -> None:
        valid_build_info = {
            "schema_version": 1,
            "application_version": "2.5.0-dev",
            "application_commit": "9" * 40,
            "package_revision": "internal-environment-test",
            "distribution_channel": "internal",
            "sidecar_protocol_version": 2,
            "sidecar_version": "orcaslicer-ai-sidecar-v9",
        }
        captured: dict[str, str | None] = {}

        def fake_run_path(path: str, *, run_name: str) -> None:
            del path, run_name
            captured["pro_api"] = os.environ.get("OPENAI_PRO_API")
            captured["pro_url"] = os.environ.get("OPENAI_PRO_URL")
            captured["legacy_api"] = os.environ.get("OPENAI_API_KEY")

        with tempfile.TemporaryDirectory() as directory:
            original_stdout, original_stderr, original_argv = sys.stdout, sys.stderr, sys.argv
            try:
                with mock.patch.dict(os.environ, {
                    "ORCASLICER_AI_PARENT_PID": str(os.getpid()),
                    "OPENAI_PRO_API": "test-pro",
                    "OPENAI_PRO_URL": "https://v.3dprint.beer/managed-ai/v1",
                    "OPENAI_API_KEY": "test-legacy",
                }, clear=False), mock.patch.object(
                    BOOTSTRAP, "load_build_info", return_value=valid_build_info
                ), mock.patch.object(BOOTSTRAP.runpy, "run_path", side_effect=fake_run_path):
                    BOOTSTRAP.run_installed_sidecar(directory)
            finally:
                redirected = sys.stdout
                sys.stdout, sys.stderr = original_stdout, original_stderr
                sys.argv = original_argv
                if redirected is not original_stdout:
                    redirected.close()

        self.assertEqual(captured, {
            "pro_api": "test-pro",
            "pro_url": "https://v.3dprint.beer/managed-ai/v1",
            "legacy_api": "test-legacy",
        })

    def test_installed_sidecar_requires_valid_build_identity(self) -> None:
        with tempfile.TemporaryDirectory() as directory, \
             mock.patch.object(BOOTSTRAP, "load_build_info", return_value={}):
            with self.assertRaisesRegex(RuntimeError, "build identity"):
                BOOTSTRAP.run_installed_sidecar(directory)
            records = [
                json.loads(line)
                for line in (Path(directory) / "log" / "orca-ai-sidecar.log").read_text(
                    encoding="utf-8"
                ).splitlines()
            ]
            self.assertEqual(records[-1]["code"], "build_identity_invalid")

    def test_installed_sidecar_requires_parent_process_identity(self) -> None:
        valid_build_info = {
            "schema_version": 1,
            "application_version": "2.5.0-dev",
            "application_commit": "e" * 40,
            "package_revision": "installed-test",
            "distribution_channel": "internal",
            "sidecar_protocol_version": 2,
            "sidecar_version": "orcaslicer-ai-sidecar-v9",
        }
        with tempfile.TemporaryDirectory() as directory, \
             mock.patch.dict(os.environ, {"ORCASLICER_AI_PARENT_PID": "invalid"}, clear=False), \
             mock.patch.object(BOOTSTRAP, "load_build_info", return_value=valid_build_info):
            with self.assertRaisesRegex(RuntimeError, "parent process"):
                BOOTSTRAP.run_installed_sidecar(directory)
            record = json.loads(
                (Path(directory) / "log" / "orca-ai-sidecar.log").read_text(encoding="utf-8")
            )
            self.assertEqual(record["code"], "parent_process_invalid")

    def test_commercial_runtime_drops_inherited_provider_credentials(self) -> None:
        valid_build_info = {
            "schema_version": 1,
            "application_version": "2.5.0-dev",
            "application_commit": "f" * 40,
            "package_revision": "commercial-test",
            "distribution_channel": "commercial",
            "sidecar_protocol_version": 2,
            "sidecar_version": "orcaslicer-ai-sidecar-v9",
        }
        captured: dict[str, str | None] = {}

        def fake_run_path(path: str, *, run_name: str) -> None:
            del path, run_name
            captured["openai"] = os.environ.get("OPENAI_API_KEY")
            captured["openai_pro"] = os.environ.get("OPENAI_PRO_API")
            captured["tripo"] = os.environ.get("TRIPO_API_KEY")

        with tempfile.TemporaryDirectory() as directory:
            original_stdout, original_stderr, original_argv = sys.stdout, sys.stderr, sys.argv
            try:
                with mock.patch.dict(os.environ, {
                     "ORCASLICER_AI_PARENT_PID": str(os.getpid()),
                     "OPENAI_API_KEY": "fake-inherited-openai",
                     "OPENAI_PRO_API": "fake-inherited-pro",
                     "TRIPO_API_KEY": "fake-inherited-tripo",
                     }, clear=False), \
                     mock.patch.object(BOOTSTRAP, "load_build_info", return_value=valid_build_info), \
                     mock.patch.object(BOOTSTRAP.runpy, "run_path", side_effect=fake_run_path):
                    BOOTSTRAP.run_installed_sidecar(directory)
            finally:
                redirected = sys.stdout
                sys.stdout, sys.stderr = original_stdout, original_stderr
                sys.argv = original_argv
                if redirected is not original_stdout:
                    redirected.close()
        self.assertEqual(captured, {"openai": None, "openai_pro": None, "tripo": None})


if __name__ == "__main__":
    unittest.main()
