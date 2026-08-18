"""Repeatable paid Image2 benchmark for printable designer-toy previews."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass
from pathlib import Path
import sys
from typing import Any, Iterable, Mapping


TOOLS_AI = Path(__file__).resolve().parent
if str(TOOLS_AI) not in sys.path:
    sys.path.insert(0, str(TOOLS_AI))

from openai_preprocessor import (  # noqa: E402
    build_style_preview_prompt,
    preprocess_image,
)
from printable_image_pipeline import (  # noqa: E402
    PrintSettings,
    process_printable_image,
)
from printable_palette import assign_palette_roles, normalize_palette  # noqa: E402


@dataclass(frozen=True)
class BenchmarkCandidate:
    case_id: str
    palette_id: str
    variant_id: str
    source: Path
    instruction: str
    style: str
    colors: tuple[str, ...]
    role_overrides: Mapping[str, str]

    @property
    def candidate_id(self) -> str:
        return f"{self.case_id}__{self.palette_id}__{self.variant_id}"


def _read_json(path: Path) -> dict[str, Any]:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise ValueError("benchmark manifest must be a JSON object")
    return value


def _safe_source(root: Path, value: object) -> Path:
    if not isinstance(value, str) or not value.strip():
        raise ValueError("each benchmark case requires a source")
    source = (root / value).resolve()
    try:
        source.relative_to(root.resolve())
    except ValueError:
        raise ValueError("benchmark source must stay inside the manifest directory") from None
    if not source.is_file():
        raise ValueError(f"benchmark source does not exist: {value}")
    return source


def load_candidates(path: str | Path) -> list[BenchmarkCandidate]:
    manifest_path = Path(path).resolve()
    manifest = _read_json(manifest_path)
    palettes = manifest.get("palettes")
    variants = manifest.get("variants")
    cases = manifest.get("cases")
    if not isinstance(palettes, dict) or not palettes:
        raise ValueError("benchmark manifest requires palettes")
    if not isinstance(variants, list) or not variants:
        raise ValueError("benchmark manifest requires variants")
    if not isinstance(cases, list) or not cases:
        raise ValueError("benchmark manifest requires cases")

    normalized_palettes: dict[str, tuple[tuple[str, ...], Mapping[str, str]]] = {}
    for palette_id, payload in palettes.items():
        if not isinstance(palette_id, str) or not isinstance(payload, dict):
            raise ValueError("invalid palette entry")
        colors = normalize_palette(payload.get("colors", ()))
        roles = payload.get("roles", {})
        if not isinstance(roles, dict):
            raise ValueError("palette roles must be an object")
        assign_palette_roles(colors, roles)
        normalized_palettes[palette_id] = (colors, roles)

    parsed_variants: list[tuple[str, str]] = []
    seen_variants: set[str] = set()
    for payload in variants:
        if not isinstance(payload, dict):
            raise ValueError("invalid benchmark variant")
        variant_id = payload.get("id")
        suffix = payload.get("instruction_suffix")
        if not isinstance(variant_id, str) or not variant_id or variant_id in seen_variants:
            raise ValueError("benchmark variant ids must be unique strings")
        if not isinstance(suffix, str):
            raise ValueError("benchmark variant requires instruction_suffix")
        seen_variants.add(variant_id)
        parsed_variants.append((variant_id, suffix.strip()))

    result: list[BenchmarkCandidate] = []
    seen_cases: set[str] = set()
    for payload in cases:
        if not isinstance(payload, dict):
            raise ValueError("invalid benchmark case")
        case_id = payload.get("id")
        if not isinstance(case_id, str) or not case_id or case_id in seen_cases:
            raise ValueError("benchmark case ids must be unique strings")
        seen_cases.add(case_id)
        source = _safe_source(manifest_path.parent, payload.get("source"))
        instruction = payload.get("instruction")
        style = payload.get("style", "q_cartoon")
        palette_ids = payload.get("palette_ids", list(normalized_palettes))
        variant_ids = payload.get("variant_ids", [item[0] for item in parsed_variants])
        if not isinstance(instruction, str) or not instruction.strip():
            raise ValueError(f"benchmark case {case_id} requires an instruction")
        if not isinstance(style, str) or not style:
            raise ValueError(f"benchmark case {case_id} requires a style")
        if not isinstance(palette_ids, list) or not all(isinstance(item, str) for item in palette_ids):
            raise ValueError(f"benchmark case {case_id} has invalid palette_ids")
        if not isinstance(variant_ids, list) or not all(isinstance(item, str) for item in variant_ids):
            raise ValueError(f"benchmark case {case_id} has invalid variant_ids")
        suffix_by_id = dict(parsed_variants)
        for palette_id in palette_ids:
            if palette_id not in normalized_palettes:
                raise ValueError(f"unknown palette: {palette_id}")
            colors, roles = normalized_palettes[palette_id]
            for variant_id in variant_ids:
                if variant_id not in suffix_by_id:
                    raise ValueError(f"unknown variant: {variant_id}")
                suffix = suffix_by_id[variant_id]
                result.append(BenchmarkCandidate(
                    case_id=case_id,
                    palette_id=palette_id,
                    variant_id=variant_id,
                    source=source,
                    instruction=(instruction.strip() + ("\n" + suffix if suffix else "")),
                    style=style,
                    colors=colors,
                    role_overrides=roles,
                ))
    return result


def benchmark_score(metrics: Mapping[str, Any]) -> float:
    """Rank printability first while preserving the pipeline's continuous score."""
    value = float(metrics.get("score", 1.0))
    if not metrics.get("palette_quality_ok", False):
        value += 4.0
    subject_ratio = float(metrics.get("printable_subject_area_ratio", 0.0))
    if subject_ratio < 0.18:
        value += (0.18 - subject_ratio) * 4.0
    continuity = float(metrics.get("largest_subject_component_ratio", 0.0))
    if continuity < 0.90:
        value += (0.90 - continuity) * 4.0
    return round(value, 6)


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _atomic_json(path: Path, value: object) -> None:
    part = path.with_suffix(path.suffix + ".part")
    part.write_text(json.dumps(value, ensure_ascii=False, indent=2), encoding="utf-8")
    part.replace(path)


