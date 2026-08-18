#!/usr/bin/env python3
"""Prepare and run one resumable Image2-to-Tripo multiview validation."""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import shutil
import sys
from typing import Any, Callable, Mapping


TOOLS_AI = Path(__file__).resolve().parent
if str(TOOLS_AI) not in sys.path:
    sys.path.insert(0, str(TOOLS_AI))

from printable_image_pipeline import PrintSettings  # noqa: E402
from printable_model_quality import analyze_printable_obj  # noqa: E402
from openai_preprocessor import complete_vision, edit_image  # noqa: E402
from printable_multiview_reference import (  # noqa: E402
    VIEW_ORDER,
    create_multiview_sheet,
    process_multiview_crops,
    review_multiview_sheet,
    split_multiview_sheet,
    write_multiview_manifest,
)
from printable_palette import normalize_palette  # noqa: E402
from printable_visual_quality import REVIEW_VERSION, review_model_visual_quality  # noqa: E402
import run_paid_tripo_validation as paid_tripo  # noqa: E402
import tripo_client  # noqa: E402


SCHEMA_VERSION = 1


class MultiviewValidationError(RuntimeError):
    pass


def _create_paid_sheet(source: Path | str, output: Path | str, description: str, palette: tuple[str, ...]) -> Path:
    return create_multiview_sheet(source, output, description, palette, editor=edit_image)


def _review_paid_sheet(
    sheet: Path | str,
    description: str,
    *,
    source_path: Path | str | None = None,
) -> dict[str, Any]:
    return review_multiview_sheet(
        sheet,
        description,
        source_path=source_path,
        completion=complete_vision,
    )


def _read_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise MultiviewValidationError(f"Cannot read state: {path}") from exc
    if not isinstance(value, dict):
        raise MultiviewValidationError(f"State must be an object: {path}")
    return value


def _write_json(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".part")
    temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2), encoding="utf-8")
    temporary.replace(path)


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest().upper()


def load_baseline(case_directory: Path | str) -> dict[str, Any]:
    directory = Path(case_directory).resolve()
    state = _read_json(directory / "case-state.json")
    prompt = state.get("prompt")
    palette_value = state.get("palette")
    artifacts = state.get("artifacts")
    if not isinstance(prompt, str) or not prompt.strip():
        raise MultiviewValidationError("The baseline case has no prompt.")
    if not isinstance(artifacts, dict):
        raise MultiviewValidationError("The baseline case has no artifacts.")
    reference_value = artifacts.get("model_reference")
    if not isinstance(reference_value, str):
        raise MultiviewValidationError("The baseline case has no model reference.")
    reference = Path(reference_value).resolve()
    try:
        reference.relative_to(directory)
    except ValueError:
        raise MultiviewValidationError("The baseline reference must stay inside its case directory.") from None
    if not reference.is_file():
        raise MultiviewValidationError("The baseline model reference does not exist.")
    try:
        palette = normalize_palette(palette_value or ())
    except ValueError as exc:
        raise MultiviewValidationError(str(exc)) from None
    if len(palette) != 4:
        raise MultiviewValidationError("The baseline case must use exactly four printable colors.")
    return {
        "case_id": str(state.get("case_id", directory.name)),
        "prompt": prompt.strip(),
        "style": str(state.get("style", "")),
        "palette": palette,
        "reference": reference,
        "reference_sha256": _sha256(reference),
    }


def _fingerprint(baseline: Mapping[str, Any], face_limit: int) -> str:
    value = {
        "case_id": baseline["case_id"],
        "prompt": baseline["prompt"],
        "palette": list(baseline["palette"]),
        "reference_sha256": baseline["reference_sha256"],
        "face_limit": face_limit,
    }
    payload = json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":")).encode("utf-8")
    return hashlib.sha256(payload).hexdigest().upper()


