#!/usr/bin/env python3
"""Resumable blind benchmark for the printable model-generation pipeline."""

from __future__ import annotations

import argparse
import csv
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import re
import sys
import time
from typing import Any, Callable, Iterable, Mapping


TOOLS_AI = Path(__file__).resolve().parent
if str(TOOLS_AI) not in sys.path:
    sys.path.insert(0, str(TOOLS_AI))

from openai_preprocessor import STYLE_PROFILES, generate_image  # noqa: E402
from printable_image_pipeline import PrintSettings, process_printable_image  # noqa: E402
from printable_model_quality import analyze_printable_obj  # noqa: E402
from printable_palette import normalize_palette  # noqa: E402
from printable_visual_quality import review_model_visual_quality  # noqa: E402
import run_paid_tripo_validation  # noqa: E402


SCHEMA_VERSION = 1
CASE_ID = re.compile(r"^[a-z0-9][a-z0-9_-]{0,63}$")
HUMAN_DIMENSIONS = (
    "prompt_fidelity",
    "subject_completeness",
    "semantic_coherence",
    "silhouette_readability",
    "base_or_grounding",
    "color_region_clarity",
    "print_readiness",
)
DEFAULT_MANIFEST = Path("Docs/benchmarks/model-generation-blind-pilot-v1.json")
DEFAULT_OUTPUT = Path("generated_models/model-blind-benchmark-phase49")


class BlindBenchmarkError(RuntimeError):
    pass


@dataclass(frozen=True)
class BlindCase:
    case_id: str
    category: str
    style: str
    prompt: str
    palette: tuple[str, ...]


@dataclass(frozen=True)
class BlindManifest:
    benchmark_id: str
    frozen_at: str
    face_limit: int
    fingerprint: str
    cases: tuple[BlindCase, ...]


def _canonical_sha(value: object) -> str:
    payload = json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":")).encode("utf-8")
    return hashlib.sha256(payload).hexdigest().upper()


def _read_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise BlindBenchmarkError(f"Cannot read JSON: {path}") from exc
    if not isinstance(value, dict):
        raise BlindBenchmarkError(f"JSON root must be an object: {path}")
    return value


def _write_json(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".part")
    temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2), encoding="utf-8")
    temporary.replace(path)


def load_manifest(path: Path | str) -> BlindManifest:
    source = Path(path).resolve()
    raw = _read_json(source)
    if raw.get("schema_version") != SCHEMA_VERSION:
        raise BlindBenchmarkError("Unsupported blind benchmark schema version.")
    benchmark_id = raw.get("benchmark_id")
    frozen_at = raw.get("frozen_at")
    face_limit = raw.get("face_limit")
    items = raw.get("cases")
    if not isinstance(benchmark_id, str) or not CASE_ID.fullmatch(benchmark_id):
        raise BlindBenchmarkError("benchmark_id must be a safe lowercase identifier.")
    if not isinstance(frozen_at, str) or not frozen_at.strip():
        raise BlindBenchmarkError("frozen_at is required.")
    if face_limit not in run_paid_tripo_validation.FACE_LIMITS:
        raise BlindBenchmarkError("face_limit must be supported by the paid Tripo runner.")
    if not isinstance(items, list) or not 1 <= len(items) <= 100:
        raise BlindBenchmarkError("cases must contain between 1 and 100 entries.")
    seen: set[str] = set()
    cases: list[BlindCase] = []
    for item in items:
        if not isinstance(item, dict):
            raise BlindBenchmarkError("Each case must be an object.")
        case_id = item.get("id")
        category = item.get("category")
        style = item.get("style")
        prompt = item.get("prompt")
        if not isinstance(case_id, str) or not CASE_ID.fullmatch(case_id) or case_id in seen:
            raise BlindBenchmarkError("Case IDs must be unique safe lowercase identifiers.")
        if not isinstance(category, str) or not category.strip():
            raise BlindBenchmarkError(f"Case {case_id} requires a category.")
        if style not in STYLE_PROFILES:
            raise BlindBenchmarkError(f"Case {case_id} uses an unsupported style.")
        if not isinstance(prompt, str) or not prompt.strip():
            raise BlindBenchmarkError(f"Case {case_id} requires a prompt.")
        try:
            palette = normalize_palette(item.get("palette", ()))
        except ValueError as exc:
            raise BlindBenchmarkError(f"Case {case_id} has an invalid palette: {exc}") from None
        if len(palette) != 4:
            raise BlindBenchmarkError(f"Case {case_id} must use exactly four colors.")
        seen.add(case_id)
        cases.append(BlindCase(case_id, category.strip(), style, prompt.strip(), palette))
    return BlindManifest(benchmark_id, frozen_at.strip(), face_limit, _canonical_sha(raw), tuple(cases))


