#!/usr/bin/env python3
from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import json
import shutil
import sys
from pathlib import Path
from typing import Any

from PIL import Image

import orca_ai_sidecar as sidecar
import tripo_client


DEFAULT_INPUT = Path("generated_models/paid-image-validation/palette-preview-v2-final.png")
DEFAULT_OUTPUT_ROOT = Path("generated_models/paid-tripo-validation")
FACE_LIMITS = (100000, 300000, 500000, 1000000)
PALETTE = (
    "#FFFFFF",
    "#83B771",
    "#EA0006",
    "#000000",
    "#804000",
    "#FFFF0C",
    "#FF0100",
    "#0102FF",
    "#03FF07",
    "#B8B3A7",
    "#242421",
    "#DCDBD7",
    "#CDA07B",
    "#A9462E",
    "#AB7A56",
    "#DAAE8C",
)


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest().upper()


def _write_json(path: Path, value: Any) -> None:
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2), encoding="utf-8")
    temporary.replace(path)


def _check_input(path: Path, palette: tuple[str, ...]) -> dict[str, Any]:
    if not path.is_file():
        raise RuntimeError(f"Input image does not exist: {path}")
    with Image.open(path) as image:
        rgb = image.convert("RGB")
        color_counts = rgb.getcolors(maxcolors=65536)
        colors = {color for _, color in color_counts} if color_counts is not None else set()
        outside: set[tuple[int, int, int]] = set()
        if palette:
            if color_counts is None:
                raise RuntimeError("Input image contains too many distinct colors for printable-palette validation.")
            allowed = {tuple(int(color[index : index + 2], 16) for index in (1, 3, 5)) for color in palette}
            outside = colors - allowed
            if outside:
                raise RuntimeError(f"Input image contains {len(outside)} colors outside the printable palette.")
        return {
            "path": str(path.resolve()),
            "sha256": _sha256(path),
            "width": rgb.width,
            "height": rgb.height,
            "colors_used": len(colors) if color_counts is not None else ">65536",
            "colors_outside_palette": len(outside),
            "palette_constrained": bool(palette),
        }


def _load_state(
    path: Path,
    input_info: dict[str, Any],
    face_limit: int,
    palette: tuple[str, ...],
) -> dict[str, Any]:
    try:
        state = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise RuntimeError("The existing paid-validation state is unreadable; refusing to create another task.") from exc
    if state.get("input_sha256") != input_info["sha256"]:
        raise RuntimeError("The existing paid-validation state belongs to another input; refusing to create another task.")
    if state.get("face_limit") != face_limit or state.get("palette") != list(palette):
        raise RuntimeError("The existing paid-validation state uses different generation settings; refusing another task.")
    task_id = state.get("generation_task_id")
    if not isinstance(task_id, str) or not task_id:
        raise RuntimeError("The existing paid-validation state has no task ID; refusing to create another task.")
    return state


def _task_directory(output_root: Path, task_id: str) -> Path:
    if not task_id or any(character not in "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ-_" for character in task_id):
        raise RuntimeError("Tripo returned an unsafe task ID.")
    return output_root / task_id


def _create_or_resume_generation(
    input_path: Path,
    input_info: dict[str, Any],
    output_root: Path,
    state_path: Path,
    confirm_paid_call: bool,
    face_limit: int,
    palette: tuple[str, ...],
) -> tuple[dict[str, Any], Path]:
    if state_path.exists():
        state = _load_state(state_path, input_info, face_limit, palette)
        task_directory = _task_directory(output_root, state["generation_task_id"])
        task_directory.mkdir(parents=True, exist_ok=True)
        print(f"Resuming Tripo generation task {state['generation_task_id']}", flush=True)
        return state, task_directory

    if not confirm_paid_call:
        raise RuntimeError("Preflight passed. Use --confirm-paid-call to create exactly one paid Tripo task.")

    file_token = tripo_client.upload_image(input_path)
    print("Input uploaded; creating one paid Tripo image-to-model task.", flush=True)
    task_id = tripo_client.create_image_task(file_token, face_limit)
    task_directory = _task_directory(output_root, task_id)
    task_directory.mkdir(parents=True, exist_ok=False)
    shutil.copy2(input_path, task_directory / "input.png")
    state = {
        "input_sha256": input_info["sha256"],
        "face_limit": face_limit,
        "palette": list(palette),
        "generation_task_id": task_id,
        "generation_status": "submitted",
        "conversion_task_id": None,
        "conversion_status": "not_submitted",
    }
    _write_json(state_path, state)
    _write_json(task_directory / "validation-state.json", state)
    print(f"Paid Tripo task submitted: {task_id}", flush=True)
    return state, task_directory


