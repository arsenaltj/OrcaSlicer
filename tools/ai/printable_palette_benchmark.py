#!/usr/bin/env python3
"""Resumable real-provider benchmark for printable four-color recommendations."""

from __future__ import annotations

from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path
import re
import time
from typing import Any, Callable, Iterable

try:
    from .openai_preprocessor import (
        STYLE_PROFILES,
        generate_image,
        preprocess_image,
        recommend_printable_palette,
    )
    from .printable_image_pipeline import PrintSettings, process_printable_image
    from .printable_palette_visual_quality import (
        REPORT_FILENAME as PALETTE_VISUAL_REPORT_FILENAME,
        review_printable_palette_visual_quality,
    )
except ImportError:
    from openai_preprocessor import STYLE_PROFILES, generate_image, preprocess_image, recommend_printable_palette
    from printable_image_pipeline import PrintSettings, process_printable_image
    from printable_palette_visual_quality import (
        REPORT_FILENAME as PALETTE_VISUAL_REPORT_FILENAME,
        review_printable_palette_visual_quality,
    )


SCHEMA_VERSION = 1
CASE_ID = re.compile(r"^[a-z0-9][a-z0-9_-]{0,63}$")
MAX_PROMPT_BYTES = 4000
STATE_FILENAME = "palette-case-state.json"


class PaletteBenchmarkError(ValueError):
    pass


@dataclass(frozen=True)
class PaletteBenchmarkCase:
    case_id: str
    category: str
    style: str
    prompt: str
    custom_style: str = ""
    reference_image: Path | None = None


@dataclass(frozen=True)
class PaletteBenchmarkManifest:
    benchmark_id: str
    frozen_at: str
    fingerprint: str
    cases: tuple[PaletteBenchmarkCase, ...]


def _read_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise PaletteBenchmarkError(f"Cannot read benchmark manifest: {path}") from exc
    if not isinstance(value, dict):
        raise PaletteBenchmarkError("Benchmark manifest must be a JSON object.")
    return value


def _bounded_text(value: object, field: str, maximum_bytes: int) -> str:
    if not isinstance(value, str) or not value.strip():
        raise PaletteBenchmarkError(f"Each benchmark case requires a non-empty {field}.")
    text = value.strip()
    if len(text.encode("utf-8")) > maximum_bytes:
        raise PaletteBenchmarkError(f"Benchmark case {field} is too long.")
    return text


def _reference_image(manifest_path: Path, value: object) -> Path | None:
    if value in (None, ""):
        return None
    if not isinstance(value, str):
        raise PaletteBenchmarkError("reference_image must be a relative path string.")
    root = manifest_path.parent.resolve()
    image = (root / value).resolve()
    try:
        image.relative_to(root)
    except ValueError:
        raise PaletteBenchmarkError("reference_image must stay inside the manifest directory.") from None
    if not image.is_file():
        raise PaletteBenchmarkError(f"reference_image does not exist: {value}")
    return image


def _canonical_sha(value: object) -> str:
    encoded = json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":")).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()


def load_manifest(path: Path | str) -> PaletteBenchmarkManifest:
    manifest_path = Path(path).resolve()
    value = _read_json(manifest_path)
    if value.get("schema_version") != SCHEMA_VERSION:
        raise PaletteBenchmarkError(f"Unsupported benchmark schema_version: {value.get('schema_version')}")
    benchmark_id = value.get("benchmark_id")
    if not isinstance(benchmark_id, str) or not CASE_ID.fullmatch(benchmark_id):
        raise PaletteBenchmarkError("benchmark_id must be a safe lowercase identifier.")
    frozen_at = _bounded_text(value.get("frozen_at"), "frozen_at", 64)
    raw_cases = value.get("cases")
    if not isinstance(raw_cases, list) or not raw_cases:
        raise PaletteBenchmarkError("Benchmark manifest requires at least one case.")

    cases: list[PaletteBenchmarkCase] = []
    seen: set[str] = set()
    canonical_cases: list[dict[str, Any]] = []
    for raw in raw_cases:
        if not isinstance(raw, dict):
            raise PaletteBenchmarkError("Each benchmark case must be an object.")
        case_id = raw.get("id")
        if not isinstance(case_id, str) or not CASE_ID.fullmatch(case_id) or case_id in seen:
            raise PaletteBenchmarkError("Benchmark case ids must be unique safe lowercase identifiers.")
        seen.add(case_id)
        category = _bounded_text(raw.get("category"), "category", 128)
        style = raw.get("style")
        if not isinstance(style, str) or style not in STYLE_PROFILES:
            raise PaletteBenchmarkError(f"Unknown benchmark style: {style}")
        prompt = _bounded_text(raw.get("prompt"), "prompt", MAX_PROMPT_BYTES)
        custom_style = str(raw.get("custom_style", "")).strip()
        if custom_style:
            raise PaletteBenchmarkError("custom_style is not supported by the frozen phase 64 benchmark.")
        reference_value = raw.get("reference_image")
        reference = _reference_image(manifest_path, reference_value)
        cases.append(PaletteBenchmarkCase(case_id, category, style, prompt, custom_style, reference))
        canonical_cases.append({
            "id": case_id,
            "category": category,
            "style": style,
            "prompt": prompt,
            "custom_style": custom_style,
            "reference_image": str(reference_value or ""),
        })

    fingerprint = _canonical_sha({
        "schema_version": SCHEMA_VERSION,
        "benchmark_id": benchmark_id,
        "frozen_at": frozen_at,
        "cases": canonical_cases,
    })
    return PaletteBenchmarkManifest(benchmark_id, frozen_at, fingerprint, tuple(cases))


