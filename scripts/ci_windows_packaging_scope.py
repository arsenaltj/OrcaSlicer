"""Select paired packaging for changes to the hosted Windows package recipe.

The candidate scheduler and Windows build share this path set. Selection uses
only the event's immutable merge candidate; it never fetches moving branch tips.
"""
from __future__ import annotations

import argparse
import json
import os
import subprocess
from pathlib import Path

PACKAGING_FILES = frozenset({
    "CMakeLists.txt",
    "build_release_vs.bat",
    "scripts/build_preset_cache.bat",
    "scripts/ci_windows_packaging.ps1",
    "scripts/ci_windows_compare_packages.py",
    "scripts/ci_windows_packaging_scope.py",
    "scripts/test_ci_windows_packaging.py",
    "scripts/test_ci_windows_compare_packages.py",
    "scripts/test_ci_windows_packaging_scope.py",
    "scripts/test_ci_windows_packaging_workflow.py",
    "scripts/team_ci_candidate.py",
    ".github/workflows/build_orca.yml",
    ".github/workflows/build_deps.yml",
    ".github/workflows/build_check_cache.yml",
    ".github/workflows/team-integration-candidate.yml",
})
# CPack is configured in the root CMakeLists today; include the module directory
# so newly introduced CPack/NSIS templates cannot bypass package comparison.
PACKAGING_PREFIXES = ("scripts/msix/", "cmake/")


def packaging_paths(paths: list[str]) -> list[str]:
    return sorted({path for path in paths
                   if path in PACKAGING_FILES or path.startswith(PACKAGING_PREFIXES)})


def select(root: Path, event: dict, event_name: str, repository: str,
           expected_sha: str, ref: str,
           workflow_name: str = "Team integration candidate") -> dict:
    # Import lazily: team_ci_candidate uses packaging_paths for its native scope.
    from team_ci_candidate import BRANCH, git, inspect, sha

    report = {"schema_version": 1, "state": "not_applicable", "paired": False,
              "event_name": event_name, "repository": repository,
              "candidate_sha": expected_sha, "ref": ref, "matched_paths": [],
              "workflow_name": workflow_name, "reason": "not_a_team_integration_pull_request"}
    if (event_name != "pull_request" or repository != "arsenaltj/OrcaSlicer"
            or workflow_name != "Team integration candidate"):
        return report
    if not isinstance(event, dict):
        raise TypeError("Expected an event object")
    pr = event["pull_request"]
    if not isinstance(pr, dict) or not isinstance(pr.get("base"), dict):
        raise TypeError("Expected a pull request object with a base object")
    # Other callers keep ordinary optimized packaging. A malformed PR event
    # raises instead of being misclassified as an unrelated change.
    if not isinstance(pr["base"].get("ref"), str):
        raise TypeError("Expected a PR base branch name")
    if pr["base"]["ref"] != BRANCH:
        return report
    if (not isinstance(pr.get("head"), dict)
            or not isinstance(pr["base"].get("repo"), dict)
            or not isinstance(pr["head"].get("ref"), str)):
        raise TypeError("Expected complete PR head and base repository identities")
    if not isinstance(event.get("repository"), dict):
        raise TypeError("Expected a repository object")
    if event["repository"].get("full_name") != repository:
        raise ValueError("Event repository mismatch")
    candidate = sha(git(root, "rev-parse", "HEAD"))
    if candidate != sha(expected_sha):
        raise ValueError("Checkout differs from the event GITHUB_SHA")
    number = event["number"]
    if type(number) is not int or number < 1 or ref != f"refs/pull/{number}/merge":
        raise ValueError("PR merge ref does not match the event number")
    identity = inspect(root, event, event_name, repository)
    paths = git(root, "diff", "--no-renames", "--name-only", "-z",
                identity["base_sha"], candidate).split("\0")
    matched = packaging_paths(paths)
    report.update(state="selected", pr_number=number, head_sha=identity["head_sha"],
                  base_sha=identity["base_sha"], paired=bool(matched),
                  matched_paths=matched,
                  reason="packaging_changed" if matched else "packaging_unchanged")
    return report


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--repo-root", type=Path, default=Path.cwd())
    args = parser.parse_args()
    report = {"schema_version": 1, "state": "failed", "paired": None,
              "event_name": os.environ.get("GITHUB_EVENT_NAME"),
              "candidate_sha": os.environ.get("GITHUB_SHA"),
              "ref": os.environ.get("GITHUB_REF")}
    try:
        event = json.loads(Path(os.environ["GITHUB_EVENT_PATH"]).read_text(encoding="utf-8"))
        if not isinstance(event, dict):
            raise TypeError("Expected an event object")
        report["repository"] = os.environ.get("GITHUB_REPOSITORY")
        if isinstance(event.get("pull_request"), dict):
            pr = event["pull_request"]
            report["head_sha"] = pr["head"].get("sha") if isinstance(pr.get("head"), dict) else None
            report["base_sha"] = pr["base"].get("sha") if isinstance(pr.get("base"), dict) else None
            report["pr_number"] = event.get("number")
        report = select(args.repo_root, event, os.environ["GITHUB_EVENT_NAME"],
                        os.environ["GITHUB_REPOSITORY"], os.environ["GITHUB_SHA"],
                        os.environ["GITHUB_REF"], os.environ["GITHUB_WORKFLOW"])
    except (KeyError, TypeError, ValueError, OSError, subprocess.SubprocessError) as exc:
        report["error"] = {"type": type(exc).__name__, "message": str(exc)}
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    if report["state"] == "failed":
        print("Windows packaging selection failed; see packaging-scope.json")
        return 1
    try:
        with Path(os.environ["GITHUB_OUTPUT"]).open("a", encoding="utf-8") as output:
            output.write(f"paired={str(report['paired']).lower()}\n")
    except (KeyError, OSError) as exc:
        report.update(state="failed", paired=None,
                      error={"type": type(exc).__name__, "message": str(exc)})
        args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        return 1
    print(json.dumps(report))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