def _record_state(state_path: Path, task_directory: Path, state: dict[str, Any]) -> None:
    _write_json(state_path, state)
    _write_json(task_directory / "validation-state.json", state)


def _inspect_obj(path: Path, palette: tuple[str, ...]) -> dict[str, Any]:
    vertex_count = 0
    faces: list[tuple[int, int, int]] = []
    colors: Counter[str] = Counter()
    with path.open("r", encoding="utf-8", errors="strict") as stream:
        for line in stream:
            fields = line.strip().split()
            if not fields:
                continue
            if fields[0].lower() == "v":
                vertex_count += 1
                if len(fields) in {7, 8}:
                    rgb = tuple(round(float(value) * 255) for value in fields[4:7])
                    colors["#{:02X}{:02X}{:02X}".format(*rgb)] += 1
            elif fields[0].lower() == "f":
                if len(fields) != 4:
                    raise RuntimeError("The generated OBJ contains non-triangular faces.")
                faces.append(
                    tuple(sidecar._resolve_obj_index(value, vertex_count, "vertex") for value in fields[1:])
                )

    parent = list(range(vertex_count))

    def find(index: int) -> int:
        while parent[index] != index:
            parent[index] = parent[parent[index]]
            index = parent[index]
        return index

    def unite(left: int, right: int) -> None:
        left_root, right_root = find(left), find(right)
        if left_root != right_root:
            parent[right_root] = left_root

    referenced: set[int] = set()
    edge_counts: Counter[tuple[int, int]] = Counter()
    degenerate_triangles = 0
    for face in faces:
        if len(set(face)) != 3:
            degenerate_triangles += 1
        referenced.update(face)
        unite(face[0], face[1])
        unite(face[1], face[2])
        for left, right in ((face[0], face[1]), (face[1], face[2]), (face[2], face[0])):
            edge_counts[tuple(sorted((left, right)))] += 1

    components = Counter(find(index) for index in referenced)
    allowed_colors = set(palette)
    displayed_colors = dict(sorted(colors.items())) if palette else dict(colors.most_common(64))
    return {
        "vertices": vertex_count,
        "referenced_vertices": len(referenced),
        "triangle_faces": len(faces),
        "connected_components": len(components),
        "component_vertex_counts": sorted(components.values(), reverse=True),
        "boundary_edges": sum(count == 1 for count in edge_counts.values()),
        "non_manifold_edges": sum(count > 2 for count in edge_counts.values()),
        "degenerate_triangles": degenerate_triangles,
        "vertex_color_count": len(colors),
        "palette_colors_used": displayed_colors,
        "colors_outside_palette": sorted(set(colors) - allowed_colors) if palette else [],
    }


