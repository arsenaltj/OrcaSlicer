"""Report material changes between the C1 and B1 fixed-camera renders."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

from PIL import Image, ImageChops


VIEWS = ("front", "back", "left", "right", "top", "bottom")


def compare(model_dir: Path, output_dir: Path, view: str) -> dict:
    with Image.open(model_dir / f"{view}-baseline.ppm") as image:
        baseline = image.convert("RGB")
    with Image.open(model_dir / f"{view}-candidate.ppm") as image:
        candidate = image.convert("RGB")
    if baseline.size != candidate.size:
        raise ValueError(f"Different C1/B1 dimensions in {model_dir.name}/{view}")
    changed = ImageChops.difference(baseline, candidate).convert("L").point(lambda value: 255 if value else 0)
    pixels = sum(value != 0 for value in changed.get_flattened_data())
    overlay = candidate.copy()
    overlay.paste((255, 0, 0), mask=changed)
    overlay.save(output_dir / f"{model_dir.name}-{view}-changes.png")
    return {"view": view, "changed_pixels": pixels, "image_pixels": baseline.width * baseline.height,
            "changed_percent": round(100 * pixels / (baseline.width * baseline.height), 4),
            "changed_bbox": changed.getbbox()}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    report = {"schema": "orca.semantic-stage-diff/v1", "model": args.model_dir.name,
              "note": "Red marks changed display pixels; it is not a correctness score.",
              "views": [compare(args.model_dir, args.output, view) for view in VIEWS]}
    (args.output / f"{args.model_dir.name}-changes.json").write_text(
        json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    print(args.model_dir.name, [(row["view"], row["changed_pixels"]) for row in report["views"]])


if __name__ == "__main__":
    main()