def load_or_create_state(output_root: Path, baseline: Mapping[str, Any], face_limit: int) -> dict[str, Any]:
    path = output_root / "multiview-state.json"
    fingerprint = _fingerprint(baseline, face_limit)
    if path.is_file():
        state = _read_json(path)
        if state.get("fingerprint") != fingerprint:
            raise MultiviewValidationError("The output directory belongs to another frozen multiview request.")
        return state
    state = {
        "schema_version": SCHEMA_VERSION,
        "fingerprint": fingerprint,
        "case_id": baseline["case_id"],
        "prompt": baseline["prompt"],
        "palette": list(baseline["palette"]),
        "reference": str(baseline["reference"]),
        "reference_sha256": baseline["reference_sha256"],
        "face_limit": face_limit,
        "image2_calls": 0,
        "visual_calls": 0,
        "sheet_generation_guidance": "",
        "prepare_status": "pending",
        "generation_task_id": None,
        "generation_status": "not_submitted",
        "conversion_task_id": None,
        "conversion_status": "not_submitted",
        "artifact": "",
    }
    _write_json(path, state)
    return state


def prepare_multiview(
    baseline_case_directory: Path | str,
    output_root: Path | str,
    *,
    face_limit: int = 300000,
    confirm_image_call: bool = False,
    confirm_visual_call: bool = False,
    allow_repeat_image_call: bool = False,
    allow_repeat_visual_call: bool = False,
    regenerate_sheet: bool = False,
    rerun_visual_review: bool = False,
    generation_guidance: str = "",
    sheet_creator: Callable[..., Path] = _create_paid_sheet,
    sheet_reviewer: Callable[..., dict[str, Any]] = _review_paid_sheet,
) -> dict[str, Any]:
    baseline = load_baseline(baseline_case_directory)
    output = Path(output_root).resolve()
    output.mkdir(parents=True, exist_ok=True)
    state_path = output / "multiview-state.json"
    state = load_or_create_state(output, baseline, face_limit)
    sheet = output / "multiview-sheet.png"
    requested_guidance = generation_guidance.strip()[:1000]
    stored_guidance = str(state.get("sheet_generation_guidance", "")).strip()
    if sheet.is_file() and requested_guidance and requested_guidance != stored_guidance and not regenerate_sheet:
        raise MultiviewValidationError("Changed generation guidance requires --regenerate-sheet.")
    if regenerate_sheet:
        if not allow_repeat_image_call:
            raise MultiviewValidationError("--regenerate-sheet also requires --allow-repeat-image-call.")
        if state.get("generation_task_id"):
            raise MultiviewValidationError("A sheet cannot be regenerated after a Tripo task has been created.")
        if sheet.is_file():
            attempt = output / "attempts" / f"image2-attempt-{int(state['image2_calls']):02d}"
            attempt.mkdir(parents=True, exist_ok=True)
            shutil.copy2(sheet, attempt / "multiview-sheet.png")
            _write_json(attempt / "multiview-state.json", state)
            manifest = output / "multiview-reference.json"
            if manifest.is_file():
                shutil.copy2(manifest, attempt / manifest.name)
        sheet.unlink(missing_ok=True)
        state["visual_review"] = None
        state["manual_view_rejection"] = None
        state["manual_view_approval"] = None
        state["prepare_status"] = "pending"
        _write_json(state_path, state)
    if not sheet.is_file():
        if state["image2_calls"] and not allow_repeat_image_call:
            raise MultiviewValidationError("A previous Image2 attempt has no sheet; automatic repeat is refused.")
        if not confirm_image_call:
            raise MultiviewValidationError("--confirm-image-call is required to create the multiview sheet.")
        effective_guidance = requested_guidance or stored_guidance
        state["sheet_generation_guidance"] = effective_guidance
        state["image2_calls"] = int(state["image2_calls"]) + 1
        state["prepare_status"] = "generating_sheet"
        _write_json(state_path, state)
        effective_description = baseline["prompt"]
        if effective_guidance:
            effective_description += "\nAdditional non-negotiable geometry contract: " + effective_guidance
        sheet_creator(baseline["reference"], sheet, effective_description, baseline["palette"])
    sheet_sha = _sha256(sheet)
    if state.get("sheet_sha256") != sheet_sha:
        state["sheet_sha256"] = sheet_sha
        state["visual_review"] = None
        state["prepare_status"] = "processing_views"
        _write_json(state_path, state)

    crops = split_multiview_sheet(sheet, output / "crops")
    settings = PrintSettings()
    references, metrics = process_multiview_crops(crops, output / "views", baseline["palette"], settings)
    local_ok = all(bool(metrics[view].get("palette_quality_ok", False)) for view in VIEW_ORDER)
    state["views"] = {view: str(references[view].resolve()) for view in VIEW_ORDER}
    state["view_metrics"] = metrics
    if not local_ok:
        state["prepare_status"] = "review"
        state["prepare_reason"] = "At least one view failed the printable image quality gate."
        _write_json(state_path, state)
        return state

    review_value = state.get("visual_review")
    if rerun_visual_review and not allow_repeat_visual_call:
        raise MultiviewValidationError("--rerun-visual-review also requires --allow-repeat-visual-call.")
    review_matches = (
        isinstance(review_value, dict)
        and review_value.get("sheet_sha256") == sheet_sha
        and not rerun_visual_review
    )
    if not review_matches:
        if state["visual_calls"] and not allow_repeat_visual_call:
            raise MultiviewValidationError("A previous visual attempt has no reusable report; automatic repeat is refused.")
        if not confirm_visual_call:
            raise MultiviewValidationError("--confirm-visual-call is required before paid 3D generation.")
        state["visual_calls"] = int(state["visual_calls"]) + 1
        _write_json(state_path, state)
        review_description = baseline["prompt"]
        stored_guidance = str(state.get("sheet_generation_guidance", "")).strip()
        if stored_guidance:
            review_description += "\nAdditional non-negotiable geometry contract: " + stored_guidance
        review = sheet_reviewer(sheet, review_description, source_path=baseline["reference"])
        review["sheet_sha256"] = sheet_sha
        state["visual_review"] = review
    else:
        review = dict(review_value)
    state["prepare_status"] = "pass" if review.get("status") == "pass" else "review"
    state["prepare_reason"] = str(review.get("summary", ""))[:500]
    manifest = write_multiview_manifest(
        output,
        sheet=sheet,
        references=references,
        metrics=metrics,
        review=review,
        palette=baseline["palette"],
        settings=settings,
    )
    state["manifest"] = str(manifest.resolve())
    _write_json(state_path, state)
    return state


