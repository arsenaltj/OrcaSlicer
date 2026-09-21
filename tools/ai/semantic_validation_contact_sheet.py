"""Build comparable six-view sheets from a semantic validation run."""

from __future__ import annotations

import argparse
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


VIEWS = ("front", "back", "left", "right", "top", "bottom")
STAGES = ("original", "baseline", "candidate")


def make_sheet(model_dir: Path, output_dir: Path, size: int) -> None:
    model = model_dir.name
    margin, heading, row_label = 12, 28, 26
    width = margin * 2 + size * len(STAGES)
    height = heading + len(VIEWS) * (row_label + size) + margin
    sheet = Image.new("RGB", (width, height), "white")
    draw = ImageDraw.Draw(sheet)
    font = ImageFont.load_default()
    for column, stage in enumerate(STAGES):
        draw.text((margin + column * size + 8, 8), stage.upper(), fill="black", font=font)
    for row, view in enumerate(VIEWS):
        y = heading + row * (row_label + size)
        draw.text((margin + 8, y + 5), f"{model} / {view}", fill="black", font=font)
        for column, stage in enumerate(STAGES):
            source = model_dir / f"{view}-{stage}.ppm"
            with Image.open(source) as opened:
                if opened.width != opened.height:
                    raise ValueError(f"Expected a square fixed-view render: {source}")
                tile = opened.convert("RGB").resize((size, size), Image.Resampling.LANCZOS)
            sheet.paste(tile, (margin + column * size, y + row_label))
    sheet.save(output_dir / f"{model}-six-views.png")

    for view in ("front", "left", "right"):
        for subject in ("skin-boundaries", "eyes-brows", "lips"):
            for stage in STAGES:
                source = model_dir / f"{view}-{subject}-4x-{stage}.ppm"
                if source.is_file():
                    with Image.open(source) as opened:
                        opened.save(output_dir / f"{model}-{view}-{subject}-{stage}.png")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--tile-size", type=int, default=512)
    args = parser.parse_args()
    if not 128 <= args.tile_size <= 1024:
        parser.error("--tile-size must be between 128 and 1024")
    if not (args.model_dir / "result.json").is_file():
        parser.error("The model run has not finished writing result.json")
    args.output.mkdir(parents=True, exist_ok=True)
    make_sheet(args.model_dir, args.output, args.tile_size)


if __name__ == "__main__":
    main()
