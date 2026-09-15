"""Offline selection checks using actual local merge commits and shallow fetches."""

from __future__ import annotations

import copy
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import ci_windows_packaging_scope as packaging
import team_ci_candidate as candidate

SCRIPT = Path(__file__).with_name("ci_windows_packaging_scope.py")
REPOSITORY = "arsenaltj/OrcaSlicer"
PACKAGING_PATHS = (
    "scripts/ci_windows_packaging.ps1",
    "scripts/ci_windows_compare_packages.py",
    "scripts/ci_windows_packaging_scope.py",
    "scripts/test_ci_windows_packaging.py",
    "scripts/test_ci_windows_compare_packages.py",
    "scripts/test_ci_windows_packaging_scope.py",
    "scripts/test_ci_windows_packaging_workflow.py",
    "scripts/msix/build_msix.ps1",
    "scripts/msix/nested/manifest.xml",
    "scripts/build_preset_cache.bat",
    "build_release_vs.bat",
    "CMakeLists.txt",
    "cmake/NSIS.template.in",
    "cmake/packaging/windows.cmake",
    ".github/workflows/build_orca.yml",
    ".github/workflows/build_deps.yml",
    ".github/workflows/build_check_cache.yml",
    ".github/workflows/team-integration-candidate.yml",
    "scripts/team_ci_candidate.py",
)


def git(root: Path, *arguments: str) -> str:
    result = subprocess.run(
        ["git", "-c", "core.autocrlf=false", "-c", "core.hooksPath=/dev/null",
         "-c", "user.name=Packaging Scope Test", "-c", "user.email=scope@example.invalid",
         "-C", str(root), *arguments],
        capture_output=True, encoding="utf-8", check=True, timeout=30,
    )
    return result.stdout.strip()


class PackagingPathTests(unittest.TestCase):
    def test_explicit_packaging_paths_match_and_are_sorted(self):
        self.assertEqual(packaging.packaging_paths(list(reversed(PACKAGING_PATHS))), sorted(PACKAGING_PATHS))

    def test_unrelated_and_similarly_named_paths_do_not_match(self):
        paths = ["docs/notes.md", "src/slic3r/GUI/MainFrame.cpp", "scripts/unrelated.py",
                 "scripts/ci_windows_packaging.ps1.bak", "scripts/msix-copy/build_msix.ps1",
                 "cmake-copy/NSIS.template.in",
                 ".github/workflows/check_locale.yml", "build_linux.sh"]
        self.assertEqual(packaging.packaging_paths(paths), [])

    def test_every_packaging_path_triggers_the_existing_windows_job(self):
        for path in PACKAGING_PATHS:
            with self.subTest(path=path):
                self.assertTrue(candidate.scope([path])["native"], f"Windows would skip {path}")
                if path.startswith("cmake/"):
                    self.assertTrue(candidate.scope([path])["cross_platform"], f"Existing cross-platform scope changed for {path}")
        self.assertFalse(candidate.scope(["docs/notes.md"])["native"])


class PackagingSelectionTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="packaging scope ")
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.root = self.directory / "source"
        self.root.mkdir()
        git(self.root, "init", "--template=", "-q", "--initial-branch=fixture-main")
        (self.root / "scripts").mkdir()
        (self.root / "scripts/ci_windows_packaging.ps1").write_text("base\n", encoding="utf-8")
        (self.root / "README.md").write_text("base\n", encoding="utf-8")
        git(self.root, "add", ".")
        git(self.root, "commit", "-qm", "base")
        self.base = git(self.root, "rev-parse", "HEAD")
        git(self.root, "checkout", "-qb", "fixture-topic")
        (self.root / "scripts/ci_windows_packaging.ps1").write_text("candidate\n", encoding="utf-8")
        git(self.root, "commit", "-qam", "packaging change")
        self.head = git(self.root, "rev-parse", "HEAD")
        git(self.root, "checkout", "-q", "fixture-main")
        git(self.root, "merge", "-q", "--no-ff", "--no-edit", self.head)
        self.merge = git(self.root, "rev-parse", "HEAD")
        self.event = {
            "number": 12,
            "repository": {"full_name": REPOSITORY},
            "pull_request": {
                "number": 12,
                "head": {"sha": self.head, "ref": "codex/team/maintenance", "repo": {"full_name": REPOSITORY}},
                "base": {"sha": self.base, "ref": candidate.BRANCH, "repo": {"full_name": REPOSITORY}},
                "merge_commit_sha": self.merge,
            },
        }

    def select(self, *, root=None, event=None, event_name="pull_request", repository=REPOSITORY,
               expected_sha=None, ref="refs/pull/12/merge", workflow_name="Team integration candidate"):
        # The selector may reuse inspect(), but must never invoke its API path.
        with mock.patch.object(candidate, "api_get", side_effect=AssertionError("network is outside selector scope")):
            return packaging.select(root or self.root, self.event if event is None else event,
                                    event_name, repository, self.merge if expected_sha is None else expected_sha,
                                    ref, workflow_name=workflow_name)

    def shallow_checkout(self, depth):
        checkout = self.directory / f"shallow-{depth}"
        checkout.mkdir()
        git(checkout, "init", "--template=", "-q")
        # file:// enforces real shallow fetch semantics; no network is used.
        git(checkout, "-c", "protocol.file.allow=always", "fetch", "-q", f"--depth={depth}", self.root.as_uri(), self.merge)
        git(checkout, "checkout", "-q", "--detach", "FETCH_HEAD")
        self.assertEqual(git(checkout, "rev-parse", "--is-shallow-repository"), "true")
        return checkout

    def test_exact_merge_with_packaging_change_selects_paired(self):
        report = self.select()
        self.assertIs(report["paired"], True)
        self.assertEqual(report["matched_paths"], ["scripts/ci_windows_packaging.ps1"])
        self.assertEqual(report["candidate_sha"], self.merge)
        self.assertEqual(report["base_sha"], self.base)
        self.assertEqual(report["head_sha"], self.head)

    def test_real_depth_two_fetch_contains_the_required_merge_parents(self):
        checkout = self.shallow_checkout(2)
        report = self.select(root=checkout)
        self.assertIs(report["paired"], True)
        self.assertEqual(report["matched_paths"], ["scripts/ci_windows_packaging.ps1"])
        self.assertEqual(report["candidate_sha"], self.merge)

    def test_depth_one_checkout_fails_instead_of_skipping_comparison(self):
        checkout = self.shallow_checkout(1)
        with self.assertRaises((ValueError, subprocess.CalledProcessError)):
            self.select(root=checkout)

    def test_missing_wrong_and_reversed_parent_identities_fail(self):
        for changes in ({"head": self.base}, {"base": self.head}, {"base": self.head, "head": self.base}):
            with self.subTest(changes=changes):
                event = copy.deepcopy(self.event)
                for part, value in changes.items():
                    event["pull_request"][part]["sha"] = value
                with self.assertRaises(ValueError):
                    self.select(event=event)
        git(self.root, "checkout", "-q", "--detach", self.head)
        with self.assertRaises(ValueError):
            self.select(expected_sha=self.head)

    def test_target_pr_head_must_equal_github_sha(self):
        with self.assertRaises(ValueError):
            self.select(expected_sha=self.base)

    def test_other_repository_and_caller_skip_without_validating_checkout(self):
        for options in ({"repository": "another/repository"}, {"workflow_name": "Build all"},
                        {"workflow_name": ""}, {"event_name": "push"}):
            with self.subTest(options=options), mock.patch.object(candidate, "git", side_effect=AssertionError("unrelated caller must not inspect the checkout")):
                report = self.select(root=self.directory / "does-not-exist", event={}, expected_sha="not-a-sha", **options)
                self.assertEqual(report["state"], "not_applicable")
                self.assertIs(report["paired"], False)

    def test_pr_ref_number_and_repository_identity_must_match(self):
        for ref in ("refs/pull/13/merge", "refs/pull/12/head", "refs/heads/codex/team/maintenance"):
            with self.subTest(ref=ref), self.assertRaises(ValueError):
                self.select(ref=ref)
        for location in ("repository", "base"):
            with self.subTest(location=location):
                event = copy.deepcopy(self.event)
                target = event["repository"] if location == "repository" else event["pull_request"]["base"]["repo"]
                target["full_name"] = "another/repository"
                with self.assertRaises(ValueError):
                    self.select(event=event)

    def test_malformed_pr_never_silently_disables_comparison(self):
        variants = []
        for key in ("pull_request", "number"):
            event = copy.deepcopy(self.event)
            del event[key]
            variants.append(event)
        for key in ("base", "head"):
            event = copy.deepcopy(self.event)
            del event["pull_request"][key]
            variants.append(event)
        event = copy.deepcopy(self.event)
        event["pull_request"]["base"].pop("ref")
        variants.append(event)
        for ref in (None, [], 42):
            event = copy.deepcopy(self.event)
            event["pull_request"]["base"]["ref"] = ref
            variants.append(event)
        for event in variants:
            with self.subTest(event=event), self.assertRaises((ValueError, KeyError, TypeError)):
                self.select(event=event)

    def test_nonintegration_pr_and_nonpr_events_are_optimized_only(self):
        event = copy.deepcopy(self.event)
        event["pull_request"]["base"]["ref"] = "main"
        report = self.select(event=event)
        self.assertEqual(report["state"], "not_applicable")
        self.assertIs(report["paired"], False)
        self.assertEqual(report["matched_paths"], [])
        for event_name in ("push", "merge_group", "workflow_dispatch"):
            with self.subTest(event_name=event_name):
                report = self.select(event={"repository": {"full_name": REPOSITORY}}, event_name=event_name,
                                     ref="refs/heads/codex/team/integration", expected_sha="not-a-sha")
                self.assertEqual(report["state"], "not_applicable")
                self.assertIs(report["paired"], False)
                self.assertEqual(report["matched_paths"], [])

    def test_unrelated_integration_pr_is_optimized_only(self):
        git(self.root, "checkout", "-q", "--detach", self.base)
        (self.root / "README.md").write_text("documentation only\n", encoding="utf-8")
        git(self.root, "commit", "-qam", "documentation")
        head = git(self.root, "rev-parse", "HEAD")
        git(self.root, "checkout", "-q", "--detach", self.base)
        git(self.root, "merge", "-q", "--no-ff", "--no-edit", head)
        merge = git(self.root, "rev-parse", "HEAD")
        event = copy.deepcopy(self.event)
        event["pull_request"]["head"]["sha"] = head
        event["pull_request"]["merge_commit_sha"] = merge
        report = self.select(event=event, expected_sha=merge)
        self.assertIs(report["paired"], False)
        self.assertEqual(report["matched_paths"], [])

    def test_rename_out_of_packaging_scope_still_selects_comparison(self):
        git(self.root, "checkout", "-q", "--detach", self.base)
        (self.root / "docs").mkdir()
        git(self.root, "mv", "scripts/ci_windows_packaging.ps1", "docs/old-packaging.txt")
        git(self.root, "commit", "-qm", "rename packaging source")
        head = git(self.root, "rev-parse", "HEAD")
        git(self.root, "checkout", "-q", "--detach", self.base)
        git(self.root, "merge", "-q", "--no-ff", "--no-edit", head)
        merge = git(self.root, "rev-parse", "HEAD")
        event = copy.deepcopy(self.event)
        event["pull_request"]["head"]["sha"] = head
        event["pull_request"]["merge_commit_sha"] = merge
        report = self.select(event=event, expected_sha=merge)
        self.assertIs(report["paired"], True)
        self.assertIn("scripts/ci_windows_packaging.ps1", report["matched_paths"])

    def run_cli(self, *, expected_sha=None, event_json=None, workflow_name="Team integration candidate"):
        event_path = self.directory / "event.json"
        event_path.write_text(json.dumps(self.event) if event_json is None else event_json, encoding="utf-8")
        report_path = self.directory / "decision.json"
        output_path = self.directory / "github-output.txt"
        env = {key: value for key, value in os.environ.items() if not key.startswith("GITHUB_")}
        env.update({
            "GITHUB_EVENT_PATH": str(event_path), "GITHUB_EVENT_NAME": "pull_request",
            "GITHUB_REPOSITORY": REPOSITORY, "GITHUB_SHA": expected_sha or self.merge,
            "GITHUB_REF": "refs/pull/12/merge", "GITHUB_OUTPUT": str(output_path),
            "PYTHONDONTWRITEBYTECODE": "1", "PYTHONUTF8": "1",
        })
        if workflow_name is not None:
            env["GITHUB_WORKFLOW"] = workflow_name
        result = subprocess.run([sys.executable, "-B", str(SCRIPT), "--report", str(report_path)],
                                cwd=self.root, env=env, capture_output=True, encoding="utf-8", check=False, timeout=30)
        self.assertTrue(report_path.is_file(), result.stdout + result.stderr)
        report = json.loads(report_path.read_text(encoding="utf-8-sig"))
        output = output_path.read_text(encoding="utf-8") if output_path.is_file() else ""
        return result, report, output

    def test_cli_publishes_selection_only_after_identity_validation(self):
        result, report, output = self.run_cli()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIs(report["paired"], True)
        self.assertIn("paired=true", output.splitlines())

    def test_cli_failure_retains_report_without_success_output(self):
        result, report, output = self.run_cli(expected_sha=self.base)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(report["state"], "failed")
        self.assertNotIn("paired=", output)

    def test_cli_missing_workflow_retains_a_failure_report(self):
        result, report, output = self.run_cli(workflow_name=None, expected_sha="not-a-sha")
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(report["state"], "failed")
        self.assertNotIn("paired=", output)

    def test_cli_malformed_event_preserves_a_failure_report(self):
        for event_json in ("null", "[]", '{"repository": null}', "{broken-json"):
            with self.subTest(event_json=event_json):
                (self.directory / "decision.json").unlink(missing_ok=True)
                result, report, output = self.run_cli(event_json=event_json)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(report["state"], "failed")
                self.assertNotIn("paired=", output)

    def test_cli_output_write_failure_is_recorded_as_failed(self):
        (self.directory / "github-output.txt").mkdir()
        result, report, output = self.run_cli()
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(report["state"], "failed")
        self.assertIsNone(report["paired"])
        self.assertEqual(output, "")


if __name__ == "__main__":
    unittest.main()
