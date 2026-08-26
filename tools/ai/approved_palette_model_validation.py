#!/usr/bin/env python3
"""Run one hash-bound, approved four-color candidate through Tripo and v9 quality checks."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import sys
from typing import Any, Callable

try:
    from .printable_palette import normalize_palette
    from .printable_palette_benchmark import (
        DEFAULT_MANIFEST,
        DEFAULT_OUTPUT,
        PaletteBenchmarkError,
        collect_results,
        load_case_state,
        load_manifest,
        select_cases,
    )
except ImportError:
    from printable_palette import normalize_palette
    from printable_palette_benchmark import (
        DEFAULT_MANIFEST,
        DEFAULT_OUTPUT,
        PaletteBenchmarkError,
        collect_results,
        load_case_state,
        load_manifest,
        select_cases,
    )


DEFAULT_FACE_LIMIT = 500000
FACE_LIMITS = (100000, 300000, 500000, 1000000)
PREFLIGHT_FILENAME = "approved-model-preflight.json"
QUALITY_FILENAME = "final-model-quality.json"
RESULT_FILENAME = "approved-model-validation.json"


class ApprovedModelValidationError(ValueError):
    pass


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest().lower()


def _write_json(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2), encoding="utf-8")
    temporary.replace(path)


def _verified_artifact(case_root: Path, record: object, name: str) -> tuple[Path, str]:
    if not isinstance(record, dict):
        raise ApprovedModelValidationError(f"The approved candidate has no {name} artifact record.")
    relative = record.get("path")
    expected = record.get("sha256")
    if not isinstance(relative, str) or not relative or not isinstance(expected, str) or len(expected) != 64:
        raise ApprovedModelValidationError(f"The approved candidate has an invalid {name} artifact record.")
    root = case_root.resolve()
    path = (root / relative).resolve()
    try:
        path.relative_to(root)
    except ValueError:
        raise ApprovedModelValidationError(f"The {name} artifact escapes the approved case directory.") from None
    if not path.is_file() or _sha256(path) != expected.lower():
        raise ApprovedModelValidationError(f"The {name} artifact no longer matches its approved SHA-256.")
    return path, expected.lower()


def _palette_from_state(state: dict[str, Any]) -> tuple[str, ...]:
    recommendation = state.get("recommendation")
    colors = recommendation.get("colors") if isinstance(recommendation, dict) else None
    if not isinstance(colors, list):
        raise ApprovedModelValidationError("The approved candidate has no printable palette recommendation.")
    values = [entry.get("hex") for entry in colors if isinstance(entry, dict)]
    try:
        palette = normalize_palette(values)
    except ValueError as exc:
        raise ApprovedModelValidationError(f"The approved candidate palette is invalid: {exc}") from None
    if len(palette) != 4:
        raise ApprovedModelValidationError("The approved candidate must contain exactly four target colors.")
    return palette


def preflight_candidate(
    manifest_path: Path | str,
    output_root: Path | str,
    case_id: str,
    *,
    face_limit: int = DEFAULT_FACE_LIMIT,
) -> dict[str, Any]:
    if face_limit not in FACE_LIMITS:
        raise ApprovedModelValidationError("Unsupported Tripo face limit.")
    manifest = load_manifest(manifest_path)
    selected = select_cases(manifest.cases, (case_id,))
    if len(selected) != 1:
        raise ApprovedModelValidationError("Exactly one approved benchmark case is required.")
    case = selected[0]
    output = Path(output_root).resolve()
    case_root = output / case.case_id
    state = load_case_state(case_root, manifest, case)
    summary = collect_results(output, manifest)
    row = next(item for item in summary["cases"] if item["case_id"] == case.case_id)
    if not row.get("tripo_candidate"):
        raise ApprovedModelValidationError(
            "The case is not a hash-bound Tripo candidate; complete explicit manual approval first."
        )

    artifacts = state.get("artifacts") if isinstance(state.get("artifacts"), dict) else {}
    strict_path, strict_sha = _verified_artifact(case_root, artifacts.get("strict_preview"), "strict preview")
    visual_path, visual_sha = _verified_artifact(case_root, artifacts.get("visual_review"), "visual review")
    reference_path, reference_sha = _verified_artifact(case_root, artifacts.get("model_reference"), "model reference")
    manual = state.get("manual_review") if isinstance(state.get("manual_review"), dict) else {}
    if manual.get("strict_preview_sha256", "").lower() != strict_sha:
        raise ApprovedModelValidationError("Manual approval is not bound to the current strict preview.")
    if manual.get("visual_review_sha256", "").lower() != visual_sha:
        raise ApprovedModelValidationError("Manual approval is not bound to the current visual review.")
    palette = _palette_from_state(state)
    return {
        "schema_version": 1,
        "benchmark_id": manifest.benchmark_id,
        "manifest_fingerprint": manifest.fingerprint,
        "case_id": case.case_id,
        "prompt": case.prompt,
        "style": case.style,
        "face_limit": face_limit,
        "palette": list(palette),
        "manual_decision": "approved",
        "manual_note": str(manual.get("note", "")),
        "strict_preview": {"path": str(strict_path), "sha256": strict_sha},
        "visual_review": {"path": str(visual_path), "sha256": visual_sha},
        "model_reference": {"path": str(reference_path), "sha256": reference_sha},
    }


def run_validation(
    manifest_path: Path | str,
    output_root: Path | str,
    case_id: str,
    *,
    confirm_paid_call: bool,
    face_limit: int = DEFAULT_FACE_LIMIT,
    paid_runner: Callable[..., Path] | None = None,
    quality_analyzer: Callable[..., dict[str, Any]] | None = None,
) -> dict[str, Any]:
    preflight = preflight_candidate(manifest_path, output_root, case_id, face_limit=face_limit)
    case_root = Path(output_root).resolve() / case_id
    validation_root = case_root / "tripo"
    _write_json(validation_root / PREFLIGHT_FILENAME, preflight)
    if not confirm_paid_call:
        raise ApprovedModelValidationError(
            "Preflight passed. Explicit paid confirmation is required for this approved case."
        )
    if paid_runner is None:
        module_directory = str(Path(__file__).resolve().parent)
        if module_directory not in sys.path:
            sys.path.insert(0, module_directory)
        from run_paid_tripo_validation import run as paid_runner
    if quality_analyzer is None:
        module_directory = str(Path(__file__).resolve().parent)
        if module_directory not in sys.path:
            sys.path.insert(0, module_directory)
        from printable_model_quality import analyze_printable_obj as quality_analyzer
    palette = tuple(preflight["palette"])
    artifact = Path(paid_runner(
        Path(preflight["model_reference"]["path"]),
        validation_root,
        True,
        face_limit,
        palette,
    )).resolve()
    if not artifact.is_file():
        raise ApprovedModelValidationError("Tripo completed without a final OBJ artifact.")
    quality = quality_analyzer(
        artifact,
        allow_repairable_topology=True,
        target_palette=palette,
    )
    _write_json(validation_root / QUALITY_FILENAME, quality)
    result = {
        **preflight,
        "artifact": {"path": str(artifact), "sha256": _sha256(artifact)},
        "quality_report": {
            "path": str((validation_root / QUALITY_FILENAME).resolve()),
            "sha256": _sha256(validation_root / QUALITY_FILENAME),
            "gate_version": str(quality.get("gate_version", "")),
            "status": str(quality.get("status", "reject")),
        },
        "accepted_for_review": quality.get("status") in {"pass", "review"},
    }
    _write_json(validation_root / RESULT_FILENAME, result)
    return result


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--output-root", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--case", required=True)
    parser.add_argument("--face-limit", type=int, choices=FACE_LIMITS, default=DEFAULT_FACE_LIMIT)
    parser.add_argument("--confirm-paid-call", action="store_true")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        if not args.confirm_paid_call:
            print(json.dumps(
                preflight_candidate(args.manifest, args.output_root, args.case, face_limit=args.face_limit),
                ensure_ascii=False,
                indent=2,
            ))
            return 0
        result = run_validation(
            args.manifest,
            args.output_root,
            args.case,
            confirm_paid_call=True,
            face_limit=args.face_limit,
        )
        print(json.dumps(result, ensure_ascii=False, indent=2))
        return 0 if result["accepted_for_review"] else 1
    except (ApprovedModelValidationError, PaletteBenchmarkError, RuntimeError) as exc:
        print(f"Approved model validation failed: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
