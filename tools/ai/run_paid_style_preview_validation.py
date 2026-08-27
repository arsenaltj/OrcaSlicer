#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path

from PIL import Image

import openai_preprocessor


STYLES = ("sculpture", "realistic", "cartoon")
INSTRUCTION = (
    "Restyle only visible source content. Preserve the exact framing and visible body extent. "
    "Do not add, remove, reveal, reconstruct, crop, zoom, outpaint, recenter, or reposition anything."
)


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest().upper()


def main() -> int:
    parser = argparse.ArgumentParser(description="Run one paid natural-color style preview validation.")
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--style", required=True, choices=STYLES)
    parser.add_argument(
        "--output-root",
        type=Path,
        default=Path("generated_models/phase30-paid-style-previews"),
    )
    parser.add_argument("--confirm-paid-call", action="store_true")
    args = parser.parse_args()

    source = args.input.resolve()
    if not source.is_file():
        parser.error(f"input does not exist: {source}")
    if not args.confirm_paid_call:
        parser.error("--confirm-paid-call is required")

    output_dir = args.output_root.resolve() / args.style
    output = output_dir / "preview.png"
    manifest = output_dir / "validation.json"
    if output.exists() or manifest.exists():
        parser.error(f"refusing a duplicate paid call because output already exists: {output_dir}")

    output_dir.mkdir(parents=True, exist_ok=False)
    try:
        openai_preprocessor.preprocess_image(source, INSTRUCTION, output, (), args.style)
        with Image.open(output) as image:
            width, height = image.size
        result = {
            "style": args.style,
            "palette_constrained": False,
            "input": str(source),
            "input_sha256": _sha256(source),
            "output": str(output),
            "output_sha256": _sha256(output),
            "width": width,
            "height": height,
            "instruction": INSTRUCTION,
            "paid_image_calls": 1,
            "paid_tripo_calls": 0,
        }
        manifest.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")
        print(json.dumps(result, ensure_ascii=False, indent=2))
        return 0
    except Exception:
        output.unlink(missing_ok=True)
        try:
            output_dir.rmdir()
        except OSError:
            pass
        raise


if __name__ == "__main__":
    sys.exit(main())
