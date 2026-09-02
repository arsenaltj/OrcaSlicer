#!/usr/bin/env python3
"""Build deterministic structural and five-view reports for a frozen Tripo wave."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import sys
from typing import Any

from PIL import Image, ImageDraw


TOOLS_AI = Path(__file__).resolve().parent
REPOSITORY_ROOT = TOOLS_AI.parents[1]
if str(TOOLS_AI) not in sys.path:
    sys.path.insert(0, str(TOOLS_AI))

from printable_model_quality import (  # noqa: E402
    ModelQualityThresholds,
    analyze_printable_obj,
    write_model_quality_report,
)
from printable_model_views import render_model_views  # noqa: E402


class TripoQualityReportError(RuntimeError):
    pass


def _read_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise TripoQualityReportError(f"Cannot read JSON: {path}") from exc
    if not isinstance(value, dict):
        raise TripoQualityReportError(f"JSON must be an object: {path}")
    return value


def _write_json(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".part")
    temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2), encoding="utf-8")
    os.replace(temporary, path)


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while block := stream.read(1024 * 1024):
            digest.update(block)
    return digest.hexdigest().upper()


def write_overview(rows: list[dict[str, Any]], output: Path) -> Path:
    """Write a compact two-row visual index from the frozen five-view sheets."""

    if not rows:
        raise TripoQualityReportError("Cannot create an overview without quality rows.")
    columns = min(5, len(rows))
    row_count = (len(rows) + columns - 1) // columns
    cell_width, cell_height = 340, 250
    overview = Image.new("RGB", (columns * cell_width, row_count * cell_height), "#F4F6F7")
    draw = ImageDraw.Draw(overview)
    status_colors = {"pass": "#2A8C62", "review": "#C6841C", "reject": "#C34F45"}
    for index, row in enumerate(rows):
        sheet_path = REPOSITORY_ROOT / Path(str(row["view_sheet"]))
        if not sheet_path.is_file():
            raise TripoQualityReportError(f"Missing view sheet: {sheet_path}")
        with Image.open(sheet_path) as opened:
            sheet = opened.convert("RGB")
            # The isometric panel occupies the lower middle third of the
            # deterministic five-view sheet and is the clearest compact index.
            crop = sheet.crop((sheet.width // 3, sheet.height // 2, sheet.width * 2 // 3, sheet.height))
            crop.thumbnail((cell_width - 20, cell_height - 58), Image.Resampling.LANCZOS)
        column, grid_row = index % columns, index // columns
        left, top = column * cell_width, grid_row * cell_height
        overview.paste(crop, (left + (cell_width - crop.width) // 2, top + 42))
        status = str(row.get("quality_status", "unknown"))
        draw.rectangle((left, top, left + cell_width - 1, top + 5), fill=status_colors.get(status, "#6B7478"))
        title = f"{index + 1:02d}  {row['id']}"
        detail = f"{row.get('profile', '?')}  {int(row.get('face_count') or 0) // 1000}k faces  {status}"
        draw.text((left + 10, top + 10), title, fill="#172126")
        draw.text((left + 10, top + 25), detail, fill="#536067")
        draw.rectangle((left, top, left + cell_width - 1, top + cell_height - 1), outline="#D5DBDE")
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_name(output.name + ".part")
    overview.save(temporary, format="PNG")
    os.replace(temporary, output)
    return output


def load_manifest(path: Path) -> list[dict[str, Any]]:
    manifest = _read_json(path)
    expected = manifest.get("expected_paid_generation_tasks")
    cases = manifest.get("cases")
    if not isinstance(expected, int) or expected <= 0 or not isinstance(cases, list) or len(cases) != expected:
        raise TripoQualityReportError("Manifest task count is inconsistent.")
    identifiers: set[str] = set()
    hashes: set[str] = set()
    result: list[dict[str, Any]] = []
    for value in cases:
        if not isinstance(value, dict):
            raise TripoQualityReportError("Each manifest case must be an object.")
        case_id = value.get("id")
        digest = value.get("sha256")
        palette = value.get("palette")
        if not isinstance(case_id, str) or not case_id or case_id in identifiers:
            raise TripoQualityReportError("Manifest case IDs must be unique non-empty strings.")
        if not isinstance(digest, str) or len(digest) != 64 or digest in hashes:
            raise TripoQualityReportError("Manifest input hashes must be unique SHA-256 values.")
        if value.get("manual_approved") is not True or not isinstance(palette, list):
            raise TripoQualityReportError(f"Case {case_id} is not frozen and approved.")
        identifiers.add(case_id)
        hashes.add(digest)
        result.append(value)
    return result


def _root_states(tripo_root: Path) -> dict[str, tuple[Path, dict[str, Any]]]:
    states: dict[str, tuple[Path, dict[str, Any]]] = {}
    for state_path in sorted(tripo_root.glob("*/validation-state.json")):
        state = _read_json(state_path)
        digest = state.get("input_sha256")
        if not isinstance(digest, str) or not digest or digest in states:
            raise TripoQualityReportError("Tripo root states contain missing or duplicate input hashes.")
        states[digest.upper()] = (state_path.parent, state)
    return states


def analyze_case(case: dict[str, Any], root_states: dict[str, tuple[Path, dict[str, Any]]]) -> dict[str, Any]:
    case_id = str(case["id"])
    digest = str(case["sha256"]).upper()
    matched = root_states.get(digest)
    if matched is None:
        raise TripoQualityReportError(f"Case {case_id} has no matching Tripo root state.")
    output_root, state = matched
    task_id = state.get("generation_task_id")
    if not isinstance(task_id, str) or not task_id:
        raise TripoQualityReportError(f"Case {case_id} has no saved generation task ID.")
    task_directory = output_root / task_id
    artifact = task_directory / "model-vertex-color.obj"
    if not artifact.is_file():
        raise TripoQualityReportError(f"Case {case_id} has no downloaded OBJ artifact.")

    palette = tuple(str(color).upper() for color in case["palette"])
    thresholds = ModelQualityThresholds(max_faces=max(1_000_000, int(case["face_limit"])))
    quality = analyze_printable_obj(artifact, thresholds, target_palette=palette)
    write_model_quality_report(quality, task_directory / "model-quality.json")
    views = render_model_views(artifact, task_directory / "review", force=True)
    validation_path = task_directory / "validation-result.json"
    validation = _read_json(validation_path) if validation_path.is_file() else {}
    metrics = quality.get("metrics", {})
    return {
        "id": case_id,
        "generation_task_id": task_id,
        "profile": case["profile"],
        "face_limit": case["face_limit"],
        "input_sha256": digest,
        "artifact": str(artifact.relative_to(REPOSITORY_ROOT)),
        "artifact_sha256": _sha256(artifact),
        "artifact_size": artifact.stat().st_size,
        "validation_ok": bool(validation.get("ok", False)),
        "quality_status": quality.get("status"),
        "quality_errors": quality.get("errors", []),
        "quality_warnings": quality.get("warnings", []),
        "face_count": metrics.get("face_count"),
        "component_count": metrics.get("component_count"),
        "largest_component_face_ratio": metrics.get("largest_component_face_ratio"),
        "floating_component_count": metrics.get("floating_component_count"),
        "boundary_edges": metrics.get("boundary_edges"),
        "non_manifold_edges": metrics.get("non_manifold_edges"),
        "bed_contact_area_ratio": metrics.get("bed_contact_area_ratio"),
        "thin_local_region_count": metrics.get("thin_local_region_count"),
        "meaningful_target_palette_color_count": metrics.get("meaningful_target_palette_color_count"),
        "view_sheet": str((task_directory / "review" / str(views["sheet"])).relative_to(REPOSITORY_ROOT)),
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--tripo-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--case", action="append", default=[])
    args = parser.parse_args(argv)

    try:
        all_cases = load_manifest(args.manifest.resolve())
        cases = all_cases
        if args.case:
            wanted = set(args.case)
            cases = [case for case in cases if case["id"] in wanted]
            missing = wanted - {str(case["id"]) for case in cases}
            if missing:
                raise TripoQualityReportError("Unknown cases: " + ", ".join(sorted(missing)))
        states = _root_states(args.tripo_root.resolve())
        previous = _read_json(args.output.resolve()) if args.output.is_file() else {"schema_version": 1, "cases": []}
        rows = {str(row["id"]): row for row in previous.get("cases", []) if isinstance(row, dict) and "id" in row}
        for case in cases:
            row = analyze_case(case, states)
            rows[str(case["id"])] = row
            _write_json(args.output.resolve(), {"schema_version": 1, "cases": list(rows.values())})
            print(json.dumps(row, ensure_ascii=False), flush=True)
        ordered_rows = [rows[str(case["id"])] for case in all_cases if str(case["id"]) in rows]
        if len(ordered_rows) == len(all_cases):
            write_overview(ordered_rows, args.output.resolve().with_name("quality-overview.png"))
        return 0
    except (OSError, ValueError, TripoQualityReportError) as exc:
        print(f"Quality report failed: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