def select_cases(
    cases: Iterable[PaletteBenchmarkCase], requested: Iterable[str]
) -> tuple[PaletteBenchmarkCase, ...]:
    ordered = tuple(cases)
    names = tuple(dict.fromkeys(requested))
    if not names:
        return ordered
    known = {case.case_id for case in ordered}
    unknown = sorted(set(names) - known)
    if unknown:
        raise PaletteBenchmarkError("unknown benchmark case ids: " + ", ".join(unknown))
    selected = set(names)
    return tuple(case for case in ordered if case.case_id in selected)


def _write_json(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".part")
    try:
        temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2), encoding="utf-8")
        os.replace(temporary, path)
    except OSError as exc:
        temporary.unlink(missing_ok=True)
        raise PaletteBenchmarkError(f"Cannot write benchmark state: {path}") from exc


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(chunk)
    except OSError as exc:
        raise PaletteBenchmarkError(f"Cannot hash benchmark artifact: {path}") from exc
    return digest.hexdigest()


def _new_stage() -> dict[str, Any]:
    return {"status": "pending", "attempts": 0, "error": ""}


def _new_state(manifest: PaletteBenchmarkManifest, case: PaletteBenchmarkCase) -> dict[str, Any]:
    now = time.time()
    return {
        "schema_version": SCHEMA_VERSION,
        "benchmark_id": manifest.benchmark_id,
        "manifest_fingerprint": manifest.fingerprint,
        "case_id": case.case_id,
        "category": case.category,
        "style": case.style,
        "prompt": case.prompt,
        "reference_image": str(case.reference_image) if case.reference_image else "",
        "reference_sha256": _sha256(case.reference_image) if case.reference_image else "",
        "stages": {
            "recommendation": _new_stage(),
            "preview": _new_stage(),
            "local_gate": _new_stage(),
            "visual_review": _new_stage(),
            "manual_review": _new_stage(),
        },
        "paid_calls": {"recommendation": 0, "image2": 0, "visual_review": 0, "tripo": 0},
        "recommendation": None,
        "artifacts": {},
        "metrics": {},
        "palette_usage": {},
        "created_at": now,
        "updated_at": now,
    }


def _save_state(case_directory: Path, state: dict[str, Any]) -> None:
    state["updated_at"] = time.time()
    _write_json(case_directory / STATE_FILENAME, state)


def load_case_state(
    case_directory: Path | str,
    manifest: PaletteBenchmarkManifest,
    case: PaletteBenchmarkCase,
) -> dict[str, Any]:
    root = Path(case_directory)
    path = root / STATE_FILENAME
    if not path.is_file():
        state = _new_state(manifest, case)
        _save_state(root, state)
        return state
    state = _read_json(path)
    if state.get("schema_version") != SCHEMA_VERSION:
        raise PaletteBenchmarkError("Unsupported palette benchmark state schema.")
    if state.get("benchmark_id") != manifest.benchmark_id or state.get("case_id") != case.case_id:
        raise PaletteBenchmarkError("Palette benchmark state belongs to another case.")
    if state.get("manifest_fingerprint") != manifest.fingerprint:
        raise PaletteBenchmarkError("Palette benchmark manifest fingerprint changed; use a new output directory.")
    return state


def _artifact_record(root: Path, path: Path) -> dict[str, str]:
    resolved_root = root.resolve()
    resolved = path.resolve()
    try:
        relative = resolved.relative_to(resolved_root).as_posix()
    except ValueError:
        raise PaletteBenchmarkError("Benchmark artifacts must stay inside the case directory.") from None
    if not resolved.is_file():
        raise PaletteBenchmarkError(f"Benchmark artifact does not exist: {relative}")
    return {"path": relative, "sha256": _sha256(resolved)}


