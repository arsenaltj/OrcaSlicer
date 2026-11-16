"""Run full integration validation and require zero integration errors for every delivery channel."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys

# The packager uses isolated Python; load only modules beside this script.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from package_source_identity import capture
from verify_ai_integration import SCHEMA, validate


def _valid_report(report: object, head: str) -> bool:
    if not isinstance(report, dict):
        return False
    errors = report.get("errors")
    return (
        report.get("schema") == SCHEMA
        and report.get("git_checks_skipped") is False
        and isinstance(report.get("git"), dict)
        and report["git"].get("head") == head
        and isinstance(errors, list)
        and all(isinstance(error, dict) and isinstance(error.get("code"), str)
                and isinstance(error.get("message"), str) for error in errors)
        and report.get("ok") is (not errors)
    )


def packaging_decision(report: object, source: dict, known: object = None, *, channel: str = "release") -> dict:
    """All delivery channels require a fresh, complete, zero-error report."""
    result = {"status": "BLOCKED", "package_allowed": False,
              "integration_passed": False, "known_findings_count": 0}
    if known is not None:
        result["reason"] = "Known integration report exceptions are retired; run the complete integration check."
    elif channel not in ("internal", "release"):
        result["reason"] = "Unknown distribution channel."
    elif not _valid_report(report, source.get("source_commit")):
        result["reason"] = "Integration report is incomplete, inconsistent, or belongs to another source HEAD."
    elif report["ok"]:
        result.update(status="PASSED", package_allowed=True, integration_passed=True)
    else:
        result["reason"] = "All integration findings block delivery, including architecture budget errors."
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--source-manifest", type=Path)
    parser.add_argument("--known-report", type=Path)
    parser.add_argument("--channel", choices=("internal", "release"), default="release")
    args = parser.parse_args()
    if args.known_report is not None:
        parser.error("--known-report is retired; all delivery channels require a complete zero-error check.")
    try:
        root = args.root.resolve()
        source = capture(root, args.source_manifest)
        report = validate(root / "docs/architecture/ai-integration-lock.json", root)
        decision = packaging_decision(report, source, channel=args.channel)
        record = {"schema_version": 1, "source_identity_sha256": source["source_identity_sha256"],
                  "distribution_channel": args.channel, "source_clean": source["source_clean"],
                  "integration_report": report, "decision": decision}
        args.report.write_text(json.dumps(record, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    except (OSError, ValueError, KeyError) as exc:
        parser.exit(1, f"Package integration verification failed: {exc}\n")
    print(f"Package integration check: {decision['status']}; findings: {len(report['errors'])}; report: {args.report}")
    if not decision["package_allowed"]:
        print(decision["reason"], file=sys.stderr)
    return 0 if decision["package_allowed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
