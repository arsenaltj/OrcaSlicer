from __future__ import annotations

import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
import zipfile


REPO_ROOT = Path(__file__).resolve().parents[2]
GENERATOR = REPO_ROOT / "release" / "create_ai_provisioner.ps1"
INSTALLER = REPO_ROOT / "release" / "internal-ai-provisioner" / "Install-OrcaAIConfig.ps1"
POWERSHELL = shutil.which("powershell.exe") or shutil.which("powershell") or shutil.which("pwsh")
SYNTHETIC_KEY = "synthetic-provisioner-key-not-a-real-secret"
BASE_URL = "https://v.3dprint.beer/managed-ai/v1"


@unittest.skipUnless(os.name == "nt" and POWERSHELL, "Windows PowerShell is required")
class InternalAIProvisionerTests(unittest.TestCase):
    def run_powershell(self, script: Path, *arguments: str, environment: dict[str, str] | None = None):
        return subprocess.run(
            [
                str(POWERSHELL),
                "-NoLogo",
                "-NoProfile",
                "-ExecutionPolicy",
                "Bypass",
                "-File",
                str(script),
                *arguments,
            ],
            cwd=REPO_ROOT,
            env=environment,
            capture_output=True,
            text=True,
            timeout=60,
            check=False,
        )

    def test_generator_creates_valid_secret_bearing_bundle_without_logging_key(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            environment = os.environ.copy()
            environment["OPENAI_PRO_API"] = SYNTHETIC_KEY
            environment["OPENAI_PRO_URL"] = BASE_URL
            result = self.run_powershell(
                GENERATOR,
                "-OutputDir",
                directory,
                "-KeySourceScope",
                "Process",
                environment=environment,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertNotIn(SYNTHETIC_KEY, result.stdout)
            self.assertNotIn(SYNTHETIC_KEY, result.stderr)

            bundles = list(Path(directory).glob("*.zip"))
            self.assertEqual(len(bundles), 1)
            checksum = bundles[0].with_suffix(".zip.sha256")
            self.assertTrue(checksum.is_file())
            self.assertNotIn(SYNTHETIC_KEY, checksum.read_text(encoding="ascii"))

            with zipfile.ZipFile(bundles[0]) as archive:
                names = set(archive.namelist())
                self.assertIn("Install-OrcaAIConfig.cmd", names)
                self.assertIn("Remove-OrcaAIConfig.cmd", names)
                payload = json.loads(archive.read("orca-ai-provisioner.json").decode("utf-8-sig"))
                manifest_text = archive.read("manifest.json").decode("utf-8-sig")
                readme_text = archive.read("README.txt").decode("utf-8-sig")

            self.assertEqual(payload["OPENAI_PRO_API"], SYNTHETIC_KEY)
            self.assertEqual(payload["OPENAI_PRO_URL"], BASE_URL)
            self.assertNotIn(SYNTHETIC_KEY, manifest_text)
            self.assertNotIn(SYNTHETIC_KEY, readme_text)
            manifest = json.loads(manifest_text)
            self.assertTrue(manifest["contains_extractable_credential"])
            self.assertFalse(manifest["paid_request_performed"])

            extract_dir = Path(directory) / "extracted"
            with zipfile.ZipFile(bundles[0]) as archive:
                archive.extractall(extract_dir)
            validation = self.run_powershell(
                extract_dir / "Install-OrcaAIConfig.ps1",
                "-ValidateOnly",
            )
            self.assertEqual(validation.returncode, 0, validation.stderr)
            self.assertIn("validation passed", validation.stdout)
            self.assertNotIn(SYNTHETIC_KEY, validation.stdout)
            self.assertNotIn(SYNTHETIC_KEY, validation.stderr)

    def test_generator_rejects_missing_process_key(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            environment = os.environ.copy()
            environment.pop("OPENAI_PRO_API", None)
            result = self.run_powershell(
                GENERATOR,
                "-OutputDir",
                directory,
                "-KeySourceScope",
                "Process",
                "-BaseUrl",
                BASE_URL,
                environment=environment,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("OPENAI_PRO_API is not configured", result.stderr)
            self.assertEqual(list(Path(directory).glob("*.zip")), [])

    def test_installer_validate_only_rejects_unexpected_payload_fields(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            payload_path = Path(directory) / "bad.json"
            payload_path.write_text(
                json.dumps(
                    {
                        "version": 1,
                        "mode": "internal_user_environment",
                        "OPENAI_PRO_API": SYNTHETIC_KEY,
                        "OPENAI_PRO_URL": BASE_URL,
                        "UNRELATED_SECRET": "must-not-be-accepted",
                    }
                ),
                encoding="utf-8",
            )
            result = self.run_powershell(
                INSTALLER,
                "-PayloadPath",
                str(payload_path),
                "-ValidateOnly",
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("unsupported field", result.stderr)
            self.assertNotIn(SYNTHETIC_KEY, result.stdout)
            self.assertNotIn(SYNTHETIC_KEY, result.stderr)

    def test_installer_validate_only_rejects_non_https_url(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            payload_path = Path(directory) / "bad.json"
            payload_path.write_text(
                json.dumps(
                    {
                        "version": 1,
                        "mode": "internal_user_environment",
                        "OPENAI_PRO_API": SYNTHETIC_KEY,
                        "OPENAI_PRO_URL": "http://example.invalid/v1",
                    }
                ),
                encoding="utf-8",
            )
            result = self.run_powershell(
                INSTALLER,
                "-PayloadPath",
                str(payload_path),
                "-ValidateOnly",
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("absolute HTTPS URL", result.stderr)
            self.assertNotIn(SYNTHETIC_KEY, result.stdout)
            self.assertNotIn(SYNTHETIC_KEY, result.stderr)


if __name__ == "__main__":
    unittest.main()
