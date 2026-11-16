"""Delivery never turns a known integration failure into a passing check."""
import json
from pathlib import Path
import subprocess
import shutil
import sys
import unittest
from package_integration_check import packaging_decision
from verify_ai_integration import SCHEMA


class PackageIntegrationTests(unittest.TestCase):
    def setUp(self):
        self.source = {"source_commit": "a" * 40, "source_clean": False}
        self.passed = {"schema": SCHEMA, "git_checks_skipped": False,
                       "git": {"head": self.source["source_commit"]}, "ok": True, "errors": []}

    def test_zero_errors_pass_for_both_channels_and_source_states(self):
        for channel in ("internal", "release"):
            for clean in (False, True):
                with self.subTest(channel=channel, clean=clean):
                    decision = packaging_decision(self.passed, dict(self.source, source_clean=clean), channel=channel)
                    self.assertTrue(decision["package_allowed"])
                    self.assertTrue(decision["integration_passed"])
                    self.assertEqual("PASSED", decision["status"])

    def test_every_failure_blocks_even_when_an_exact_historical_report_exists(self):
        for code in ("architecture.diff_budget", "architecture.file_budget", "boundary.contract", "source.credentials"):
            report = dict(self.passed, ok=False, errors=[{"code": code, "message": "existing error"}])
            for channel in ("internal", "release"):
                for known in (None, report, self.passed):
                    with self.subTest(code=code, channel=channel, known=known):
                        decision = packaging_decision(report, self.source, known, channel=channel)
                        self.assertFalse(decision["package_allowed"])
                        self.assertFalse(decision["integration_passed"])

    def test_retired_report_cannot_be_supplied_even_to_a_passing_candidate(self):
        self.assertFalse(packaging_decision(self.passed, self.source, self.passed, channel="internal")["package_allowed"])

    def test_incomplete_wrong_source_skipped_and_inconsistent_reports_block(self):
        for report in ({}, [], dict(self.passed, git={"head": "b" * 40}),
                       dict(self.passed, schema="unknown"), dict(self.passed, git_checks_skipped=True),
                       dict(self.passed, ok=False), dict(self.passed, errors=[{}]),
                       dict(self.passed, errors=[{"code": "x", "message": "failure"}])):
            with self.subTest(report=report):
                self.assertFalse(packaging_decision(report, self.source)["package_allowed"])

    def test_unknown_channel_blocks(self):
        for channel in ("", "unknown"):
            self.assertFalse(packaging_decision(self.passed, self.source, channel=channel)["package_allowed"])

    def test_legacy_command_line_parameters_fail_before_running_or_reading_reports(self):
        root = Path(__file__).resolve().parents[1]
        commands = [
            [sys.executable, str(root / "scripts/package_integration_check.py"), "--root", str(root),
             "--report", "unused.json", "--known-report", "nonexistent.json"],
            [sys.executable, str(root / "tools/ai/test_integration_guardrails.py"),
             "--internal-package-report", "nonexistent.json"],
            [sys.executable, str(root / "tools/ai/test_integration_guardrails.py"),
             "--source-manifest", "nonexistent.json"],
        ]
        for command in commands:
            with self.subTest(command=command):
                result = subprocess.run(command, capture_output=True, text=True)
                self.assertEqual(2, result.returncode)
                self.assertIn("retired", result.stderr)

    @unittest.skipUnless(shutil.which("pwsh"), "PowerShell Core unavailable")
    def test_legacy_powershell_parameters_fail_before_using_the_build_tree(self):
        root = Path(__file__).resolve().parents[1]
        for script in ("scripts/package_internal_fast.ps1", "release/build_internal.ps1"):
            for channel in ("internal", "commercial"):
                result = subprocess.run(["pwsh", "-NoProfile", "-File", str(root / script),
                    "-BuildDir", "nonexistent", "-KnownIntegrationReport", "nonexistent.json",
                    "-DistributionChannel", channel, "-ValidateOnly"], capture_output=True)
                self.assertNotEqual(0, result.returncode)
                self.assertIn(b"KnownIntegrationReport is retired", result.stderr)


if __name__ == "__main__":
    unittest.main()