def _artifact_valid(root: Path, value: object) -> bool:
    if not isinstance(value, dict):
        return False
    relative = value.get("path")
    expected = value.get("sha256")
    if not isinstance(relative, str) or not isinstance(expected, str):
        return False
    path = (root / relative).resolve()
    try:
        path.relative_to(root.resolve())
    except ValueError:
        return False
    return path.is_file() and _sha256(path) == expected


def _stage_call(
    root: Path,
    state: dict[str, Any],
    stage_name: str,
    paid_name: str,
    confirmed: bool,
    allow_retry_uncertain: bool,
    confirmation_flag: str,
    call: Callable[[], Any],
) -> Any:
    stage = state["stages"][stage_name]
    if stage.get("status") in {"calling", "uncertain"} and not allow_retry_uncertain:
        raise PaletteBenchmarkError(
            f"The {stage_name} paid call is uncertain; inspect its state before an explicit retry."
        )
    if not confirmed:
        raise PaletteBenchmarkError(f"{confirmation_flag} is required before the paid {stage_name} call.")
    stage["status"] = "calling"
    stage["attempts"] = int(stage.get("attempts", 0)) + 1
    stage["error"] = ""
    state["paid_calls"][paid_name] = int(state["paid_calls"].get(paid_name, 0)) + 1
    _save_state(root, state)
    try:
        return call()
    except Exception as exc:
        stage["status"] = "uncertain"
        stage["error"] = f"{type(exc).__name__}: {exc}"[:500]
        _save_state(root, state)
        raise


def _palette_from_state(state: dict[str, Any]) -> tuple[tuple[str, ...], dict[str, str]]:
    value = state.get("recommendation")
    raw_colors = value.get("colors") if isinstance(value, dict) else None
    if not isinstance(raw_colors, list) or len(raw_colors) != 4:
        raise PaletteBenchmarkError("The persisted palette recommendation is incomplete.")
    try:
        colors = tuple(str(item["hex"]) for item in raw_colors)
        roles = {str(item["role"]): str(item["hex"]) for item in raw_colors}
    except (KeyError, TypeError):
        raise PaletteBenchmarkError("The persisted palette recommendation is invalid.") from None
    return colors, roles