def _new_state(manifest: BlindManifest, case: BlindCase) -> dict[str, Any]:
    return {
        "schema_version": SCHEMA_VERSION,
        "benchmark_id": manifest.benchmark_id,
        "manifest_fingerprint": manifest.fingerprint,
        "case_id": case.case_id,
        "category": case.category,
        "style": case.style,
        "prompt": case.prompt,
        "palette": list(case.palette),
        "face_limit": manifest.face_limit,
        "stages": {
            "reference": {"status": "pending"},
            "tripo": {"status": "pending"},
            "structural": {"status": "pending"},
            "visual": {"status": "pending"},
        },
        "artifacts": {},
        "paid_calls": {"image2": 0, "tripo_generation": 0, "tripo_conversion": 0, "visual_review": 0},
        "updated_at": time.time(),
    }


def load_case_state(case_directory: Path, manifest: BlindManifest, case: BlindCase) -> dict[str, Any]:
    path = case_directory / "case-state.json"
    if not path.is_file():
        state = _new_state(manifest, case)
        _write_json(path, state)
        return state
    state = _read_json(path)
    if state.get("manifest_fingerprint") != manifest.fingerprint or state.get("case_id") != case.case_id:
        raise BlindBenchmarkError(f"Case directory {case.case_id} belongs to another frozen manifest.")
    return state


def _save_state(case_directory: Path, state: dict[str, Any]) -> None:
    state["updated_at"] = time.time()
    _write_json(case_directory / "case-state.json", state)


def _set_stage(case_directory: Path, state: dict[str, Any], stage: str, status: str, **details: Any) -> None:
    state["stages"][stage] = {"status": status, "updated_at": time.time(), **details}
    _save_state(case_directory, state)