def create_or_resume_generation(
    output_root: Path,
    state: dict[str, Any],
    *,
    confirm_paid_call: bool,
    uploader: Callable[[Path], str] = tripo_client.upload_image,
    creator: Callable[[Mapping[str, str], int], str] = tripo_client.create_multiview_task,
) -> tuple[dict[str, Any], Path]:
    state_path = output_root / "multiview-state.json"
    generation_id = state.get("generation_task_id")
    if isinstance(generation_id, str) and generation_id:
        task_directory = paid_tripo._task_directory(output_root / "tripo", generation_id)
        task_directory.mkdir(parents=True, exist_ok=True)
        return state, task_directory
    if not confirm_paid_call:
        raise MultiviewValidationError("--confirm-tripo-call is required to create one paid multiview 3D task.")
    views = state.get("views")
    if not isinstance(views, dict) or set(views) != set(VIEW_ORDER):
        raise MultiviewValidationError("Prepared multiview references are unavailable.")
    tokens = {view: uploader(Path(views[view])) for view in VIEW_ORDER}
    generation_id = creator(tokens, int(state["face_limit"]))
    state["generation_task_id"] = generation_id
    state["generation_status"] = "submitted"
    _write_json(state_path, state)
    task_directory = paid_tripo._task_directory(output_root / "tripo", generation_id)
    task_directory.mkdir(parents=True, exist_ok=True)
    for view in VIEW_ORDER:
        destination = task_directory / f"input-{view}.png"
        if not destination.exists():
            shutil.copy2(Path(views[view]), destination)
    paid_tripo._write_json(task_directory / "validation-state.json", state)
    return state, task_directory