def _run_candidate(
    candidate: BenchmarkCandidate,
    output_root: Path,
    force: bool,
    reprocess_only: bool = False,
) -> dict[str, Any]:
    output = output_root / candidate.candidate_id
    output.mkdir(parents=True, exist_ok=True)
    summary_path = output / "candidate.json"
    if summary_path.is_file() and not force and not reprocess_only:
        cached = _read_json(summary_path)
        cached["resumed"] = True
        return cached

    raw = output / "style_preview_raw.png"
    role_assignment = assign_palette_roles(candidate.colors, candidate.role_overrides)
    provider_prompt = build_style_preview_prompt(
        candidate.instruction,
        candidate.colors,
        candidate.style,
        palette_roles=role_assignment.color_by_role,
    )
    request = {
        "candidate_id": candidate.candidate_id,
        "case_id": candidate.case_id,
        "palette_id": candidate.palette_id,
        "variant_id": candidate.variant_id,
        "source": str(candidate.source),
        "source_sha256": _sha256(candidate.source),
        "instruction": candidate.instruction,
        "provider_prompt": provider_prompt,
        "style": candidate.style,
        "palette": list(candidate.colors),
        "palette_roles": role_assignment.as_metadata(),
    }
    _atomic_json(output / "request.json", request)
    if reprocess_only:
        if not raw.is_file():
            raise FileNotFoundError(f"cached provider image does not exist: {raw}")
    else:
        preprocess_image(
            candidate.source,
            candidate.instruction,
            raw,
            candidate.colors,
            candidate.style,
            palette_roles=role_assignment.color_by_role,
        )
    result = process_printable_image(
        raw,
        output,
        candidate.colors,
        PrintSettings(width_mm=160.0, nozzle_mm=0.4, line_width_mm=0.4, minimum_feature_mm=0.8),
        role_assignment.color_by_role,
    )
    summary = {
        **request,
        "resumed": False,
        "benchmark_score": benchmark_score(result.metrics),
        "metrics": result.metrics,
        "outputs": {
            "raw": raw.name,
            "strict_preview": result.strict_preview.name,
            "clean_preview": result.clean_preview.name,
            "model_reference": result.model_reference.name,
            "metadata": result.metadata.name,
        },
    }
    _atomic_json(summary_path, summary)
    return summary


def write_summary(output_root: Path, rows: Iterable[Mapping[str, Any]]) -> list[dict[str, Any]]:
    ordered = sorted((dict(row) for row in rows), key=lambda row: (row["case_id"], row["benchmark_score"]))
    _atomic_json(output_root / "summary.json", ordered)
    with (output_root / "summary.csv").open("w", encoding="utf-8-sig", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=[
            "case_id", "palette_id", "variant_id", "benchmark_score", "quality_ok",
            "meaningful_colors", "subject_ratio", "continuity", "warnings", "candidate_id",
        ])
        writer.writeheader()
        for row in ordered:
            metrics = row.get("metrics", {})
            writer.writerow({
                "case_id": row["case_id"],
                "palette_id": row["palette_id"],
                "variant_id": row["variant_id"],
                "benchmark_score": row["benchmark_score"],
                "quality_ok": metrics.get("palette_quality_ok"),
                "meaningful_colors": metrics.get("meaningful_subject_color_count"),
                "subject_ratio": metrics.get("printable_subject_area_ratio"),
                "continuity": metrics.get("largest_subject_component_ratio"),
                "warnings": ";".join(metrics.get("quality_warnings", [])),
                "candidate_id": row["candidate_id"],
            })
    return ordered


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--workers", type=int, default=2)
    parser.add_argument("--force", action="store_true")
    parser.add_argument("--reprocess-only", action="store_true")
    parser.add_argument("--confirm-paid-call", action="store_true")
    args = parser.parse_args()
    if not args.confirm_paid_call and not args.reprocess_only:
        parser.error("--confirm-paid-call is required")
    if not 1 <= args.workers <= 4:
        parser.error("--workers must be between 1 and 4")
    candidates = load_candidates(args.manifest)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    rows: list[dict[str, Any]] = []
    with ThreadPoolExecutor(max_workers=args.workers) as pool:
        futures = {
            pool.submit(_run_candidate, candidate, output, args.force, args.reprocess_only): candidate
            for candidate in candidates
        }
        for future in as_completed(futures):
            candidate = futures[future]
            try:
                row = future.result()
            except Exception as exc:
                row = {
                    "candidate_id": candidate.candidate_id,
                    "case_id": candidate.case_id,
                    "palette_id": candidate.palette_id,
                    "variant_id": candidate.variant_id,
                    "benchmark_score": 99.0,
                    "metrics": {"palette_quality_ok": False, "quality_warnings": [type(exc).__name__]},
                    "error": str(exc),
                }
            rows.append(row)
            write_summary(output, rows)
            print(json.dumps({
                "candidate_id": row["candidate_id"],
                "benchmark_score": row["benchmark_score"],
                "error": row.get("error"),
            }, ensure_ascii=False), flush=True)
    write_summary(output, rows)
    return 1 if any(row.get("error") for row in rows) else 0


if __name__ == "__main__":
    raise SystemExit(main())