def prepare_reference(
    case_directory: Path,
    manifest: BlindManifest,
    case: BlindCase,
    *,
    confirm_paid_call: bool,
    image_generator: Callable[..., Path] = generate_image,
    processor: Callable[..., Any] = process_printable_image,
) -> dict[str, Any]:
    case_directory.mkdir(parents=True, exist_ok=True)
    state = load_case_state(case_directory, manifest, case)
    reference_directory = case_directory / "reference"
    raw = reference_directory / "ai_raw.png"
    model_reference = reference_directory / "model_reference.png"
    if model_reference.is_file():
        state["artifacts"]["model_reference"] = str(model_reference.resolve())
        previous = state["stages"]["reference"].get("status")
        metrics = state["stages"]["reference"].get("metrics", {})
        metadata_path = reference_directory / "metadata.json"
        if previous not in {"success", "review"} and metadata_path.is_file():
            metrics = _read_json(metadata_path).get("metrics", {})
            previous = "success" if isinstance(metrics, dict) and metrics.get("palette_quality_ok") else "review"
        _set_stage(
            case_directory,
            state,
            "reference",
            previous if previous in {"success", "review"} else "review",
            resumed=True,
            metrics=metrics,
        )
        return state
    reference_directory.mkdir(parents=True, exist_ok=True)
    if not raw.is_file():
        if int(state["paid_calls"].get("image2", 0)) > 0:
            raise BlindBenchmarkError(
                f"Case {case.case_id} has an attempted Image2 call but no saved image; refusing an automatic repeat."
            )
        if not confirm_paid_call:
            raise BlindBenchmarkError("Reference preflight passed; --confirm-image-calls is required for Image2.")
        state["paid_calls"]["image2"] = 1
        _set_stage(case_directory, state, "reference", "running", paid_call_recorded=True)
        try:
            image_generator(case.prompt, raw, case.palette, case.style)
        except Exception as exc:
            _set_stage(case_directory, state, "reference", "failed", error=str(exc)[:500])
            raise
    try:
        result = processor(raw, reference_directory, case.palette, PrintSettings())
    except Exception as exc:
        _set_stage(case_directory, state, "reference", "failed", error=str(exc)[:500])
        raise
    model_reference = Path(result.model_reference)
    if not model_reference.is_file():
        raise BlindBenchmarkError(f"Reference pipeline did not create model_reference.png for {case.case_id}.")
    state["artifacts"].update({
        "ai_raw": str(raw.resolve()),
        "model_reference": str(model_reference.resolve()),
        "image_metadata": str(Path(result.metadata).resolve()),
    })
    quality_ok = bool(result.metrics.get("palette_quality_ok", False))
    _set_stage(
        case_directory,
        state,
        "reference",
        "success" if quality_ok else "review",
        metrics=result.metrics,
        reason="" if quality_ok else "The printable image quality gate requires review.",
    )
    return state


def _find_tripo_artifact(tripo_root: Path) -> Path | None:
    artifacts = sorted(tripo_root.glob("*/model-vertex-color.obj"), key=lambda path: path.stat().st_mtime, reverse=True)
    return artifacts[0] if artifacts else None


def run_tripo(
    case_directory: Path,
    manifest: BlindManifest,
    case: BlindCase,
    *,
    confirm_paid_call: bool,
    runner: Callable[..., Path] = run_paid_tripo_validation.run,
) -> dict[str, Any]:
    state = load_case_state(case_directory, manifest, case)
    reference = Path(str(state["artifacts"].get("model_reference", "")))
    if not reference.is_file():
        raise BlindBenchmarkError(f"Case {case.case_id} has no prepared model reference.")
    if state["stages"]["reference"].get("status") != "success":
        raise BlindBenchmarkError(f"Case {case.case_id} did not pass the printable reference quality gate.")
    tripo_root = case_directory / "tripo"
    task_state_path = tripo_root / "validation-state.json"
    existing_task = task_state_path.is_file()
    if not confirm_paid_call:
        raise BlindBenchmarkError("--confirm-tripo-calls is required to create or resume remote Tripo work.")
    _set_stage(case_directory, state, "tripo", "running", resumed=existing_task)
    error = ""
    artifact: Path | None = None
    try:
        artifact = Path(runner(reference, tripo_root, confirm_paid_call, manifest.face_limit, case.palette))
    except Exception as exc:
        error = str(exc)[:500]
        artifact = _find_tripo_artifact(tripo_root)
    if task_state_path.is_file():
        task_state = _read_json(task_state_path)
        if task_state.get("generation_task_id"):
            state["paid_calls"]["tripo_generation"] = 1
        if task_state.get("conversion_task_id"):
            state["paid_calls"]["tripo_conversion"] = 1
        state["artifacts"]["tripo_state"] = str(task_state_path.resolve())
    if artifact and artifact.is_file():
        state["artifacts"]["model_obj"] = str(artifact.resolve())
        _set_stage(case_directory, state, "tripo", "success" if not error else "review", error=error)
        return state
    _set_stage(case_directory, state, "tripo", "failed", error=error or "Tripo produced no final OBJ.")
    raise BlindBenchmarkError(error or f"Case {case.case_id} produced no final OBJ.")


