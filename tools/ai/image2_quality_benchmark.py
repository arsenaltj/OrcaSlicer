"""Resumable real Image2 quality benchmark for public preview styles."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
import shutil
import sys
import threading
import time
import urllib.parse
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable, Iterable, Mapping

from PIL import Image, ImageDraw


TOOLS_AI = Path(__file__).resolve().parent
REPOSITORY_ROOT = TOOLS_AI.parents[1]
if str(TOOLS_AI) not in sys.path:
    sys.path.insert(0, str(TOOLS_AI))

from openai_preprocessor import (  # noqa: E402
    build_style_preview_prompt,
    preprocess_image,
)
from model_input_image_quality import assess_model_input_image  # noqa: E402
from printable_image_pipeline import PrintSettings, process_printable_image  # noqa: E402
from printable_palette import assign_palette_roles, normalize_palette  # noqa: E402


SCHEMA_VERSION = 1
PUBLIC_STYLES = ("sculpture", "realistic", "cartoon", "custom")
STATE_FILENAME = "state.json"
OUTPUT_FILENAME = "image2-output.png"
MINIMUM_EDGE = 256
MAXIMUM_PIXELS = 64_000_000


class Image2QualityBenchmarkError(RuntimeError):
    pass


@dataclass(frozen=True)
class BenchmarkCase:
    case_id: str
    source: Path
    instruction: str
    label: str = ""
    category: str = "unspecified"
    challenges: tuple[str, ...] = ()
    preserve: tuple[str, ...] = ()
    community_use: str = ""
    source_page: str = ""
    license: str = ""
    license_url: str = ""
    attribution: str = ""


@dataclass(frozen=True)
class BenchmarkCandidate:
    case: BenchmarkCase
    style: str
    palette_id: str
    palette: tuple[str, ...]
    repetition: int
    custom_style: str

    @property
    def candidate_id(self) -> str:
        palette = self.palette_id or "natural"
        return f"{self.case.case_id}__{self.style}__{palette}__r{self.repetition:02d}"


def _read_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise Image2QualityBenchmarkError(f"Cannot read benchmark JSON: {path}") from exc
    if not isinstance(value, dict):
        raise Image2QualityBenchmarkError(f"Benchmark JSON must be an object: {path}")
    return value


def _atomic_json(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".part")
    temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2), encoding="utf-8")
    # Windows file indexers and antivirus scanners may briefly lock the old
    # state file. Retry only the local atomic replace; never retry a paid call.
    for attempt in range(7):
        try:
            os.replace(temporary, path)
            return
        except PermissionError:
            if attempt == 6:
                raise
            time.sleep(0.05 * (2**attempt))


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _safe_source(repository_root: Path, value: object) -> Path:
    if not isinstance(value, str) or not value.strip():
        raise Image2QualityBenchmarkError("Each benchmark case requires a source path.")
    source = (repository_root / value).resolve()
    try:
        source.relative_to(repository_root.resolve())
    except ValueError:
        raise Image2QualityBenchmarkError("Benchmark sources must stay inside the repository.") from None
    if not source.is_file():
        raise Image2QualityBenchmarkError(f"Benchmark source does not exist: {value}")
    return source


def _case_string_list(case_id: str, field: str, value: object) -> tuple[str, ...]:
    if value is None:
        return ()
    if not isinstance(value, list) or len(value) > 16:
        raise Image2QualityBenchmarkError(f"Case {case_id} field {field} must be a list with at most 16 items.")
    result: list[str] = []
    for item in value:
        if not isinstance(item, str) or not item.strip() or len(item.strip().encode("utf-8")) > 160:
            raise Image2QualityBenchmarkError(
                f"Case {case_id} field {field} requires non-empty strings up to 160 bytes."
            )
        cleaned = item.strip()
        if cleaned not in result:
            result.append(cleaned)
    return tuple(result)


def _case_optional_string(case_id: str, field: str, value: object, maximum_bytes: int) -> str:
    if value is None:
        return ""
    if not isinstance(value, str) or len(value.strip().encode("utf-8")) > maximum_bytes:
        raise Image2QualityBenchmarkError(
            f"Case {case_id} field {field} must be a string up to {maximum_bytes} bytes."
        )
    return value.strip()


def _case_https_url(case_id: str, field: str, value: object) -> str:
    result = _case_optional_string(case_id, field, value, 800)
    if result:
        parsed = urllib.parse.urlsplit(result)
        if parsed.scheme != "https" or not parsed.netloc or parsed.username or parsed.password:
            raise Image2QualityBenchmarkError(f"Case {case_id} field {field} must be an HTTPS URL.")
    return result


def _validate_image(path: Path) -> dict[str, Any]:
    try:
        with Image.open(path) as image:
            image.load()
            width, height = image.size
            format_name = str(image.format or "").upper()
            if format_name not in {"PNG", "JPEG"}:
                raise Image2QualityBenchmarkError("Image2 output must be PNG or JPEG.")
            if min(width, height) < MINIMUM_EDGE:
                raise Image2QualityBenchmarkError("Image2 output is too small for model reference use.")
            if width * height > MAXIMUM_PIXELS:
                raise Image2QualityBenchmarkError("Image2 output exceeds the pixel limit.")
            extrema = image.convert("RGBA").getextrema()
            if extrema[3][1] == 0:
                raise Image2QualityBenchmarkError("Image2 output is fully transparent.")
            if all(low == high for low, high in extrema[:3]):
                raise Image2QualityBenchmarkError("Image2 output is visually uniform.")
    except Image2QualityBenchmarkError:
        raise
    except (OSError, ValueError) as exc:
        raise Image2QualityBenchmarkError("Image2 output cannot be fully decoded.") from exc
    return {
        "width": width,
        "height": height,
        "format": format_name,
        "sha256": _sha256(path),
        "quality": assess_model_input_image(path),
    }


def load_candidates(
    manifest_path: str | Path,
    repository_root: str | Path = REPOSITORY_ROOT,
) -> list[BenchmarkCandidate]:
    manifest = _read_json(Path(manifest_path).resolve())
    root = Path(repository_root).resolve()
    custom_style = manifest.get("custom_style", "")

    raw_palettes = manifest.get("palettes")
    if not isinstance(raw_palettes, dict) or not raw_palettes:
        raise Image2QualityBenchmarkError("The benchmark requires printable palettes.")
    palettes: dict[str, tuple[str, ...]] = {}
    for palette_id, colors in raw_palettes.items():
        if not isinstance(palette_id, str) or not palette_id:
            raise Image2QualityBenchmarkError("Palette IDs must be non-empty strings.")
        palettes[palette_id] = normalize_palette(colors)
        assign_palette_roles(palettes[palette_id])

    raw_runs = manifest.get("style_runs")
    if not isinstance(raw_runs, dict) or not raw_runs:
        raise Image2QualityBenchmarkError("style_runs must define at least one public style.")
    unknown_styles = set(raw_runs) - set(PUBLIC_STYLES)
    if unknown_styles:
        raise Image2QualityBenchmarkError("style_runs contains unknown public styles: " + ", ".join(sorted(unknown_styles)))
    active_styles = tuple(style for style in PUBLIC_STYLES if style in raw_runs)
    if "custom" in active_styles and (not isinstance(custom_style, str) or not custom_style.strip()):
        raise Image2QualityBenchmarkError("A benchmark containing custom style requires one frozen custom style description.")
    style_runs: dict[str, list[tuple[str, int]]] = {}
    for style in active_styles:
        entries = raw_runs.get(style)
        if not isinstance(entries, list) or not entries:
            raise Image2QualityBenchmarkError(f"Style {style} requires one or more run entries.")
        parsed: list[tuple[str, int]] = []
        for entry in entries:
            if not isinstance(entry, dict):
                raise Image2QualityBenchmarkError(f"Style {style} has an invalid run entry.")
            palette_id = entry.get("palette", "")
            repetitions = entry.get("repetitions")
            if palette_id is None:
                palette_id = ""
            if not isinstance(palette_id, str) or (palette_id and palette_id not in palettes):
                raise Image2QualityBenchmarkError(f"Style {style} references an unknown palette.")
            if isinstance(repetitions, bool) or not isinstance(repetitions, int) or repetitions < 1 or repetitions > 10:
                raise Image2QualityBenchmarkError("Repetitions must be integers from 1 to 10.")
            if style == "sculpture" and palette_id:
                raise Image2QualityBenchmarkError("The sculpture benchmark must remain monochrome/natural mode.")
            if style != "sculpture" and not palette_id:
                raise Image2QualityBenchmarkError("Multicolor styles require a printable palette.")
            parsed.append((palette_id, repetitions))
        style_runs[style] = parsed

    raw_cases = manifest.get("cases")
    if not isinstance(raw_cases, list) or not raw_cases:
        raise Image2QualityBenchmarkError("The benchmark requires cases.")
    cases: list[BenchmarkCase] = []
    seen_cases: set[str] = set()
    for entry in raw_cases:
        if not isinstance(entry, dict):
            raise Image2QualityBenchmarkError("Each benchmark case must be an object.")
        case_id = entry.get("id")
        instruction = entry.get("instruction")
        if not isinstance(case_id, str) or not case_id or case_id in seen_cases:
            raise Image2QualityBenchmarkError("Case IDs must be unique non-empty strings.")
        if not isinstance(instruction, str) or not instruction.strip():
            raise Image2QualityBenchmarkError(f"Case {case_id} requires an instruction.")
        category = entry.get("category", "unspecified")
        if (
            not isinstance(category, str)
            or not category.strip()
            or len(category.strip()) > 40
            or any(character not in "abcdefghijklmnopqrstuvwxyz0123456789_-" for character in category.strip())
        ):
            raise Image2QualityBenchmarkError(
                f"Case {case_id} category must use lowercase letters, digits, underscores or hyphens."
            )
        label = entry.get("label", case_id)
        if not isinstance(label, str) or not label.strip() or len(label.strip().encode("utf-8")) > 120:
            raise Image2QualityBenchmarkError(f"Case {case_id} label must be a non-empty string up to 120 bytes.")
        seen_cases.add(case_id)
        cases.append(BenchmarkCase(
            case_id,
            _safe_source(root, entry.get("source")),
            instruction.strip(),
            label.strip(),
            category.strip(),
            _case_string_list(case_id, "challenges", entry.get("challenges")),
            _case_string_list(case_id, "preserve", entry.get("preserve")),
            _case_optional_string(case_id, "community_use", entry.get("community_use"), 300),
            _case_https_url(case_id, "source_page", entry.get("source_page")),
            _case_optional_string(case_id, "license", entry.get("license"), 120),
            _case_https_url(case_id, "license_url", entry.get("license_url")),
            _case_optional_string(case_id, "attribution", entry.get("attribution"), 600),
        ))

    candidates: list[BenchmarkCandidate] = []
    for case in cases:
        for style in active_styles:
            for palette_id, repetitions in style_runs[style]:
                for repetition in range(1, repetitions + 1):
                    candidates.append(BenchmarkCandidate(
                        case=case,
                        style=style,
                        palette_id=palette_id,
                        palette=palettes.get(palette_id, ()),
                        repetition=repetition,
                        custom_style=custom_style.strip() if style == "custom" else "",
                    ))
    identifiers = [candidate.candidate_id for candidate in candidates]
    if len(identifiers) != len(set(identifiers)):
        raise Image2QualityBenchmarkError("The benchmark expands to duplicate candidate IDs.")
    return candidates


def _new_state(candidate: BenchmarkCandidate, provider_prompt: str) -> dict[str, Any]:
    return {
        "schema_version": SCHEMA_VERSION,
        "candidate_id": candidate.candidate_id,
        "case_id": candidate.case.case_id,
        "style": candidate.style,
        "palette_id": candidate.palette_id,
        "palette": list(candidate.palette),
        "repetition": candidate.repetition,
        "custom_style": candidate.custom_style,
        "source": str(candidate.case.source),
        "source_sha256": _sha256(candidate.case.source),
        "instruction": candidate.case.instruction,
        "label": candidate.case.label or candidate.case.case_id,
        "category": candidate.case.category,
        "challenges": list(candidate.case.challenges),
        "preserve": list(candidate.case.preserve),
        "community_use": candidate.case.community_use,
        "source_page": candidate.case.source_page,
        "license": candidate.case.license,
        "license_url": candidate.case.license_url,
        "attribution": candidate.case.attribution,
        "provider_prompt": provider_prompt,
        "provider_prompt_sha256": hashlib.sha256(provider_prompt.encode("utf-8")).hexdigest(),
        "provider": "openai-compatible",
        "image_model": os.environ.get("OPENAI_IMAGE_MODEL", "gpt-image-2"),
        "image_quality": os.environ.get("OPENAI_IMAGE_QUALITY", "high") or "high",
        "image_size": "1024x1024",
        "status": "pending",
        "paid_calls": {"image2": 0, "tripo": 0},
        "created_at": time.time(),
        "updated_at": time.time(),
        "error": "",
        "output": None,
        "printable": None,
    }


def _compatible_state(state: Mapping[str, Any], expected: Mapping[str, Any]) -> bool:
    keys = (
        "candidate_id", "source_sha256", "provider_prompt_sha256", "image_model", "image_quality", "image_size"
    )
    return all(state.get(key) == expected.get(key) for key in keys)


def _set_model_input_quality(directory: Path, state: dict[str, Any]) -> None:
    output_quality = state.get("output", {}).get("quality")
    printable = state.get("printable")
    reference_quality = None
    quality_source = "image2_output"
    if isinstance(printable, dict):
        relative_reference = printable.get("model_reference")
        if isinstance(relative_reference, str) and relative_reference:
            reference_path = (directory / relative_reference).resolve()
            try:
                reference_path.relative_to(directory.resolve())
            except ValueError:
                raise Image2QualityBenchmarkError("The model reference escaped its candidate directory.") from None
            reference = _validate_image(reference_path)
            printable["model_reference_sha256"] = reference["sha256"]
            printable["model_reference_quality"] = reference["quality"]
            reference_quality = reference["quality"]
            quality_source = "printable_model_reference"
    selected = reference_quality or output_quality
    if not isinstance(selected, dict):
        return
    blockers = list(selected.get("blockers", []))
    warnings = list(selected.get("warnings", []))
    state["quality"] = {
        "score": selected.get("score", 0.0),
        "model_input_eligible": bool(selected.get("model_input_eligible", False)),
        "blockers": blockers,
        "warnings": warnings,
        "flags": blockers + warnings,
        "source": quality_source,
    }


def run_candidate(
    candidate: BenchmarkCandidate,
    output_root: Path,
    *,
    image_runner: Callable[..., Path] = preprocess_image,
) -> dict[str, Any]:
    directory = output_root / "candidates" / candidate.candidate_id
    directory.mkdir(parents=True, exist_ok=True)
    state_path = directory / STATE_FILENAME
    output_path = directory / OUTPUT_FILENAME
    roles = assign_palette_roles(candidate.palette).color_by_role if candidate.palette else {}
    provider_prompt = build_style_preview_prompt(
        candidate.case.instruction,
        candidate.palette,
        candidate.style,
        palette_roles=roles,
        custom_style=candidate.custom_style,
    )
    expected = _new_state(candidate, provider_prompt)
    (directory / "provider-prompt.txt").write_text(provider_prompt, encoding="utf-8")

    if state_path.is_file():
        state = _read_json(state_path)
        if not _compatible_state(state, expected):
            raise Image2QualityBenchmarkError(f"Frozen candidate changed: {candidate.candidate_id}")
        if output_path.is_file():
            output = _validate_image(output_path)
            if state.get("status") == "complete" and state.get("output", {}).get("sha256") == output["sha256"]:
                state["output"] = output
                _set_model_input_quality(directory, state)
                state["resumed"] = True
                state["updated_at"] = time.time()
                _atomic_json(state_path, state)
                return state
            if int(state.get("paid_calls", {}).get("image2", 0)) > 0:
                state["output"] = output
                state["status"] = "image_ready"
                state["updated_at"] = time.time()
                _atomic_json(state_path, state)
        if int(state.get("paid_calls", {}).get("image2", 0)) > 0 and not output_path.is_file():
            state["status"] = "uncertain" if state.get("status") == "calling" else state.get("status", "failed")
            state["updated_at"] = time.time()
            _atomic_json(state_path, state)
            raise Image2QualityBenchmarkError(
                f"Candidate {candidate.candidate_id} already attempted one paid call without a reusable image; refusing repeat."
            )
    else:
        state = expected
        _atomic_json(state_path, state)

    if not output_path.is_file():
        state["status"] = "calling"
        state["paid_calls"]["image2"] = int(state["paid_calls"].get("image2", 0)) + 1
        state["paid_call_started_at"] = time.time()
        state["updated_at"] = time.time()
        _atomic_json(state_path, state)
        try:
            image_runner(
                candidate.case.source,
                candidate.case.instruction,
                output_path,
                candidate.palette,
                candidate.style,
                palette_roles=roles,
                custom_style=candidate.custom_style,
            )
            state["output"] = _validate_image(output_path)
            state["status"] = "image_ready"
            state["updated_at"] = time.time()
            _atomic_json(state_path, state)
        except Exception as exc:
            state["status"] = "failed"
            state["error"] = f"{type(exc).__name__}: {exc}"[:500]
            state["updated_at"] = time.time()
            _atomic_json(state_path, state)
            raise

    try:
        if candidate.palette:
            printable_dir = directory / "printable"
            result = process_printable_image(
                output_path,
                printable_dir,
                candidate.palette,
                PrintSettings(width_mm=160.0, nozzle_mm=0.4, line_width_mm=0.4, minimum_feature_mm=0.8),
                roles,
            )
            model_reference = _validate_image(result.model_reference)
            state["printable"] = {
                "metrics": result.metrics,
                "model_reference": str(result.model_reference.relative_to(directory)),
                "model_reference_sha256": model_reference["sha256"],
                "model_reference_quality": model_reference["quality"],
            }
        else:
            state["printable"] = {
                "metrics": {},
                "model_reference": OUTPUT_FILENAME,
                "model_reference_sha256": state["output"]["sha256"],
                "model_reference_quality": state["output"]["quality"],
            }
        _set_model_input_quality(directory, state)
        state["status"] = "complete"
        state["error"] = ""
        state["completed_at"] = time.time()
        state["updated_at"] = time.time()
        state["resumed"] = False
        _atomic_json(state_path, state)
        return state
    except Exception as exc:
        state["status"] = "local_failed"
        state["error"] = f"{type(exc).__name__}: {exc}"[:500]
        state["updated_at"] = time.time()
        _atomic_json(state_path, state)
        raise


def refresh_quality_assessments(output_root: Path, candidates: Iterable[BenchmarkCandidate]) -> dict[str, int]:
    """Re-score reusable benchmark artifacts without invoking a provider."""

    refreshed = 0
    failed = 0
    for candidate in candidates:
        directory = output_root / "candidates" / candidate.candidate_id
        state_path = directory / STATE_FILENAME
        output_path = directory / OUTPUT_FILENAME
        if not state_path.is_file() or not output_path.is_file():
            continue
        state = _read_json(state_path)
        try:
            state["output"] = _validate_image(output_path)
            _set_model_input_quality(directory, state)
            state["quality_refreshed_at"] = time.time()
            _atomic_json(state_path, state)
            refreshed += 1
        except Exception as exc:
            state["quality"] = {
                "score": 0.0,
                "model_input_eligible": False,
                "blockers": ["quality_assessment_failed"],
                "warnings": [],
                "flags": ["quality_assessment_failed"],
                "source": "assessment_error",
            }
            state["quality_assessment_error"] = f"{type(exc).__name__}: {exc}"[:500]
            _atomic_json(state_path, state)
            failed += 1
    return {"refreshed": refreshed, "failed": failed}


def _state_rows(output_root: Path, candidates: Iterable[BenchmarkCandidate]) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for candidate in candidates:
        path = output_root / "candidates" / candidate.candidate_id / STATE_FILENAME
        if path.is_file():
            state = _read_json(path)
        else:
            state = {
                "candidate_id": candidate.candidate_id,
                "case_id": candidate.case.case_id,
                "style": candidate.style,
                "palette_id": candidate.palette_id,
                "repetition": candidate.repetition,
                "status": "pending",
                "paid_calls": {"image2": 0, "tripo": 0},
                "error": "",
            }
        state.setdefault("category", candidate.case.category)
        state.setdefault("label", candidate.case.label or candidate.case.case_id)
        state.setdefault("challenges", list(candidate.case.challenges))
        state.setdefault("preserve", list(candidate.case.preserve))
        state.setdefault("community_use", candidate.case.community_use)
        state.setdefault("source_page", candidate.case.source_page)
        state.setdefault("license", candidate.case.license)
        state.setdefault("license_url", candidate.case.license_url)
        state.setdefault("attribution", candidate.case.attribution)
        rows.append(state)
    return rows


def write_summary(output_root: Path, candidates: Iterable[BenchmarkCandidate]) -> dict[str, Any]:
    candidate_list = list(candidates)
    rows = _state_rows(output_root, candidate_list)
    active_styles = tuple(style for style in PUBLIC_STYLES if any(candidate.style == style for candidate in candidate_list))
    statuses: dict[str, int] = {}
    by_style: dict[str, dict[str, Any]] = {
        style: {"total": 0, "complete": 0, "failed": 0, "quality_assessed": 0, "quality_eligible": 0,
                "quality_blocked": 0, "quality_score_total": 0.0}
        for style in active_styles
    }
    by_category: dict[str, dict[str, int]] = {}
    quality_flags: dict[str, int] = {}
    quality_assessed = 0
    quality_eligible = 0
    quality_score_total = 0.0
    for row in rows:
        status = str(row.get("status", "pending"))
        statuses[status] = statuses.get(status, 0) + 1
        style = str(row.get("style", ""))
        category = str(row.get("category", "unspecified"))
        category_values = by_category.setdefault(category, {"total": 0, "complete": 0, "quality_eligible": 0})
        category_values["total"] += 1
        if style in by_style:
            by_style[style]["total"] += 1
            if status == "complete":
                by_style[style]["complete"] += 1
                category_values["complete"] += 1
            elif status in {"failed", "local_failed", "uncertain"}:
                by_style[style]["failed"] += 1
        quality = row.get("quality")
        if isinstance(quality, dict):
            quality_assessed += 1
            score = float(quality.get("score", 0.0))
            quality_score_total += score
            if style in by_style:
                by_style[style]["quality_assessed"] += 1
                by_style[style]["quality_score_total"] += score
            if quality.get("model_input_eligible"):
                quality_eligible += 1
                category_values["quality_eligible"] += 1
                if style in by_style:
                    by_style[style]["quality_eligible"] += 1
            elif style in by_style:
                by_style[style]["quality_blocked"] += 1
            for flag in quality.get("flags", []):
                quality_flags[str(flag)] = quality_flags.get(str(flag), 0) + 1
    for values in by_style.values():
        assessed = int(values.pop("quality_assessed"))
        score_total = float(values.pop("quality_score_total"))
        eligible = int(values["quality_eligible"])
        values["quality_assessed"] = assessed
        values["quality_pass_rate"] = round(eligible / assessed, 4) if assessed else None
        values["mean_quality_score"] = round(score_total / assessed, 2) if assessed else None
    summary = {
        "schema_version": SCHEMA_VERSION,
        "candidate_count": len(rows),
        "paid_image2_calls": sum(int(row.get("paid_calls", {}).get("image2", 0)) for row in rows),
        "paid_tripo_calls": sum(int(row.get("paid_calls", {}).get("tripo", 0)) for row in rows),
        "statuses": statuses,
        "by_style": by_style,
        "by_category": by_category,
        "quality": {
            "assessed": quality_assessed,
            "eligible": quality_eligible,
            "blocked": quality_assessed - quality_eligible,
            "pass_rate": round(quality_eligible / quality_assessed, 4) if quality_assessed else None,
            "mean_score": round(quality_score_total / quality_assessed, 2) if quality_assessed else None,
            "flags": dict(sorted(quality_flags.items(), key=lambda item: (-item[1], item[0]))),
        },
        "updated_at": time.time(),
        "rows": rows,
    }
    _atomic_json(output_root / "benchmark-summary.json", summary)
    with (output_root / "benchmark-summary.csv").open("w", encoding="utf-8-sig", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=(
            "candidate_id", "case_id", "category", "community_use", "source_page", "license", "attribution",
            "challenges", "preserve", "style", "palette_id", "repetition", "status", "image2_calls",
            "quality_score", "model_input_eligible", "quality_flags", "error"
        ))
        writer.writeheader()
        for row in rows:
            writer.writerow({
                "candidate_id": row.get("candidate_id", ""),
                "case_id": row.get("case_id", ""),
                "category": row.get("category", "unspecified"),
                "community_use": row.get("community_use", ""),
                "source_page": row.get("source_page", ""),
                "license": row.get("license", ""),
                "attribution": row.get("attribution", ""),
                "challenges": " | ".join(row.get("challenges", [])),
                "preserve": " | ".join(row.get("preserve", [])),
                "style": row.get("style", ""),
                "palette_id": row.get("palette_id", ""),
                "repetition": row.get("repetition", ""),
                "status": row.get("status", ""),
                "image2_calls": row.get("paid_calls", {}).get("image2", 0),
                "quality_score": row.get("quality", {}).get("score", ""),
                "model_input_eligible": row.get("quality", {}).get("model_input_eligible", ""),
                "quality_flags": ",".join(row.get("quality", {}).get("flags", [])),
                "error": row.get("error", ""),
            })
    return summary


def write_validation_catalog(output_root: Path, candidates: Iterable[BenchmarkCandidate]) -> dict[str, Any]:
    """Write a compact human-readable checklist from frozen case metadata."""

    candidate_list = list(candidates)
    styles = [style for style in PUBLIC_STYLES if any(candidate.style == style for candidate in candidate_list)]
    cases: list[BenchmarkCase] = []
    seen: set[str] = set()
    for candidate in candidate_list:
        if candidate.case.case_id not in seen:
            seen.add(candidate.case.case_id)
            cases.append(candidate.case)
    payload = {
        "schema_version": SCHEMA_VERSION,
        "styles": styles,
        "candidate_count": len(candidate_list),
        "case_count": len(cases),
        "cases": [{
            "id": case.case_id,
            "label": case.label or case.case_id,
            "category": case.category,
            "challenges": list(case.challenges),
            "preserve": list(case.preserve),
            "community_use": case.community_use,
            "source_page": case.source_page,
            "license": case.license,
            "license_url": case.license_url,
            "attribution": case.attribution,
        } for case in cases],
    }
    _atomic_json(output_root / "validation-catalog.json", payload)
    style_labels = {"sculpture": "单色写实雕塑", "realistic": "多色写实", "cartoon": "多色卡通", "custom": "自定义"}
    lines = [
        "# Image2 风格质量验证清单",
        "",
        "风格：" + "、".join(style_labels.get(style, style) for style in styles),
        "",
        f"共 {len(cases)} 个案例、{len(candidate_list)} 个候选。每张先检查主体与必保留元素，再检查 3D 输入门禁。",
        "",
        "| 案例 | 社区用途 | 类别 | 难点 | 必保留元素 | 来源/许可 |",
        "|---|---|---|---|---|---|",
    ]
    for case in cases:
        challenges = "、".join(case.challenges) or "基础保真"
        preserve = "、".join(case.preserve) or "主体、轮廓、姿态、数量"
        source = f"[{case.license}]({case.source_page})" if case.source_page and case.license else "内部/既有"
        lines.append(
            f"| {case.label or case.case_id} | {case.community_use or '-'} | {case.category} | "
            f"{challenges} | {preserve} | {source} |"
        )
    lines.extend([
        "",
        "## 通用判定",
        "",
        "- 单色写实雕塑：只能改变材质和表面表现，不能借单色之名删减部件。",
        "- 多色写实：保持主体身份、结构、比例、视角和颜色角色，不泛化成同类商品或人物。",
        "- 多色卡通：可通过圆润线条和表情变可爱，但不能换成通用娃娃脸或改变标志性比例。",
        "- 三种风格：人物/人像必须使用一个低矮一体底座；半身像不补造腿脚，全身像不改变姿势，多人像共用一个底座。非人物不得凭空添加底座/配饰/零件。",
        "- 人像底座必须有平整底面，并通过脚、衣摆、座椅或半身下缘与主体形成清晰实体连接，不能仅靠阴影接触或出现悬浮。",
    ])
    (output_root / "validation-catalog.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    return payload


def _sheet_tile(path: Path, label: str, tile_size: int = 300) -> Image.Image:
    canvas = Image.new("RGB", (tile_size, tile_size + 30), "white")
    with Image.open(path) as image:
        image.load()
        converted = image.convert("RGBA")
        converted.thumbnail((tile_size - 12, tile_size - 12), Image.Resampling.LANCZOS)
        checker = Image.new("RGBA", converted.size, (248, 248, 248, 255))
        checker_draw = ImageDraw.Draw(checker)
        square = 16
        for y in range(0, converted.height, square):
            for x in range(0, converted.width, square):
                if (x // square + y // square) % 2:
                    checker_draw.rectangle(
                        (x, y, min(x + square - 1, converted.width - 1), min(y + square - 1, converted.height - 1)),
                        fill=(226, 226, 226, 255),
                    )
        checker.alpha_composite(converted)
        x = (tile_size - converted.width) // 2
        y = (tile_size - converted.height) // 2
        canvas.paste(checker.convert("RGB"), (x, y))
    ImageDraw.Draw(canvas).text((8, tile_size + 8), label[:44], fill="black")
    return canvas


def create_contact_sheets(output_root: Path, candidates: Iterable[BenchmarkCandidate]) -> int:
    grouped: dict[tuple[str, str], list[BenchmarkCandidate]] = {}
    for candidate in candidates:
        grouped.setdefault((candidate.case.case_id, candidate.style), []).append(candidate)
    count = 0
    for (case_id, style), group in sorted(grouped.items()):
        complete: list[tuple[BenchmarkCandidate, dict[str, Any]]] = []
        for candidate in sorted(group, key=lambda item: item.candidate_id):
            state_path = output_root / "candidates" / candidate.candidate_id / STATE_FILENAME
            if state_path.is_file():
                state = _read_json(state_path)
                if state.get("status") == "complete":
                    complete.append((candidate, state))
        if not complete:
            continue
        raw_tiles = [_sheet_tile(group[0].case.source, "SOURCE")]
        model_tiles = [_sheet_tile(group[0].case.source, "SOURCE")]
        for candidate, state in complete:
            directory = output_root / "candidates" / candidate.candidate_id
            label = f"{candidate.palette_id or 'natural'} r{candidate.repetition}"
            raw_tiles.append(_sheet_tile(directory / OUTPUT_FILENAME, label))
            reference = directory / str(state["printable"]["model_reference"])
            model_tiles.append(_sheet_tile(reference, label))
        for stage, tiles in (("raw", raw_tiles), ("model-reference", model_tiles)):
            sheet = Image.new("RGB", (sum(tile.width for tile in tiles), max(tile.height for tile in tiles)), "white")
            x = 0
            for tile in tiles:
                sheet.paste(tile, (x, 0))
                x += tile.width
            destination = output_root / "contact-sheets" / stage / f"{case_id}__{style}.jpg"
            destination.parent.mkdir(parents=True, exist_ok=True)
            sheet.save(destination, format="JPEG", quality=92, optimize=True)
            count += 1
    return count


def _blank_sheet_tile(label: str, tile_size: int = 240) -> Image.Image:
    tile = Image.new("RGB", (tile_size, tile_size + 30), (238, 240, 243))
    draw = ImageDraw.Draw(tile)
    draw.rectangle((12, 12, tile_size - 13, tile_size - 13), outline=(180, 184, 190), width=2)
    draw.text((tile_size // 2 - 24, tile_size // 2 - 6), "PENDING", fill=(110, 114, 120))
    draw.text((8, tile_size + 8), label[:44], fill="black")
    return tile


def _candidate_artifacts(
    output_root: Path, candidate: BenchmarkCandidate | None
) -> tuple[Path | None, Path | None]:
    if candidate is None:
        return None, None
    directory = output_root / "candidates" / candidate.candidate_id
    state_path = directory / STATE_FILENAME
    raw_path = directory / OUTPUT_FILENAME
    if not state_path.is_file() or not raw_path.is_file():
        return None, None
    state = _read_json(state_path)
    if state.get("status") != "complete":
        return None, None
    printable = state.get("printable")
    if not isinstance(printable, dict) or not isinstance(printable.get("model_reference"), str):
        return raw_path, None
    reference = (directory / printable["model_reference"]).resolve()
    try:
        reference.relative_to(directory.resolve())
    except ValueError:
        return raw_path, None
    return raw_path, reference if reference.is_file() else None


def _preferred_candidate(
    candidates: Iterable[BenchmarkCandidate], style: str, palette_id: str | None = None
) -> BenchmarkCandidate | None:
    matching = [candidate for candidate in candidates if candidate.style == style]
    if palette_id is not None:
        preferred = [candidate for candidate in matching if candidate.palette_id == palette_id]
        if preferred:
            matching = preferred
    return sorted(matching, key=lambda item: item.candidate_id)[0] if matching else None


def _write_overview_pages(
    destination_root: Path,
    headers: list[str],
    rows: list[list[tuple[Path | None, str]]],
    cases_per_page: int,
) -> int:
    tile_size = 240
    tile_height = tile_size + 30
    header_height = 38
    destination_root.mkdir(parents=True, exist_ok=True)
    page_count = 0
    for start in range(0, len(rows), cases_per_page):
        page_rows = rows[start:start + cases_per_page]
        canvas = Image.new(
            "RGB",
            (len(headers) * tile_size, header_height + len(page_rows) * tile_height),
            (232, 234, 238),
        )
        draw = ImageDraw.Draw(canvas)
        for column, header in enumerate(headers):
            draw.text((column * tile_size + 8, 12), header, fill="black")
        for row_index, row in enumerate(page_rows):
            for column, (path, label) in enumerate(row):
                tile = _sheet_tile(path, label, tile_size) if path and path.is_file() else _blank_sheet_tile(label)
                canvas.paste(tile, (column * tile_size, header_height + row_index * tile_height))
        page_count += 1
        canvas.save(destination_root / f"page-{page_count:02d}.jpg", quality=92, optimize=True)
    return page_count


def create_journey_summary_sheets(
    output_root: Path, candidates: Iterable[BenchmarkCandidate], cases_per_page: int = 10
) -> dict[str, int]:
    """Create the user-journey six-column review plus a palette comparison appendix."""

    if cases_per_page < 1:
        raise Image2QualityBenchmarkError("cases_per_page must be positive")
    grouped: dict[str, list[BenchmarkCandidate]] = {}
    cases: list[BenchmarkCase] = []
    for candidate in candidates:
        if candidate.case.case_id not in grouped:
            grouped[candidate.case.case_id] = []
            cases.append(candidate.case)
        grouped[candidate.case.case_id].append(candidate)
    primary_rows: list[list[tuple[Path | None, str]]] = []
    palette_rows: list[list[tuple[Path | None, str]]] = []
    for case in cases:
        group = grouped[case.case_id]
        sculpture = _preferred_candidate(group, "sculpture")
        realistic_warm = _preferred_candidate(group, "realistic", "warm")
        realistic_cool = _preferred_candidate(group, "realistic", "cool")
        cartoon_warm = _preferred_candidate(group, "cartoon", "warm")
        cartoon_cool = _preferred_candidate(group, "cartoon", "cool")
        sculpture_raw, sculpture_reference = _candidate_artifacts(output_root, sculpture)
        realistic_warm_raw, realistic_warm_reference = _candidate_artifacts(output_root, realistic_warm)
        realistic_cool_raw, realistic_cool_reference = _candidate_artifacts(output_root, realistic_cool)
        cartoon_warm_raw, cartoon_warm_reference = _candidate_artifacts(output_root, cartoon_warm)
        cartoon_cool_raw, cartoon_cool_reference = _candidate_artifacts(output_root, cartoon_cool)
        primary_rows.append([
            (case.source, case.case_id),
            (sculpture_reference or sculpture_raw, "natural"),
            (realistic_warm_raw, "warm"),
            (realistic_warm_reference, "warm"),
            (cartoon_warm_raw, "warm"),
            (cartoon_warm_reference, "warm"),
        ])
        palette_rows.append([
            (case.source, case.case_id),
            (realistic_warm_reference, "realistic warm"),
            (realistic_cool_reference, "realistic cool"),
            (cartoon_warm_reference, "cartoon warm"),
            (cartoon_cool_reference, "cartoon cool"),
        ])
    overview_root = output_root / "overview-sheets"
    return {
        "primary": _write_overview_pages(
            overview_root / "primary",
            ["Source", "Sculpture", "Realistic raw", "Realistic 3D ref", "Cartoon raw", "Cartoon 3D ref"],
            primary_rows,
            cases_per_page,
        ),
        "palette": _write_overview_pages(
            overview_root / "palette",
            ["Source", "Realistic warm", "Realistic cool", "Cartoon warm", "Cartoon cool"],
            palette_rows,
            cases_per_page,
        ),
    }


def _select_candidates(
    candidates: list[BenchmarkCandidate], identifiers: list[str], limit: int | None
) -> list[BenchmarkCandidate]:
    if identifiers:
        wanted = set(identifiers)
        selected = [candidate for candidate in candidates if candidate.candidate_id in wanted]
        missing = sorted(wanted - {candidate.candidate_id for candidate in selected})
        if missing:
            raise Image2QualityBenchmarkError("Unknown candidate IDs: " + ", ".join(missing))
    else:
        selected = candidates
    return selected[:limit] if limit is not None else selected


def build_dry_run_report(
    output_root: Path,
    candidates: Iterable[BenchmarkCandidate],
    selected: Iterable[BenchmarkCandidate],
) -> dict[str, Any]:
    """Describe exactly what a run would reuse, process locally, call or block."""

    all_candidates = list(candidates)
    selected_candidates = list(selected)
    active_styles = tuple(style for style in PUBLIC_STYLES if any(candidate.style == style for candidate in all_candidates))
    classifications: dict[str, int] = {
        "reusable": 0,
        "local_resume": 0,
        "planned_paid_call": 0,
        "blocked_after_paid_attempt": 0,
        "frozen_candidate_changed": 0,
        "invalid_existing_output": 0,
    }
    by_style = {style: {name: 0 for name in classifications} for style in active_styles}
    blocked_ids: list[str] = []
    frozen_mismatch_fields: dict[str, int] = {}
    for candidate in selected_candidates:
        directory = output_root / "candidates" / candidate.candidate_id
        state_path = directory / STATE_FILENAME
        output_path = directory / OUTPUT_FILENAME
        state = _read_json(state_path) if state_path.is_file() else None
        classification = "planned_paid_call"
        if state is not None:
            roles = assign_palette_roles(candidate.palette).color_by_role if candidate.palette else {}
            prompt = build_style_preview_prompt(
                candidate.case.instruction,
                candidate.palette,
                candidate.style,
                palette_roles=roles,
                custom_style=candidate.custom_style,
            )
            expected = _new_state(candidate, prompt)
            if not _compatible_state(state, expected):
                classification = "frozen_candidate_changed"
                for key in (
                    "candidate_id", "source_sha256", "provider_prompt_sha256", "image_model", "image_quality", "image_size"
                ):
                    if state.get(key) != expected.get(key):
                        frozen_mismatch_fields[key] = frozen_mismatch_fields.get(key, 0) + 1
            elif output_path.is_file():
                try:
                    _validate_image(output_path)
                    classification = "reusable" if state.get("status") == "complete" else "local_resume"
                except Exception:
                    classification = "invalid_existing_output"
            elif int(state.get("paid_calls", {}).get("image2", 0)) > 0:
                classification = "blocked_after_paid_attempt"
        elif output_path.is_file():
            classification = "invalid_existing_output"
        classifications[classification] += 1
        by_style[candidate.style][classification] += 1
        if classification in {
            "blocked_after_paid_attempt", "frozen_candidate_changed", "invalid_existing_output"
        }:
            blocked_ids.append(candidate.candidate_id)

    endpoint = os.environ.get("OPENAI_BASE_URL", "https://api.openai.com/v1").strip()
    endpoint_host = urllib.parse.urlsplit(endpoint).netloc
    return {
        "schema_version": SCHEMA_VERSION,
        "mode": "dry_run",
        "candidate_count": len(all_candidates),
        "selected_count": len(selected_candidates),
        "planned_paid_calls": classifications["planned_paid_call"],
        "paid_tripo_calls": 0,
        "classifications": classifications,
        "by_style": by_style,
        "blocked_candidate_count": len(blocked_ids),
        "blocked_candidate_ids_preview": blocked_ids[:20],
        "frozen_mismatch_fields": frozen_mismatch_fields,
        "ready": {
            "api_key_present": bool(os.environ.get("OPENAI_API_KEY")),
            "endpoint_host": endpoint_host,
            "safe_to_start": not blocked_ids and bool(os.environ.get("OPENAI_API_KEY")),
        },
    }


def skip_blocked_candidates(
    output_root: Path,
    all_candidates: Iterable[BenchmarkCandidate],
    selected: Iterable[BenchmarkCandidate],
) -> tuple[list[BenchmarkCandidate], list[str]]:
    """Exclude only candidates that the dry-run classifies as unsafe to resume."""

    all_items = list(all_candidates)
    resumable: list[BenchmarkCandidate] = []
    skipped: list[str] = []
    for candidate in selected:
        report = build_dry_run_report(output_root, all_items, (candidate,))
        if report["blocked_candidate_count"]:
            skipped.append(candidate.candidate_id)
        else:
            resumable.append(candidate)
    return resumable, skipped


def main() -> int:
    parser = argparse.ArgumentParser(description="Run a resumable real Image2 public-style quality benchmark.")
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--workers", type=int, default=2)
    parser.add_argument("--candidate", action="append", default=[])
    parser.add_argument("--limit", type=int)
    parser.add_argument("--list", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--report-only", action="store_true")
    parser.add_argument(
        "--skip-blocked",
        action="store_true",
        help="Explicitly omit prior paid/uncertain or incompatible states instead of retrying them.",
    )
    parser.add_argument("--confirm-image2-calls", action="store_true")
    parser.add_argument("--max-paid-calls", type=int)
    args = parser.parse_args()
    if not 1 <= args.workers <= 4:
        parser.error("--workers must be from 1 to 4")
    if args.limit is not None and args.limit < 1:
        parser.error("--limit must be positive")
    if args.max_paid_calls is not None and args.max_paid_calls < 0:
        parser.error("--max-paid-calls must not be negative")
    if args.dry_run and args.confirm_image2_calls:
        parser.error("--dry-run cannot be combined with --confirm-image2-calls")
    candidates = load_candidates(args.manifest)
    selected = _select_candidates(candidates, args.candidate, args.limit)
    if args.list:
        for candidate in selected:
            print(candidate.candidate_id)
        return 0
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    frozen_manifest = output / "frozen-manifest.json"
    manifest_bytes = args.manifest.resolve().read_bytes()
    if frozen_manifest.is_file() and frozen_manifest.read_bytes() != manifest_bytes:
        parser.error("output already belongs to a different frozen manifest")
    if not frozen_manifest.is_file():
        frozen_manifest.write_bytes(manifest_bytes)
    catalog = write_validation_catalog(output, candidates)
    if args.report_only:
        refresh = refresh_quality_assessments(output, candidates)
        summary = write_summary(output, candidates)
        sheets = create_contact_sheets(output, candidates)
        overview_sheets = create_journey_summary_sheets(output, candidates)
        compact_summary = {key: value for key, value in summary.items() if key != "rows"}
        print(json.dumps({
            "quality_refresh": refresh,
            "summary": compact_summary,
            "contact_sheets": sheets,
            "overview_sheets": overview_sheets,
            "validation_cases": catalog["case_count"],
        }, ensure_ascii=False, indent=2))
        return 0
    skipped_blocked: list[str] = []
    if args.skip_blocked:
        selected, skipped_blocked = skip_blocked_candidates(output, candidates, selected)
        if not selected:
            parser.error("--skip-blocked removed every selected candidate")
    if not args.confirm_image2_calls:
        report = build_dry_run_report(output, candidates, selected)
        report["skipped_blocked_count"] = len(skipped_blocked)
        report["skipped_blocked_ids"] = skipped_blocked
        print(json.dumps(report, ensure_ascii=False, indent=2))
        return 0

    dry_run = build_dry_run_report(output, candidates, selected)
    if dry_run["blocked_candidate_count"]:
        parser.error("selected candidates contain blocked or incompatible prior state; inspect the dry-run report")
    if args.max_paid_calls is None:
        parser.error("--max-paid-calls is required with --confirm-image2-calls")
    if dry_run["planned_paid_calls"] > args.max_paid_calls:
        parser.error(
            f"dry-run requires {dry_run['planned_paid_calls']} paid calls, exceeding --max-paid-calls {args.max_paid_calls}"
        )
    if dry_run["planned_paid_calls"] and not dry_run["ready"]["api_key_present"]:
        parser.error("OPENAI_API_KEY is required for the planned Image2 calls")

    summary_lock = threading.Lock()
    failures = 0
    with ThreadPoolExecutor(max_workers=args.workers) as pool:
        futures = {pool.submit(run_candidate, candidate, output): candidate for candidate in selected}
        for future in as_completed(futures):
            candidate = futures[future]
            try:
                state = future.result()
                payload = {"candidate_id": candidate.candidate_id, "status": state.get("status"), "error": ""}
            except Exception as exc:
                failures += 1
                payload = {"candidate_id": candidate.candidate_id, "status": "failed", "error": f"{type(exc).__name__}: {exc}"}
            with summary_lock:
                summary = write_summary(output, candidates)
            payload["paid_image2_calls"] = summary["paid_image2_calls"]
            payload["complete"] = summary["statuses"].get("complete", 0)
            print(json.dumps(payload, ensure_ascii=False), flush=True)
    summary = write_summary(output, candidates)
    create_contact_sheets(output, candidates)
    create_journey_summary_sheets(output, candidates)
    print(json.dumps({
        "candidate_count": summary["candidate_count"],
        "paid_image2_calls": summary["paid_image2_calls"],
        "statuses": summary["statuses"],
        "skipped_blocked_count": len(skipped_blocked),
        "skipped_blocked_ids": skipped_blocked,
    }, ensure_ascii=False, indent=2))
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
