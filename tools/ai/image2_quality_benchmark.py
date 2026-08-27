"""Resumable real Image2 quality benchmark for the four public preview styles."""

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
    os.replace(temporary, path)


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
    return {"width": width, "height": height, "format": format_name, "sha256": _sha256(path)}


def load_candidates(
    manifest_path: str | Path,
    repository_root: str | Path = REPOSITORY_ROOT,
) -> list[BenchmarkCandidate]:
    manifest = _read_json(Path(manifest_path).resolve())
    root = Path(repository_root).resolve()
    custom_style = manifest.get("custom_style")
    if not isinstance(custom_style, str) or not custom_style.strip():
        raise Image2QualityBenchmarkError("The benchmark requires one frozen custom style description.")

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
    if not isinstance(raw_runs, dict) or set(raw_runs) != set(PUBLIC_STYLES):
        raise Image2QualityBenchmarkError("style_runs must define exactly the four public styles.")
    style_runs: dict[str, list[tuple[str, int]]] = {}
    for style in PUBLIC_STYLES:
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
        seen_cases.add(case_id)
        cases.append(BenchmarkCase(case_id, _safe_source(root, entry.get("source")), instruction.strip()))

    candidates: list[BenchmarkCandidate] = []
    for case in cases:
        for style in PUBLIC_STYLES:
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
                state["resumed"] = True
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
            }
        else:
            state["printable"] = {
                "metrics": {},
                "model_reference": OUTPUT_FILENAME,
                "model_reference_sha256": state["output"]["sha256"],
            }
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
        rows.append(state)
    return rows


def write_summary(output_root: Path, candidates: Iterable[BenchmarkCandidate]) -> dict[str, Any]:
    candidate_list = list(candidates)
    rows = _state_rows(output_root, candidate_list)
    statuses: dict[str, int] = {}
    by_style: dict[str, dict[str, int]] = {style: {"total": 0, "complete": 0, "failed": 0} for style in PUBLIC_STYLES}
    for row in rows:
        status = str(row.get("status", "pending"))
        statuses[status] = statuses.get(status, 0) + 1
        style = str(row.get("style", ""))
        if style in by_style:
            by_style[style]["total"] += 1
            if status == "complete":
                by_style[style]["complete"] += 1
            elif status in {"failed", "local_failed", "uncertain"}:
                by_style[style]["failed"] += 1
    summary = {
        "schema_version": SCHEMA_VERSION,
        "candidate_count": len(rows),
        "paid_image2_calls": sum(int(row.get("paid_calls", {}).get("image2", 0)) for row in rows),
        "paid_tripo_calls": sum(int(row.get("paid_calls", {}).get("tripo", 0)) for row in rows),
        "statuses": statuses,
        "by_style": by_style,
        "updated_at": time.time(),
        "rows": rows,
    }
    _atomic_json(output_root / "benchmark-summary.json", summary)
    with (output_root / "benchmark-summary.csv").open("w", encoding="utf-8-sig", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=(
            "candidate_id", "case_id", "style", "palette_id", "repetition", "status", "image2_calls", "error"
        ))
        writer.writeheader()
        for row in rows:
            writer.writerow({
                "candidate_id": row.get("candidate_id", ""),
                "case_id": row.get("case_id", ""),
                "style": row.get("style", ""),
                "palette_id": row.get("palette_id", ""),
                "repetition": row.get("repetition", ""),
                "status": row.get("status", ""),
                "image2_calls": row.get("paid_calls", {}).get("image2", 0),
                "error": row.get("error", ""),
            })
    return summary


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


def main() -> int:
    parser = argparse.ArgumentParser(description="Run a resumable real Image2 four-style quality benchmark.")
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--workers", type=int, default=2)
    parser.add_argument("--candidate", action="append", default=[])
    parser.add_argument("--limit", type=int)
    parser.add_argument("--list", action="store_true")
    parser.add_argument("--report-only", action="store_true")
    parser.add_argument("--confirm-image2-calls", action="store_true")
    args = parser.parse_args()
    if not 1 <= args.workers <= 4:
        parser.error("--workers must be from 1 to 4")
    if args.limit is not None and args.limit < 1:
        parser.error("--limit must be positive")
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
    if args.report_only:
        summary = write_summary(output, candidates)
        sheets = create_contact_sheets(output, candidates)
        print(json.dumps({"summary": summary, "contact_sheets": sheets}, ensure_ascii=False, indent=2))
        return 0
    if not args.confirm_image2_calls:
        parser.error("--confirm-image2-calls is required before real Image2 calls")

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
    print(json.dumps({
        "candidate_count": summary["candidate_count"],
        "paid_image2_calls": summary["paid_image2_calls"],
        "statuses": summary["statuses"],
    }, ensure_ascii=False, indent=2))
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