def prepare_case(
    case_directory: Path | str,
    manifest: PaletteBenchmarkManifest,
    case: PaletteBenchmarkCase,
    *,
    confirm_recommendation_call: bool = False,
    confirm_image_call: bool = False,
    allow_retry_uncertain: bool = False,
    stop_after: str = "local_gate",
    recommender: Callable[..., Any] = recommend_printable_palette,
    image_generator: Callable[..., Path] = generate_image,
    image_editor: Callable[..., Path] = preprocess_image,
    processor: Callable[..., Any] = process_printable_image,
) -> dict[str, Any]:
    if stop_after not in {"recommendation", "preview", "local_gate"}:
        raise PaletteBenchmarkError(f"Unsupported prepare stop_after stage: {stop_after}")
    root = Path(case_directory).resolve()
    root.mkdir(parents=True, exist_ok=True)
    state = load_case_state(root, manifest, case)

    recommendation_stage = state["stages"]["recommendation"]
    if recommendation_stage.get("status") != "complete" or not isinstance(state.get("recommendation"), dict):
        recommendation = _stage_call(
            root,
            state,
            "recommendation",
            "recommendation",
            confirm_recommendation_call,
            allow_retry_uncertain,
            "--confirm-recommendation-call",
            lambda: recommender(case.prompt, case.style, case.custom_style, case.reference_image),
        )
        if not hasattr(recommendation, "as_dict"):
            raise PaletteBenchmarkError("Palette recommender returned an unsupported result.")
        state["recommendation"] = recommendation.as_dict()
        recommendation_stage["status"] = "complete"
        recommendation_stage["error"] = ""
        recommendation_stage["provider"] = "openai-compatible"
        recommendation_stage["model"] = os.environ.get("OPENAI_TEXT_MODEL", "gpt-5.4")
        _save_state(root, state)
    if stop_after == "recommendation":
        return state

    colors, roles = _palette_from_state(state)
    preview_stage = state["stages"]["preview"]
    preview_record = state["artifacts"].get("provider_preview")
    if preview_stage.get("status") == "complete" and not _artifact_valid(root, preview_record):
        preview_stage["status"] = "uncertain"
        preview_stage["error"] = "The recorded provider preview is missing or has changed."
        _save_state(root, state)
    if preview_stage.get("status") != "complete":
        raw = root / "provider-preview.png"

        def create_preview() -> Path:
            if case.reference_image:
                return Path(image_editor(
                    case.reference_image,
                    case.prompt,
                    raw,
                    colors,
                    case.style,
                    "blue",
                    palette_roles=roles,
                    custom_style=case.custom_style,
                ))
            return Path(image_generator(
                case.prompt,
                raw,
                colors,
                case.style,
                "blue",
                palette_roles=roles,
                custom_style=case.custom_style,
            ))

        generated = _stage_call(
            root,
            state,
            "preview",
            "image2",
            confirm_image_call,
            allow_retry_uncertain,
            "--confirm-image-call",
            create_preview,
        )
        state["artifacts"]["provider_preview"] = _artifact_record(root, generated)
        preview_stage["status"] = "complete"
        preview_stage["error"] = ""
        preview_stage["provider"] = "openai-compatible"
        preview_stage["model"] = os.environ.get("OPENAI_IMAGE_MODEL", "gpt-image-2")
        _save_state(root, state)
    if stop_after == "preview":
        return state

    local_stage = state["stages"]["local_gate"]
    local_artifacts = ("strict_preview", "clean_preview", "model_reference", "metadata")
    cached_local = (
        local_stage.get("status") == "complete"
        and local_stage.get("provider_preview_sha256") == state["artifacts"]["provider_preview"]["sha256"]
        and all(_artifact_valid(root, state["artifacts"].get(name)) for name in local_artifacts)
    )
    if not cached_local:
        provider_preview = root / state["artifacts"]["provider_preview"]["path"]
        local_stage["status"] = "processing"
        local_stage["attempts"] = int(local_stage.get("attempts", 0)) + 1
        local_stage["error"] = ""
        _save_state(root, state)
        try:
            result = processor(provider_preview, root, colors, PrintSettings(), roles)
        except Exception as exc:
            local_stage["status"] = "failed"
            local_stage["error"] = f"{type(exc).__name__}: {exc}"[:500]
            _save_state(root, state)
            raise
        for name in local_artifacts:
            state["artifacts"][name] = _artifact_record(root, Path(getattr(result, name)))
        state["metrics"] = dict(result.metrics)
        state["palette_usage"] = dict(result.palette_usage)
        local_stage["status"] = "complete"
        local_stage["provider_preview_sha256"] = state["artifacts"]["provider_preview"]["sha256"]
        _save_state(root, state)
    return state


def review_case(
    case_directory: Path | str,
    manifest: PaletteBenchmarkManifest,
    case: PaletteBenchmarkCase,
    *,
    confirm_visual_call: bool = False,
    allow_retry_uncertain: bool = False,
    reviewer: Callable[..., dict[str, Any]] = review_printable_palette_visual_quality,
) -> dict[str, Any]:
    root = Path(case_directory).resolve()
    state = load_case_state(root, manifest, case)
    if state["stages"]["local_gate"].get("status") != "complete":
        raise PaletteBenchmarkError("The local printable-image gate must complete before visual review.")
    strict_record = state["artifacts"].get("strict_preview")
    if not _artifact_valid(root, strict_record):
        raise PaletteBenchmarkError("The strict preview is missing or has changed.")
    stage = state["stages"]["visual_review"]
    report_record = state["artifacts"].get("visual_review")
    if stage.get("status") == "complete" and _artifact_valid(root, report_record):
        return state

    strict = root / strict_record["path"]
    report_directory = root / "visual-review"
    report_directory.mkdir(parents=True, exist_ok=True)

    def run_review() -> dict[str, Any]:
        return reviewer(
            strict,
            report_directory,
            prompt=case.prompt,
            style=case.style,
            recommendation=state["recommendation"],
            reference_path=case.reference_image,
        )

    report = _stage_call(
        root,
        state,
        "visual_review",
        "visual_review",
        confirm_visual_call,
        allow_retry_uncertain,
        "--confirm-visual-call",
        run_review,
    )
    if not isinstance(report, dict) or report.get("status") not in {"pass", "review", "unavailable"}:
        stage["status"] = "uncertain"
        stage["error"] = "The palette visual reviewer returned an invalid report."
        _save_state(root, state)
        raise PaletteBenchmarkError(stage["error"])
    report_path = report_directory / PALETTE_VISUAL_REPORT_FILENAME
    state["artifacts"]["visual_review"] = _artifact_record(root, report_path)
    state["visual_review"] = report
    stage["status"] = "complete"
    stage["error"] = ""
    stage["provider"] = "openai-compatible"
    stage["model"] = os.environ.get("OPENAI_TEXT_MODEL", "gpt-5.4")
    _save_state(root, state)
    return state