def run(
    input_path: Path,
    output_root: Path,
    confirm_paid_call: bool,
    face_limit: int,
    palette: tuple[str, ...],
) -> Path:
    input_path = input_path.resolve()
    output_root = output_root.resolve()
    output_root.mkdir(parents=True, exist_ok=True)
    input_info = _check_input(input_path, palette)
    _write_json(
        output_root / "input-preflight.json",
        {**input_info, "palette": list(palette), "face_limit": face_limit},
    )
    print(
        f"Input preflight passed: {input_info['width']}x{input_info['height']}, "
        f"{input_info['colors_used']} colors, face limit={face_limit}, SHA256={input_info['sha256']}",
        flush=True,
    )

    state_path = output_root / "validation-state.json"
    state, task_directory = _create_or_resume_generation(
        input_path, input_info, output_root, state_path, confirm_paid_call, face_limit, palette
    )
    generation_id = state["generation_task_id"]

    generation_result = tripo_client.wait_for_task(
        generation_id,
        progress=lambda value: print(f"Generation progress: {value}%", flush=True),
    )
    state["generation_status"] = "success"
    _write_json(task_directory / "generation-result.json", generation_result)
    _record_state(state_path, task_directory, state)

    conversion_id = state.get("conversion_task_id")
    if not conversion_id:
        conversion_id = tripo_client.create_conversion(generation_id, "obj")
        state["conversion_task_id"] = conversion_id
        state["conversion_status"] = "submitted"
        _record_state(state_path, task_directory, state)
        print(f"OBJ conversion submitted: {conversion_id}", flush=True)
    else:
        print(f"Resuming OBJ conversion task {conversion_id}", flush=True)

    conversion_result = tripo_client.wait_for_task(
        conversion_id,
        progress=lambda value: print(f"Conversion progress: {value}%", flush=True),
    )
    state["conversion_status"] = "success"
    _write_json(task_directory / "conversion-result.json", conversion_result)
    _record_state(state_path, task_directory, state)

    raw_download = task_directory / "artifact-raw.download"
    archive = task_directory / "artifact-raw.zip"
    raw_obj = task_directory / "artifact-raw.obj"
    if not raw_download.exists() and not archive.exists() and not raw_obj.exists():
        tripo_client.download_task_artifact(conversion_result, raw_download, sidecar.MAX_ARTIFACT_BYTES)
    artifact = task_directory / "model-vertex-color.obj"
    preparation_error = None
    if not artifact.exists():
        if archive.exists():
            raw_download = archive.with_name("artifact-resume.download")
            shutil.copy2(archive, raw_download)
        elif raw_obj.exists():
            raw_download = raw_obj.with_name("artifact-resume.download")
            shutil.copy2(raw_obj, raw_download)
        try:
            artifact = sidecar._prepare_obj_artifact(raw_download, task_directory, palette)
        except sidecar.TripoError as exc:
            preparation_error = str(exc)
            if not artifact.exists():
                raise

    inspection = _inspect_obj(artifact, palette)
    errors = []
    if preparation_error:
        errors.append(preparation_error)
    if inspection["triangle_faces"] > sidecar.MAX_MODEL_FACES:
        errors.append(
            f"Triangle count {inspection['triangle_faces']} exceeds the {sidecar.MAX_MODEL_FACES} limit."
        )
    if inspection["boundary_edges"] or inspection["non_manifold_edges"]:
        errors.append(
            f"The mesh has {inspection['boundary_edges']} boundary edges and "
            f"{inspection['non_manifold_edges']} non-manifold edges."
        )
    if inspection["degenerate_triangles"]:
        errors.append(f"The mesh has {inspection['degenerate_triangles']} degenerate triangles.")
    if palette and inspection["colors_outside_palette"]:
        errors.append("The mesh contains vertex colors outside the printable palette.")
    if not inspection["palette_colors_used"]:
        errors.append("The mesh does not contain vertex RGB colors.")

    validation = {
        "ok": not errors,
        "generation_task_id": generation_id,
        "conversion_task_id": conversion_id,
        "face_limit": face_limit,
        "palette_constrained": bool(palette),
        "artifact": str(artifact),
        "artifact_size": artifact.stat().st_size,
        "artifact_sha256": _sha256(artifact),
        **inspection,
        "errors": errors,
    }
    _write_json(task_directory / "validation-result.json", validation)
    state["validation_status"] = "success" if validation["ok"] else "failed"
    _record_state(state_path, task_directory, state)
    print(json.dumps(validation, ensure_ascii=False, indent=2), flush=True)
    if errors:
        raise RuntimeError("Generated OBJ failed local printability gates; see validation-result.json.")
    if palette:
        sidecar._validate_obj_palette(artifact, palette)
    sidecar._validate_obj_topology(artifact)
    sidecar._validate_artifact(artifact, "obj")
    return artifact


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run one resumable paid Tripo high-detail OBJ validation.")
    parser.add_argument("--input", type=Path, default=DEFAULT_INPUT)
    parser.add_argument("--output-root", type=Path, default=DEFAULT_OUTPUT_ROOT)
    parser.add_argument("--preflight-only", action="store_true")
    parser.add_argument("--confirm-paid-call", action="store_true")
    parser.add_argument("--face-limit", type=int, choices=FACE_LIMITS, default=300000)
    parser.add_argument("--natural-color", action="store_true")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    palette = () if args.natural_color else PALETTE
    try:
        if args.preflight_only:
            info = _check_input(args.input.resolve(), palette)
            print(
                json.dumps(
                    {**info, "palette": list(palette), "face_limit": args.face_limit},
                    ensure_ascii=False,
                    indent=2,
                )
            )
            return 0
        run(args.input, args.output_root, args.confirm_paid_call, args.face_limit, palette)
    except (RuntimeError, tripo_client.TripoError, sidecar.TripoError) as exc:
        print(f"Validation failed: {exc}", file=sys.stderr, flush=True)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
