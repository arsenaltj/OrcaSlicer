"""Internal packaging must retain all integration failures and enforce the channel."""
from __future__ import annotations

import copy
import unittest

from package_integration_check import internal_budget_expectation, packaging_decision


class PackageIntegrationTests(unittest.TestCase):
    def setUp(self):
        self.source = {
            "source_commit": "a" * 40,
            "source_clean": False,
            "source_identity_sha256": "b" * 64,
        }
        self.known = {
            "schema": "orcaslicer.ai-integration-lock/v4",
            "ok": False,
            "git_checks_skipped": False,
            "git": {"head": "a" * 40},
            "errors": [{"code": "architecture.diff_budget", "message": "Existing.cpp exceeds its budget"}],
        }

    def test_complete_passing_check_needs_no_exception(self):
        report = dict(self.known, ok=True, errors=[])
        result = packaging_decision(report, dict(self.source, source_clean=True))
        self.assertTrue(result["package_allowed"])
        self.assertTrue(result["integration_passed"])
        self.assertEqual(result["status"], "PASSED")

    def test_failure_without_explicit_baseline_blocks(self):
        self.assertFalse(packaging_decision(self.known, self.source, channel="internal")["package_allowed"])

    def test_exact_known_snapshot_findings_are_recorded_as_not_passed(self):
        result = packaging_decision(copy.deepcopy(self.known), self.source, self.known, channel="internal")
        self.assertTrue(result["package_allowed"])
        self.assertFalse(result["integration_passed"])
        self.assertEqual(result["known_findings_count"], 1)
        self.assertEqual(result["status"], "KNOWN_ARCHITECTURE_BUDGET_FINDINGS")

    def test_new_or_changed_findings_block(self):
        for errors in (
            self.known["errors"] + [{"code": "architecture.diff_budget", "message": "New.cpp exceeds its budget"}],
            [{"code": "architecture.diff_budget", "message": "Existing.cpp now has a larger diff"}],
            self.known["errors"] * 2,
        ):
            with self.subTest(errors=errors):
                self.assertFalse(packaging_decision(dict(self.known, errors=errors), self.source, self.known, channel="internal")["package_allowed"])

    def test_non_budget_failure_cannot_be_allowlisted(self):
        for code in ("boundary.contract", "source.credentials", "architecture.diff_base", "lock.read"):
            report = dict(self.known, errors=[{"code": code, "message": "Existing failure"}])
            with self.subTest(code=code):
                self.assertFalse(packaging_decision(report, self.source, report, channel="internal")["package_allowed"])

    def test_other_head_or_schema_blocks(self):
        for field, value in (("git", {"head": "c" * 40}), ("schema", "unknown")):
            report = dict(self.known, **{field: value})
            with self.subTest(field=field):
                self.assertFalse(packaging_decision(self.known, self.source, report, channel="internal")["package_allowed"])
                self.assertFalse(packaging_decision(report, self.source, self.known, channel="internal")["package_allowed"])

    def test_clean_internal_source_can_record_known_findings_without_passing_integration(self):
        result = packaging_decision(self.known, dict(self.source, source_clean=True),
                                    self.known, channel="internal")
        self.assertTrue(result["package_allowed"])
        self.assertFalse(result["integration_passed"])
        self.assertEqual(result["known_findings_count"], 1)

    def test_release_and_unspecified_channels_block_exceptions_for_both_source_states(self):
        for clean in (True, False):
            source = dict(self.source, source_clean=clean)
            with self.subTest(clean=clean):
                self.assertFalse(packaging_decision(self.known, source, self.known)["package_allowed"])
                for channel in ("release", "", "unknown"):
                    self.assertFalse(packaging_decision(self.known, source, self.known,
                                                       channel=channel)["package_allowed"])

    def test_clean_internal_source_still_rejects_missing_changed_and_non_budget_baselines(self):
        source = dict(self.source, source_clean=True)
        self.assertFalse(packaging_decision(self.known, source, channel="internal")["package_allowed"])
        for code in ("architecture.diff_budget", "source.credentials"):
            report = dict(self.known, errors=[{"code": code, "message": "New failure"}])
            self.assertFalse(packaging_decision(report, source, self.known,
                                               channel="internal")["package_allowed"])
        self.assertFalse(packaging_decision(report, source, report,
                                           channel="internal")["package_allowed"])

    def test_exception_requires_boolean_source_cleanliness(self):
        for clean in (None, 0, 1, "false"):
            with self.subTest(clean=clean):
                self.assertFalse(packaging_decision(self.known, dict(self.source, source_clean=clean),
                                                   self.known, channel="internal")["package_allowed"])

    def test_internal_test_expectations_preserve_exact_budget_failures(self):
        record = {"distribution_channel": "internal", "source_clean": False,
                  "source_identity_sha256": self.source["source_identity_sha256"],
                  "integration_report": self.known,
                  "decision": packaging_decision(self.known, self.source, self.known, channel="internal")}
        self.assertEqual(self.known["errors"], internal_budget_expectation(record, self.source))
        for key, value in (("distribution_channel", "release"), ("source_clean", True),
                           ("source_identity_sha256", "other"), ("decision", {}),
                           ("integration_report", dict(self.known, git_checks_skipped=True))):
            with self.subTest(key=key), self.assertRaises(ValueError):
                internal_budget_expectation(dict(record, **{key: value}), self.source)

    def test_non_budget_and_passing_reports_cannot_set_internal_test_expectations(self):
        for report in (dict(self.known, ok=True, errors=[]),
                       dict(self.known, errors=[{"code": "source.credentials", "message": "secret"}])):
            record = {"distribution_channel": "internal", "source_clean": False,
                      "source_identity_sha256": self.source["source_identity_sha256"],
                      "integration_report": report,
                      "decision": packaging_decision(report, self.source, report, channel="internal")}
            with self.assertRaises(ValueError):
                internal_budget_expectation(record, self.source)

    def test_skipped_git_checks_block_even_when_report_says_pass(self):
        for report in (dict(self.known, git_checks_skipped=True), dict(self.known, ok=True, errors=[], git_checks_skipped=True)):
            with self.subTest(report=report):
                self.assertFalse(packaging_decision(report, self.source, self.known, channel="internal")["package_allowed"])

    def test_inconsistent_and_malformed_reports_block(self):
        for report in ({}, [], dict(self.known, ok=True), dict(self.known, errors=[]), dict(self.known, errors=[{}])):
            with self.subTest(report=report):
                self.assertFalse(packaging_decision(report, self.source, self.known, channel="internal")["package_allowed"])


if __name__ == "__main__":
    unittest.main()
