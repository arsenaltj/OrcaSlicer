"""Execute the real paired workflow body with a disposable packaging fixture."""
from __future__ import annotations

import json
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parents[1]
WORKFLOW = yaml.safe_load((ROOT / ".github/workflows/build_orca.yml").read_text(encoding="utf-8"))
STEPS = next(iter(WORKFLOW["jobs"].values()))["steps"]
PAIR = next(step for step in STEPS if step.get("id") == "paired_packaging")


class PackagingWorkflowTests(unittest.TestCase):
    def run_pair(self, failure: str = "") -> tuple[subprocess.CompletedProcess, list[str], dict]:
        pwsh = shutil.which("pwsh")
        self.assertIsNotNone(pwsh, "PowerShell is required to verify workflow execution")
        with tempfile.TemporaryDirectory(prefix="orca-paired-workflow-") as temporary:
            root = Path(temporary)
            (root / "scripts").mkdir()
            (root / "scripts/ci_windows_packaging.ps1").write_text(
                'param($BuildDir, $Version, $Architecture, $Profile, $MetricsPath, '
                '$IdentityName, $Publisher, $PublisherDisplayName, $OutputDirectory)\n'
                'Add-Content -LiteralPath $env:PAIR_TRACE -Value $Profile\n'
                'if ($Profile -eq $env:PAIR_FAILURE) { throw "fixture packaging failed: $Profile" }\n',
                encoding="utf-8")
            body = PAIR["run"].replace("${{ inputs.arch }}", "x64").replace("${{ inputs.compiler }}", "msvc")
            script = root / "run.ps1"
            script.write_text(body, encoding="utf-8")
            trace = root / "trace.txt"
            env = dict(os.environ, GITHUB_WORKSPACE=str(root), RUNNER_TEMP=str(root / "temp"),
                       BUILD_DIR="build", ver="fixture", BENCHMARK_REF="normal-pr",
                       GITHUB_SHA="a" * 40, GITHUB_RUN_ID="123", GITHUB_RUN_ATTEMPT="1",
                       MSIX_IDENTITY="fixture", MSIX_PUBLISHER="CN=fixture", MSIX_DISPLAY="fixture",
                       PAIR_TRACE=str(trace), PAIR_FAILURE=failure)
            process = subprocess.run([pwsh, "-NoProfile", "-NonInteractive", "-File", str(script)],
                                     cwd=root, env=env, capture_output=True, text=True,
                                     encoding="utf-8", timeout=30, check=False)
            execution = json.loads((root / "temp/orca-ci-windows-x64-msvc/packaging-execution.json")
                                   .read_text(encoding="utf-8-sig"))
            return process, trace.read_text(encoding="utf-8-sig").splitlines(), execution

    def test_one_pair_reuses_build_and_records_source(self):
        process, profiles, report = self.run_pair()
        self.assertEqual(process.returncode, 0, process.stderr)
        self.assertEqual(profiles, ["baseline", "optimized"])
        self.assertEqual(report["status"], "succeeded")
        self.assertEqual(report["completed_profiles"], profiles)
        self.assertEqual(report["candidate_sha"], "a" * 40)
        self.assertEqual(report["run_id"], "123")

    def test_baseline_failure_stops_optimized_and_leaves_failed_record(self):
        process, profiles, report = self.run_pair("baseline")
        self.assertNotEqual(process.returncode, 0)
        self.assertEqual(profiles, ["baseline"])
        self.assertEqual(report["status"], "failed")
        self.assertEqual(report["completed_profiles"], [])
        self.assertEqual(report["current_profile"], "baseline")
        self.assertIn("fixture packaging failed", report["error"])

    def test_optimized_failure_retains_completed_baseline_identity(self):
        process, profiles, report = self.run_pair("optimized")
        self.assertNotEqual(process.returncode, 0)
        self.assertEqual(profiles, ["baseline", "optimized"])
        self.assertEqual(report["status"], "failed")
        self.assertEqual(report["completed_profiles"], ["baseline"])
        self.assertEqual(report["current_profile"], "optimized")

    def test_failure_archives_are_diagnostic_and_normal_uploads_remain_gated(self):
        diagnostic = next(step for step in STEPS if step.get("name") == "Preserve failed Windows packaging evidence")
        self.assertIn("failure()", diagnostic["if"])
        for owner in ("paired_packaging", "optimized_packaging", "packaging_comparison"):
            self.assertIn(f"steps.{owner}.outcome == 'failure'", diagnostic["if"])
        self.assertTrue(diagnostic["with"]["name"].startswith("windows-packaging-failed-"))
        self.assertTrue(diagnostic["with"]["include-hidden-files"])
        for suffix in ("*.exe", "*.msix", "*.7z", "*.zip"):
            self.assertIn(".ci-windows-packaging/**/" + suffix, diagnostic["with"]["path"])
        for name in ("Upload artifacts Win zip", "Upload artifacts Win installer",
                     "Upload artifacts Win PDB", "Upload artifacts Win MSIX"):
            step = next(step for step in STEPS if step.get("name") == name)
            self.assertNotIn("always()", step["if"])
            self.assertNotIn("failure()", step["if"])
            self.assertNotIn("continue-on-error", step)
        self.assertEqual(sum(step.get("name") == "Build slicer Win" for step in STEPS), 1)


if __name__ == "__main__":
    unittest.main()
