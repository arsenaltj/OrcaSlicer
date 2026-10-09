"""Run full integration validation and record internal-only budget exceptions."""
from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import sys

# The packager uses isolated Python; load only modules beside this script.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from package_source_identity import capture
from verify_ai_integration import validate


def _valid_report(report: object, head: str) -> bool:
    if not isinstance(report, dict):
        return False
    errors = report.get("errors")
    return (
        report.get("schema") == "orcaslicer.ai-integration-lock/v4"
        and report.get("git_checks_skipped") is False
        and isinstance(report.get("git"), dict)
        and report["git"].get("head") == head
        and isinstance(errors, list)
        and all(isinstance(error, dict) and isinstance(error.get("code"), str)
                and isinstance(error.get("message"), str) for error in errors)
        and report.get("ok") is (not errors)
    )


def packaging_decision(report: object, source: dict, known: object = None, *, channel: str = "release") -> dict:
    result = {"status": "BLOCKED", "package_allowed": False,
              "integration_passed": False, "known_findings_count": 0}
    head = source.get("source_commit")
    if not _valid_report(report, head):
        result["reason"] = "Integration report is incomplete, inconsistent, or belongs to another source HEAD."
    elif report["ok"]:
        result.update(status="PASSED", package_allowed=True, integration_passed=True)
    elif channel != "internal":
        result["reason"] = "Known architecture budget exceptions are restricted to explicit internal test packages."
    elif type(source.get("source_clean")) is not bool or not _valid_report(known, head):
        result["reason"] = "Failed integration checks require verified source identity and an explicit matching internal baseline."
    elif not all(error["code"] == "architecture.diff_budget" for error in report["errors"] + known["errors"]):
        result["reason"] = "Only existing architecture diff budget findings can be recorded as an internal package exception."
    elif Counter((error["code"], error["message"]) for error in report["errors"]) != Counter(
            (error["code"], error["message"]) for error in known["errors"]):
        result["reason"] = "Integration findings changed; the recorded baseline does not cover this source."
    else:
        result.update(status="KNOWN_ARCHITECTURE_BUDGET_FINDINGS", package_allowed=True,
                      known_findings_count=len(report["errors"]))
        result["reason"] = "Internal test package only. Integration validation has not passed; all findings remain recorded."
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--source-manifest", type=Path)
    parser.add_argument("--known-report", type=Path)
    parser.add_argument("--channel", choices=("internal", "release"), default="release")
    args = parser.parse_args()
    if args.known_report and not args.source_manifest:
        parser.error("An explicit source snapshot manifest is required with --known-report.")
    try:
        root = args.root.resolve()
        source = capture(root, args.source_manifest)
        report = validate(root / "docs/architecture/ai-integration-lock.json", root)
        known_bytes = args.known_report.read_bytes() if args.known_report else None
        known = json.loads(known_bytes.decode("utf-8-sig")) if known_bytes is not None else None
        decision = packaging_decision(report, source, known, channel=args.channel)
        record = {"schema_version": 1, "source_identity_sha256": source["source_identity_sha256"],
                  "distribution_channel": args.channel, "source_clean": source["source_clean"],
                  "integration_report": report, "decision": decision,
                  "known_report_sha256": hashlib.sha256(known_bytes).hexdigest() if known_bytes is not None else None}
        args.report.write_text(json.dumps(record, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    except (OSError, ValueError, KeyError) as exc:
        parser.exit(1, f"Package integration verification failed: {exc}\n")
    print(f"Package integration check: {decision['status']}; findings: {len(report['errors'])}; report: {args.report}")
    if not decision["package_allowed"]:
        print(decision["reason"], file=sys.stderr)
    return 0 if decision["package_allowed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