def run_multiview(
    baseline_case_directory: Path | str,
    output_root: Path | str,
    *,
    face_limit: int = 300000,
    confirm_tripo_call: bool = False,
    allow_reviewed_views: bool = False,
    manual_approval_note: str = "",
) -> Path:
    baseline = load_baseline(baseline_case_directory)
    output = Path(output_root).resolve()
    state = load_or_create_state(output, baseline, face_limit)
    rejection = state.get("manual_view_rejection")
    if (
        isinstance(rejection, dict)
        and rejection.get("sheet_sha256") == state.get("sheet_sha256")
    ):
        raise MultiviewValidationError("The current multiview reference was manually rejected.")
    if state.get("prepare_status") == "review" and allow_reviewed_views:
        note = manual_approval_note.strip()
        if not note:
            raise MultiviewValidationError("A non-empty --manual-approval-note is required for reviewed views.")
        review = state.get("visual_review") if isinstance(state.get("visual_review"), dict) else {}
        state["manual_view_approval"] = {
            "approved_at": datetime.now(timezone.utc).isoformat(),
            "note": note[:500],
            "sheet_sha256": state.get("sheet_sha256", ""),
            "visual_score": review.get("score"),
            "visual_warnings": review.get("warnings", []),
        }
        _write_json(output / "multiview-state.json", state)
    elif state.get("prepare_status") != "pass":
        raise MultiviewValidationError("The multiview reference has not passed its consistency gate.")
    state, task_directory = create_or_resume_generation(output, state, confirm_paid_call=confirm_tripo_call)
    state_path = output / "multiview-state.json"
    artifact = paid_tripo.complete_generation(
        state,
        state_path,
        task_directory,
        face_limit,
        baseline["palette"],
    )
    state["artifact"] = str(artifact.resolve())
    _write_json(state_path, state)
    return artifact


def record_manual_rejection(
    baseline_case_directory: Path | str,
    output_root: Path | str,
    *,
    scope: str,
    note: str,
    face_limit: int = 300000,
) -> dict[str, Any]:
    """Persist an auditable human veto for prepared views or a final model."""
    reason = note.strip()
    if not reason:
        raise MultiviewValidationError("A non-empty --manual-rejection-note is required.")
    if scope not in {"views", "result"}:
        raise MultiviewValidationError("Manual rejection scope must be 'views' or 'result'.")
    baseline = load_baseline(baseline_case_directory)
    output = Path(output_root).resolve()
    state_path = output / "multiview-state.json"
    state = load_or_create_state(output, baseline, face_limit)
    rejected_at = datetime.now(timezone.utc).isoformat()
    if scope == "views":
        sheet_sha = str(state.get("sheet_sha256", ""))
        if not sheet_sha:
            raise MultiviewValidationError("No prepared multiview sheet is available to reject.")
        rejection = {
            "rejected_at": rejected_at,
            "note": reason[:500],
            "sheet_sha256": sheet_sha,
        }
        state["manual_view_rejection"] = rejection
        state["manual_view_approval"] = None
        state["prepare_status"] = "rejected"
        state["prepare_reason"] = reason[:500]
    else:
        artifact_value = state.get("artifact")
        if not isinstance(artifact_value, str) or not artifact_value:
            raise MultiviewValidationError("No final multiview artifact is available to reject.")
        artifact = Path(artifact_value).resolve()
        try:
            artifact.relative_to(output)
        except ValueError:
            raise MultiviewValidationError("The rejected artifact must stay inside its output directory.") from None
        if not artifact.is_file():
            raise MultiviewValidationError("The rejected artifact does not exist.")
        rejection = {
            "rejected_at": rejected_at,
            "note": reason[:500],
            "artifact_sha256": _sha256(artifact),
        }
        state["manual_result_rejection"] = rejection
        final_review = state.get("final_review")
        if not isinstance(final_review, dict):
            final_review = {}
        final_review["manual_status"] = "rejected"
        final_review["manual_rejection"] = rejection
        state["final_review"] = final_review
        comparison_path = output / "review" / "comparison.json"
        if comparison_path.is_file():
            comparison = _read_json(comparison_path)
            comparison["manual_outcome"] = "rejected"
            comparison["manual_rejection"] = rejection
            _write_json(comparison_path, comparison)
    _write_json(state_path, state)
    return rejection


def _report_metric(report: Mapping[str, Any], name: str) -> int:
    metrics = report.get("metrics") if isinstance(report.get("metrics"), dict) else {}
    value = metrics.get(name)
    return int(value) if isinstance(value, (int, float)) and not isinstance(value, bool) else 0