def review_case(
    case_directory: Path,
    manifest: BlindManifest,
    case: BlindCase,
    *,
    confirm_visual_call: bool,
    visual_reviewer: Callable[..., dict[str, Any]] = review_model_visual_quality,
) -> dict[str, Any]:
    state = load_case_state(case_directory, manifest, case)
    obj = Path(str(state["artifacts"].get("model_obj", "")))
    if not obj.is_file():
        raise BlindBenchmarkError(f"Case {case.case_id} has no final OBJ to review.")
    review_directory = case_directory / "review"
    review_directory.mkdir(parents=True, exist_ok=True)
    structural = analyze_printable_obj(obj, allow_repairable_topology=True)
    structural_path = review_directory / "model-quality.json"
    _write_json(structural_path, structural)
    state["artifacts"]["model_quality"] = str(structural_path.resolve())
    _set_stage(case_directory, state, "structural", str(structural.get("status", "reject")))
    if not confirm_visual_call:
        _set_stage(case_directory, state, "visual", "pending", reason="--confirm-visual-calls was not supplied")
        return state
    visual_path = review_directory / "visual-quality.json"
    already_reviewed = False
    if visual_path.is_file():
        existing_visual = _read_json(visual_path)
        if existing_visual.get("status") in {"pass", "review"}:
            already_reviewed = True
        elif int(state["paid_calls"].get("visual_review", 0)) > 0:
            state["artifacts"]["visual_quality"] = str(visual_path.resolve())
            _set_stage(
                case_directory,
                state,
                "visual",
                str(existing_visual.get("status", "unavailable")),
                resumed=True,
                reason="Previous paid visual attempt is retained; automatic repeat refused.",
            )
            return state
    elif int(state["paid_calls"].get("visual_review", 0)) > 0:
        raise BlindBenchmarkError(
            f"Case {case.case_id} has an attempted visual call but no saved report; refusing an automatic repeat."
        )
    if not already_reviewed:
        state["paid_calls"]["visual_review"] = 1
        _save_state(case_directory, state)
    reference = Path(str(state["artifacts"].get("model_reference", "")))
    visual = visual_reviewer(
        obj,
        review_directory,
        description=case.prompt,
        style=case.style,
        reference_path=reference if reference.is_file() else None,
    )
    state["artifacts"]["visual_quality"] = str(visual_path.resolve())
    _set_stage(case_directory, state, "visual", str(visual.get("status", "unavailable")), resumed=already_reviewed)
    return state


def ensure_human_review(case_directory: Path) -> dict[str, Any]:
    path = case_directory / "human-review.json"
    if path.is_file():
        return _read_json(path)
    value = {
        "status": "pending",
        "scores": {dimension: None for dimension in HUMAN_DIMENSIONS},
        "critical_defects": [],
        "import_to_orca": None,
        "notes": "",
    }
    _write_json(path, value)
    return value


def _report_status(path_value: object) -> str:
    if not isinstance(path_value, str) or not Path(path_value).is_file():
        return "pending"
    try:
        return str(_read_json(Path(path_value)).get("status", "pending"))
    except BlindBenchmarkError:
        return "invalid"


