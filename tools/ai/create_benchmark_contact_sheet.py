"""Create compact visual QA sheets from a quality benchmark summary."""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path

from PIL import Image, ImageDraw, ImageOps


def _checker(size: tuple[int, int], cell: int = 16) -> Image.Image:
    image = Image.new("RGB", size, "#F5F5F5")
    draw = ImageDraw.Draw(image)
    for y in range(0, size[1], cell):
        for x in range(0, size[0], cell):
            if (x // cell + y // cell) % 2:
                draw.rectangle((x, y, min(x + cell - 1, size[0] - 1), min(y + cell - 1, size[1] - 1)), fill="#D8D8D8")
    return image


def create_sheet(summary: Path, output: Path, output_key: str, columns: int = 4) -> None:
    rows = json.loads(summary.read_text(encoding="utf-8"))
    if not isinstance(rows, list) or not rows:
        raise ValueError("summary must contain benchmark rows")
    tile_width, image_height, label_height = 300, 260, 54
    tile_height = image_height + label_height
    sheet = Image.new("RGB", (columns * tile_width, math.ceil(len(rows) / columns) * tile_height), "white")
    draw = ImageDraw.Draw(sheet)
    for index, row in enumerate(rows):
        candidate_dir = summary.parent / row["candidate_id"]
        filename = row["outputs"][output_key]
        with Image.open(candidate_dir / filename) as opened:
            image = opened.convert("RGBA")
            preview = ImageOps.contain(image, (tile_width - 16, image_height - 16), Image.Resampling.LANCZOS)
            background = _checker((tile_width - 8, image_height - 8))
            offset = ((background.width - preview.width) // 2, (background.height - preview.height) // 2)
            background.paste(preview, offset, preview)
        left = (index % columns) * tile_width
        top = (index // columns) * tile_height
        sheet.paste(background, (left + 4, top + 4))
        label = f"{row['case_id']} | {row['palette_id']} | {row['variant_id']}\nscore {row['benchmark_score']:.4f}"
        draw.multiline_text((left + 8, top + image_height + 4), label, fill="black", spacing=3)
    output.parent.mkdir(parents=True, exist_ok=True)
    sheet.save(output, format="PNG", optimize=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--summary", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--stage", choices=("raw", "clean"), default="raw")
    args = parser.parse_args()
    create_sheet(args.summary.resolve(), args.output.resolve(), "raw" if args.stage == "raw" else "clean_preview")
    print(args.output.resolve())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