def build_baseline_comparison(
    case_id: str,
    baseline_quality: Mapping[str, Any],
    baseline_visual: Mapping[str, Any],
    current_quality: Mapping[str, Any],
    current_visual: Mapping[str, Any],
) -> dict[str, Any]:
    baseline_score = int(baseline_visual.get("score", 0))
    current_score = int(current_visual.get("score", 0))
    score_delta = current_score - baseline_score
    return {
        "schema_version": 1,
        "case_id": case_id,
        "baseline": {
            "quality_status": str(baseline_quality.get("status", "unknown")),
            "visual_status": str(baseline_visual.get("status", "unknown")),
            "visual_score": baseline_score,
            "face_count": _report_metric(baseline_quality, "face_count"),
            "component_count": _report_metric(baseline_quality, "component_count"),
            "tiny_component_count": _report_metric(baseline_quality, "tiny_component_count"),
        },
        "multiview": {
            "quality_status": str(current_quality.get("status", "unknown")),
            "visual_status": str(current_visual.get("status", "unknown")),
            "visual_score": current_score,
            "face_count": _report_metric(current_quality, "face_count"),
            "component_count": _report_metric(current_quality, "component_count"),
            "tiny_component_count": _report_metric(current_quality, "tiny_component_count"),
        },
        "delta": {
            "visual_score": score_delta,
            "face_count": _report_metric(current_quality, "face_count") - _report_metric(baseline_quality, "face_count"),
            "component_count": _report_metric(current_quality, "component_count") - _report_metric(baseline_quality, "component_count"),
            "tiny_component_count": _report_metric(current_quality, "tiny_component_count") - _report_metric(baseline_quality, "tiny_component_count"),
        },
        "outcome": "improved" if score_delta > 0 else "stable" if score_delta == 0 else "regressed",
    }


def _cached_visual_report(report_path: Path, artifact_sha256: str) -> dict[str, Any] | None:
    if not report_path.is_file():
        return None
    report = _read_json(report_path)
    expected_model = os.environ.get("OPENAI_TEXT_MODEL", "gpt-5.4")
    if (
        report.get("status") in {"pass", "review"}
        and report.get("review_version") == REVIEW_VERSION
        and str(report.get("obj_sha256", "")).lower() == artifact_sha256.lower()
        and report.get("model") == expected_model
    ):
        report["cached"] = True
        return report
    return None