def collect_results(output_root: Path, manifest: BlindManifest) -> dict[str, Any]:
    rows: list[dict[str, Any]] = []
    totals = {"image2": 0, "tripo_generation": 0, "tripo_conversion": 0, "visual_review": 0}
    agreement_count = 0
    comparable_count = 0
    for case in manifest.cases:
        directory = output_root / case.case_id
        state = load_case_state(directory, manifest, case)
        human = ensure_human_review(directory)
        structural = _report_status(state["artifacts"].get("model_quality"))
        visual = _report_status(state["artifacts"].get("visual_quality"))
        automatic = "fail" if structural == "reject" else "review" if structural == "review" or visual == "review" else (
            "pass" if structural == "pass" and visual == "pass" else "pending"
        )
        human_status = str(human.get("status", "pending"))
        agreement: bool | None = None
        if automatic in {"pass", "review", "fail"} and human_status in {"pass", "review", "fail"}:
            agreement = automatic == human_status
            comparable_count += 1
            agreement_count += int(agreement)
        paid = state.get("paid_calls", {})
        for key in totals:
            totals[key] += int(paid.get(key, 0))
        rows.append({
            "case_id": case.case_id,
            "category": case.category,
            "reference_status": state["stages"]["reference"]["status"],
            "tripo_status": state["stages"]["tripo"]["status"],
            "structural_status": structural,
            "visual_status": visual,
            "automatic_status": automatic,
            "human_status": human_status,
            "agreement": agreement,
            "model_obj": state["artifacts"].get("model_obj", ""),
        })
    summary = {
        "schema_version": SCHEMA_VERSION,
        "benchmark_id": manifest.benchmark_id,
        "manifest_fingerprint": manifest.fingerprint,
        "generated_at": time.time(),
        "case_count": len(rows),
        "paid_calls": totals,
        "automatic_counts": {status: sum(row["automatic_status"] == status for row in rows) for status in ("pass", "review", "fail", "pending")},
        "human_comparable_count": comparable_count,
        "exact_agreement_rate": round(agreement_count / comparable_count, 4) if comparable_count else None,
        "cases": rows,
    }
    _write_json(output_root / "benchmark-summary.json", summary)
    with (output_root / "benchmark-summary.csv").open("w", encoding="utf-8-sig", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    return summary


def _selected(cases: Iterable[BlindCase], requested: list[str]) -> tuple[BlindCase, ...]:
    result = tuple(case for case in cases if not requested or case.case_id in requested)
    unknown = set(requested) - {case.case_id for case in cases}
    if unknown:
        raise BlindBenchmarkError("Unknown case IDs: " + ", ".join(sorted(unknown)))
    return result


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--output-root", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--stage", choices=("validate", "reference", "tripo", "review", "collect", "all"), default="validate")
    parser.add_argument("--case", action="append", default=[])
    parser.add_argument("--confirm-image-calls", action="store_true")
    parser.add_argument("--confirm-tripo-calls", action="store_true")
    parser.add_argument("--confirm-visual-calls", action="store_true")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        manifest = load_manifest(args.manifest)
        output_root = args.output_root.resolve()
        output_root.mkdir(parents=True, exist_ok=True)
        _write_json(output_root / "frozen-manifest.json", _read_json(args.manifest.resolve()))
        cases = _selected(manifest.cases, args.case)
        if args.stage == "validate":
            print(json.dumps({"benchmark_id": manifest.benchmark_id, "fingerprint": manifest.fingerprint, "cases": [case.case_id for case in cases]}, ensure_ascii=False, indent=2))
            return 0
        failures: list[str] = []
        for case in cases:
            directory = output_root / case.case_id
            try:
                if args.stage in {"reference", "all"}:
                    prepare_reference(directory, manifest, case, confirm_paid_call=args.confirm_image_calls)
                if args.stage in {"tripo", "all"}:
                    run_tripo(directory, manifest, case, confirm_paid_call=args.confirm_tripo_calls)
                if args.stage in {"review", "all"}:
                    review_case(directory, manifest, case, confirm_visual_call=args.confirm_visual_calls)
            except Exception as exc:
                failures.append(f"{case.case_id}: {exc}")
                print(f"[{case.case_id}] failed: {exc}", file=sys.stderr, flush=True)
        summary = collect_results(output_root, manifest)
        print(json.dumps(summary, ensure_ascii=False, indent=2), flush=True)
        if failures:
            print("Failures:\n" + "\n".join(failures), file=sys.stderr)
            return 1
        return 0
    except (BlindBenchmarkError, ValueError) as exc:
        print(f"Blind benchmark failed: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
