#!/usr/bin/env python3
"""Run one resumable paid Tripo texture-only validation on existing geometry."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import sys
from typing import Any, Callable


TOOLS_AI = Path(__file__).resolve().parent
if str(TOOLS_AI) not in sys.path:
    sys.path.insert(0, str(TOOLS_AI))

import orca_ai_sidecar as sidecar  # noqa: E402
from printable_palette import PrintablePaletteError, normalize_palette  # noqa: E402
import run_paid_tripo_validation as paid_tripo  # noqa: E402
import tripo_client  # noqa: E402


SCHEMA_VERSION = 1
TEXTURE_ALIGNMENTS = ("original_image", "geometry")
TEXTURE_QUALITIES = ("standard", "detailed", "extreme")
TEXTURE_VIEW_ORDER = ("front", "left", "back", "right")


def _fingerprint(
    source_task_id: str,
    input_info: dict[str, Any],
    face_limit: int,
    palette: tuple[str, ...],
    texture_alignment: str,
    texture_quality: str,
    texture_seed: int | None,
) -> str:
    value = {
        "source_task_id": source_task_id,
        "input_sha256": input_info["sha256"],
        "face_limit": face_limit,
        "palette": list(palette),
        "texture_alignment": texture_alignment,
        "texture_quality": texture_quality,
        "texture_seed": texture_seed,
    }
    payload = json.dumps(value, sort_keys=True, separators=(",", ":")).encode("utf-8")
    return hashlib.sha256(payload).hexdigest().upper()


def _validate_source_task_id(value: str) -> str:
    candidate = value.strip() if isinstance(value, str) else ""
    if not candidate or any(character not in "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ-_" for character in candidate):
        raise RuntimeError("The source Tripo task ID is invalid.")
    return candidate


def _multiview_fingerprint(
    source_task_id: str,
    input_infos: tuple[dict[str, Any], ...],
    face_limit: int,
    palette: tuple[str, ...],
    texture_alignment: str,
    texture_quality: str,
    texture_seed: int | None,
) -> str:
    value = {
        "source_task_id": source_task_id,
        "input_sha256s": [info["sha256"] for info in input_infos],
        "face_limit": face_limit,
        "palette": list(palette),
        "texture_alignment": texture_alignment,
        "texture_quality": texture_quality,
        "texture_seed": texture_seed,
    }
    payload = json.dumps(value, sort_keys=True, separators=(",", ":")).encode("utf-8")
    return hashlib.sha256(payload).hexdigest().upper()


def _create_or_resume_texture(
    input_path: Path,
    input_info: dict[str, Any],
    output_root: Path,
    state_path: Path,
    *,
    source_task_id: str,
    confirm_paid_call: bool,
    face_limit: int,
    palette: tuple[str, ...],
    texture_alignment: str,
    texture_quality: str,
    texture_seed: int | None,
    uploader: Callable[[Path], str] = tripo_client.upload_image,
    creator: Callable[..., str] = tripo_client.create_texture_task,
) -> tuple[dict[str, Any], Path]:
    fingerprint = _fingerprint(
        source_task_id, input_info, face_limit, palette,
        texture_alignment, texture_quality, texture_seed,
    )
    if state_path.is_file():
        state = paid_tripo._read_json(state_path) if hasattr(paid_tripo, "_read_json") else json.loads(
            state_path.read_text(encoding="utf-8")
        )
        if state.get("fingerprint") != fingerprint:
            raise RuntimeError("The output directory belongs to another frozen texture request.")
        task_id = state.get("generation_task_id")
        if not isinstance(task_id, str) or not task_id:
            raise RuntimeError("The previous texture creation outcome is ambiguous; refusing another paid task.")
        task_directory = paid_tripo._task_directory(output_root, task_id)
        task_directory.mkdir(parents=True, exist_ok=True)
        return state, task_directory

    if not confirm_paid_call:
        raise RuntimeError("Preflight passed. Use --confirm-paid-call to create exactly one paid Tripo texture task.")

    image_token = uploader(input_path)
    state: dict[str, Any] = {
        "schema_version": SCHEMA_VERSION,
        "fingerprint": fingerprint,
        "source_task_id": source_task_id,
        "input_sha256": input_info["sha256"],
        "face_limit": face_limit,
        "palette": list(palette),
        "texture_alignment": texture_alignment,
        "texture_quality": texture_quality,
        "texture_seed": texture_seed,
        "generation_task_id": None,
        "generation_status": "creating",
        "conversion_task_id": None,
        "conversion_status": "not_submitted",
    }
    paid_tripo._write_json(state_path, state)
    try:
        task_id = creator(
            source_task_id,
            image_token,
            texture_alignment=texture_alignment,
            texture_quality=texture_quality,
            texture_seed=texture_seed,
        )
    except tripo_client.TripoError:
        state["generation_status"] = "creation_failed_or_ambiguous"
        paid_tripo._write_json(state_path, state)
        raise
    state["generation_task_id"] = task_id
    state["generation_status"] = "submitted"
    # Persist the paid task ID before any non-essential filesystem work.
    paid_tripo._write_json(state_path, state)
    task_directory = paid_tripo._task_directory(output_root, task_id)
    task_directory.mkdir(parents=True, exist_ok=True)
    destination = task_directory / "texture-reference.png"
    if not destination.exists():
        shutil.copy2(input_path, destination)
    paid_tripo._write_json(task_directory / "validation-state.json", state)
    return state, task_directory


def _create_or_resume_multiview_texture(
    input_paths: tuple[Path, ...],
    input_infos: tuple[dict[str, Any], ...],
    output_root: Path,
    state_path: Path,
    *,
    source_task_id: str,
    confirm_paid_call: bool,
    face_limit: int,
    palette: tuple[str, ...],
    texture_alignment: str,
    texture_quality: str,
    texture_seed: int | None,
    uploader: Callable[[Path], str] = tripo_client.upload_image,
    creator: Callable[..., str] = tripo_client.create_texture_task,
) -> tuple[dict[str, Any], Path]:
    if len(input_paths) != 4 or len(input_infos) != 4:
        raise RuntimeError("Multiview texturing requires front, left, back, and right references.")
    fingerprint = _multiview_fingerprint(
        source_task_id, input_infos, face_limit, palette,
        texture_alignment, texture_quality, texture_seed,
    )
    if state_path.is_file():
        state = json.loads(state_path.read_text(encoding="utf-8"))
        if state.get("fingerprint") != fingerprint:
            raise RuntimeError("The output directory belongs to another frozen texture request.")
        task_id = state.get("generation_task_id")
        if not isinstance(task_id, str) or not task_id:
            raise RuntimeError("The previous texture creation outcome is ambiguous; refusing another paid task.")
        task_directory = paid_tripo._task_directory(output_root, task_id)
        task_directory.mkdir(parents=True, exist_ok=True)
        return state, task_directory

    if not confirm_paid_call:
        raise RuntimeError("Preflight passed. Use --confirm-paid-call to create exactly one paid Tripo texture task.")

    image_tokens = [uploader(path) for path in input_paths]
    state: dict[str, Any] = {
        "schema_version": SCHEMA_VERSION,
        "fingerprint": fingerprint,
        "source_task_id": source_task_id,
        "input_sha256": input_infos[0]["sha256"],
        "input_sha256s": [info["sha256"] for info in input_infos],
        "texture_views": list(TEXTURE_VIEW_ORDER),
        "face_limit": face_limit,
        "palette": list(palette),
        "texture_alignment": texture_alignment,
        "texture_quality": texture_quality,
        "texture_seed": texture_seed,
        "generation_task_id": None,
        "generation_status": "creating",
        "conversion_task_id": None,
        "conversion_status": "not_submitted",
    }
    paid_tripo._write_json(state_path, state)
    try:
        task_id = creator(
            source_task_id,
            image_tokens,
            texture_alignment=texture_alignment,
            texture_quality=texture_quality,
            texture_seed=texture_seed,
        )
    except tripo_client.TripoError:
        state["generation_status"] = "creation_failed_or_ambiguous"
        paid_tripo._write_json(state_path, state)
        raise
    state["generation_task_id"] = task_id
    state["generation_status"] = "submitted"
    paid_tripo._write_json(state_path, state)
    task_directory = paid_tripo._task_directory(output_root, task_id)
    task_directory.mkdir(parents=True, exist_ok=True)
    for view, input_path in zip(TEXTURE_VIEW_ORDER, input_paths):
        destination = task_directory / f"texture-reference-{view}.png"
        if not destination.exists():
            shutil.copy2(input_path, destination)
    paid_tripo._write_json(task_directory / "validation-state.json", state)
    return state, task_directory


def run(
    input_path: Path,
    output_root: Path,
    *,
    source_task_id: str,
    confirm_paid_call: bool,
    face_limit: int,
    palette: tuple[str, ...],
    texture_alignment: str,
    texture_quality: str,
    texture_seed: int | None,
) -> Path:
    source_id = _validate_source_task_id(source_task_id)
    source = input_path.resolve()
    output = output_root.resolve()
    output.mkdir(parents=True, exist_ok=True)
    # The provider reference intentionally retains sculptural shading and is
    # not required to contain exact filament colors.
    input_info = paid_tripo._check_input(source, ())
    preflight = {
        **input_info,
        "source_task_id": source_id,
        "face_limit": face_limit,
        "palette": list(palette),
        "texture_alignment": texture_alignment,
        "texture_quality": texture_quality,
        "texture_seed": texture_seed,
    }
    paid_tripo._write_json(output / "input-preflight.json", preflight)
    state_path = output / "validation-state.json"
    state, task_directory = _create_or_resume_texture(
        source,
        input_info,
        output,
        state_path,
        source_task_id=source_id,
        confirm_paid_call=confirm_paid_call,
        face_limit=face_limit,
        palette=palette,
        texture_alignment=texture_alignment,
        texture_quality=texture_quality,
        texture_seed=texture_seed,
    )
    return paid_tripo.complete_generation(state, state_path, task_directory, face_limit, palette)


def run_multiview(
    input_paths: tuple[Path, ...],
    output_root: Path,
    *,
    source_task_id: str,
    confirm_paid_call: bool,
    face_limit: int,
    palette: tuple[str, ...],
    texture_alignment: str,
    texture_quality: str,
    texture_seed: int | None,
) -> Path:
    source_id = _validate_source_task_id(source_task_id)
    sources = tuple(path.resolve() for path in input_paths)
    if len(sources) != 4:
        raise RuntimeError("Multiview texturing requires front, left, back, and right references.")
    output = output_root.resolve()
    output.mkdir(parents=True, exist_ok=True)
    input_infos = tuple(paid_tripo._check_input(source, ()) for source in sources)
    preflight = {
        "source_task_id": source_id,
        "inputs": [
            {"view": view, **info}
            for view, info in zip(TEXTURE_VIEW_ORDER, input_infos)
        ],
        "face_limit": face_limit,
        "palette": list(palette),
        "texture_alignment": texture_alignment,
        "texture_quality": texture_quality,
        "texture_seed": texture_seed,
    }
    paid_tripo._write_json(output / "input-preflight.json", preflight)
    state_path = output / "validation-state.json"
    state, task_directory = _create_or_resume_multiview_texture(
        sources,
        input_infos,
        output,
        state_path,
        source_task_id=source_id,
        confirm_paid_call=confirm_paid_call,
        face_limit=face_limit,
        palette=palette,
        texture_alignment=texture_alignment,
        texture_quality=texture_quality,
        texture_seed=texture_seed,
    )
    return paid_tripo.complete_generation(state, state_path, task_directory, face_limit, palette)


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-task-id", required=True)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--input-left", type=Path)
    parser.add_argument("--input-back", type=Path)
    parser.add_argument("--input-right", type=Path)
    parser.add_argument("--output-root", type=Path, required=True)
    parser.add_argument("--preflight-only", action="store_true")
    parser.add_argument("--confirm-paid-call", action="store_true")
    parser.add_argument("--face-limit", type=int, choices=paid_tripo.FACE_LIMITS, default=1000000)
    parser.add_argument("--texture-alignment", choices=TEXTURE_ALIGNMENTS, default="original_image")
    parser.add_argument("--texture-quality", choices=TEXTURE_QUALITIES, default="detailed")
    parser.add_argument("--texture-seed", type=int)
    parser.add_argument("--natural-color", action="store_true")
    parser.add_argument("--palette", help="Comma-separated printable colors in #RRGGBB form.")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    if args.natural_color and args.palette:
        print("Validation failed: --natural-color and --palette cannot be combined.", file=sys.stderr)
        return 1
    try:
        source_task_id = _validate_source_task_id(args.source_task_id)
        palette = (
            () if args.natural_color else
            normalize_palette(args.palette.split(",")) if args.palette else paid_tripo.PALETTE
        )
        optional_views = (args.input_left, args.input_back, args.input_right)
        if any(optional_views) and not all(optional_views):
            raise RuntimeError("Provide all of --input-left, --input-back, and --input-right together.")
        multiview_inputs = (
            (args.input, args.input_left, args.input_back, args.input_right)
            if all(optional_views) else None
        )
        input_info = paid_tripo._check_input(args.input.resolve(), ())
        if args.preflight_only:
            if multiview_inputs:
                input_infos = [paid_tripo._check_input(path.resolve(), ()) for path in multiview_inputs]
                value = {
                    "source_task_id": source_task_id,
                    "inputs": [
                        {"view": view, **info}
                        for view, info in zip(TEXTURE_VIEW_ORDER, input_infos)
                    ],
                    "face_limit": args.face_limit,
                    "palette": list(palette),
                    "texture_alignment": args.texture_alignment,
                    "texture_quality": args.texture_quality,
                    "texture_seed": args.texture_seed,
                }
            else:
                value = {
                    **input_info,
                    "source_task_id": source_task_id,
                    "face_limit": args.face_limit,
                    "palette": list(palette),
                    "texture_alignment": args.texture_alignment,
                    "texture_quality": args.texture_quality,
                    "texture_seed": args.texture_seed,
                }
            print(json.dumps(value, ensure_ascii=False, indent=2))
            return 0
        if multiview_inputs:
            artifact = run_multiview(
                multiview_inputs,
                args.output_root,
                source_task_id=source_task_id,
                confirm_paid_call=args.confirm_paid_call,
                face_limit=args.face_limit,
                palette=palette,
                texture_alignment=args.texture_alignment,
                texture_quality=args.texture_quality,
                texture_seed=args.texture_seed,
            )
        else:
            artifact = run(
                args.input,
                args.output_root,
                source_task_id=source_task_id,
                confirm_paid_call=args.confirm_paid_call,
                face_limit=args.face_limit,
                palette=palette,
                texture_alignment=args.texture_alignment,
                texture_quality=args.texture_quality,
                texture_seed=args.texture_seed,
            )
        print(str(artifact), flush=True)
    except (RuntimeError, PrintablePaletteError, tripo_client.TripoError, sidecar.TripoError) as exc:
        print(f"Validation failed: {exc}", file=sys.stderr, flush=True)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