def review_multiview_result(
    baseline_case_directory: Path | str,
    output_root: Path | str,
    *,
    face_limit: int = 300000,
    confirm_visual_call: bool = False,
    force_visual_call: bool = False,
    quality_analyzer: Callable[[Path], dict[str, Any]] = analyze_printable_obj,
    visual_reviewer: Callable[..., dict[str, Any]] = review_model_visual_quality,
) -> dict[str, Any]:
    baseline = load_baseline(baseline_case_directory)
    baseline_directory = Path(baseline_case_directory).resolve()
    output = Path(output_root).resolve()
    state_path = output / "multiview-state.json"
    state = load_or_create_state(output, baseline, face_limit)
    artifact_value = state.get("artifact")
    if not isinstance(artifact_value, str) or not artifact_value:
        raise MultiviewValidationError("The final multiview artifact is unavailable.")
    artifact = Path(artifact_value).resolve()
    try:
        artifact.relative_to(output)
    except ValueError:
        raise MultiviewValidationError("The final multiview artifact must stay inside its output directory.") from None
    if not artifact.is_file():
        raise MultiviewValidationError("The final multiview artifact does not exist.")

    baseline_quality = _read_json(baseline_directory / "review" / "model-quality.json")
    baseline_visual = _read_json(baseline_directory / "review" / "visual-quality.json")
    review_directory = output / "review"
    review_directory.mkdir(parents=True, exist_ok=True)
    quality = quality_analyzer(artifact)
    quality_path = review_directory / "model-quality.json"
    _write_json(quality_path, quality)

    visual_path = review_directory / "visual-quality.json"
    artifact_sha = _sha256(artifact)
    cached = None if force_visual_call else _cached_visual_report(visual_path, artifact_sha)
    if cached is None:
        if not confirm_visual_call:
            raise MultiviewValidationError("--confirm-result-visual-call is required for the final model review.")
        visual = visual_reviewer(
            artifact,
            review_directory,
            description=baseline["prompt"],
            style=baseline["style"],
            reference_path=baseline["reference"],
            force=force_visual_call,
        )
    else:
        visual = cached
    if visual.get("status") not in {"pass", "review"}:
        raise MultiviewValidationError("The final visual review is unavailable.")

    comparison = build_baseline_comparison(
        str(baseline["case_id"]), baseline_quality, baseline_visual, quality, visual,
    )
    comparison["generated_at"] = datetime.now(timezone.utc).isoformat()
    manual_rejection = state.get("manual_result_rejection")
    rejection_matches = (
        isinstance(manual_rejection, dict)
        and str(manual_rejection.get("artifact_sha256", "")).lower() == artifact_sha.lower()
    )
    if rejection_matches:
        comparison["manual_outcome"] = "rejected"
        comparison["manual_rejection"] = manual_rejection
    comparison_path = review_directory / "comparison.json"
    _write_json(comparison_path, comparison)
    state["final_review"] = {
        "quality": str(quality_path.resolve()),
        "visual": str(visual_path.resolve()),
        "comparison": str(comparison_path.resolve()),
        "quality_status": quality.get("status", "unknown"),
        "visual_status": visual.get("status", "unknown"),
        "visual_score": visual.get("score", 0),
        "visual_score_delta": comparison["delta"]["visual_score"],
    }
    if rejection_matches:
        state["final_review"]["manual_status"] = "rejected"
        state["final_review"]["manual_rejection"] = manual_rejection
    _write_json(state_path, state)
    return {"quality": quality, "visual": visual, "comparison": comparison}


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline-case-dir", type=Path, required=True)
    parser.add_argument("--output-root", type=Path, required=True)
    parser.add_argument("--stage", choices=("prepare", "tripo", "review", "reject", "all"), default="prepare")
    parser.add_argument("--face-limit", type=int, choices=paid_tripo.FACE_LIMITS, default=300000)
    parser.add_argument("--confirm-image-call", action="store_true")
    parser.add_argument("--confirm-visual-call", action="store_true")
    parser.add_argument("--confirm-tripo-call", action="store_true")
    parser.add_argument("--allow-repeat-image-call", action="store_true")
    parser.add_argument("--allow-repeat-visual-call", action="store_true")
    parser.add_argument("--regenerate-sheet", action="store_true")
    parser.add_argument("--rerun-visual-review", action="store_true")
    parser.add_argument("--generation-guidance", default="")
    parser.add_argument("--allow-reviewed-views", action="store_true")
    parser.add_argument("--manual-approval-note", default="")
    parser.add_argument("--confirm-result-visual-call", action="store_true")
    parser.add_argument("--force-result-visual-call", action="store_true")
    parser.add_argument("--rejection-scope", choices=("views", "result"), default="views")
    parser.add_argument("--manual-rejection-note", default="")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        if args.stage in {"prepare", "all"}:
            state = prepare_multiview(
                args.baseline_case_dir,
                args.output_root,
                face_limit=args.face_limit,
                confirm_image_call=args.confirm_image_call,
                confirm_visual_call=args.confirm_visual_call,
                allow_repeat_image_call=args.allow_repeat_image_call,
                allow_repeat_visual_call=args.allow_repeat_visual_call,
                regenerate_sheet=args.regenerate_sheet,
                rerun_visual_review=args.rerun_visual_review,
                generation_guidance=args.generation_guidance,
            )
            print(json.dumps(state, ensure_ascii=False, indent=2), flush=True)
        if args.stage in {"tripo", "all"}:
            artifact = run_multiview(
                args.baseline_case_dir,
                args.output_root,
                face_limit=args.face_limit,
                confirm_tripo_call=args.confirm_tripo_call,
                allow_reviewed_views=args.allow_reviewed_views,
                manual_approval_note=args.manual_approval_note,
            )
            print(str(artifact), flush=True)
        if args.stage in {"review", "all"}:
            result = review_multiview_result(
                args.baseline_case_dir,
                args.output_root,
                face_limit=args.face_limit,
                confirm_visual_call=args.confirm_result_visual_call,
                force_visual_call=args.force_result_visual_call,
            )
            print(json.dumps(result, ensure_ascii=False, indent=2), flush=True)
        if args.stage == "reject":
            rejection = record_manual_rejection(
                args.baseline_case_dir,
                args.output_root,
                scope=args.rejection_scope,
                note=args.manual_rejection_note,
                face_limit=args.face_limit,
            )
            print(json.dumps(rejection, ensure_ascii=False, indent=2), flush=True)
    except (MultiviewValidationError, RuntimeError, tripo_client.TripoError) as exc:
        print(f"Multiview validation failed: {exc}", file=sys.stderr, flush=True)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
