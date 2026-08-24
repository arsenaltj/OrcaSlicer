#!/usr/bin/env python3
"""Create one paid Image2 sample and validate the printable fixed-palette pipeline."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys


TOOLS_AI = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS_AI))

from openai_preprocessor import generate_image  # noqa: E402
from printable_image_pipeline import PrintSettings, process_printable_image  # noqa: E402


DEFAULT_PALETTE = ("#D93632", "#3B8C54", "#315CA8", "#F2F1EA")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--confirm-paid-call", action="store_true")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--prompt", default="一只正在奔跑的机械麒麟，全身清晰，Q版拼色玩具风格")
    args = parser.parse_args()
    if not args.confirm_paid_call:
        parser.error("--confirm-paid-call is required")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    raw = output / "ai_raw.png"
    generate_image(args.prompt, raw, DEFAULT_PALETTE, "q_cartoon", "blue")
    result = process_printable_image(
        raw,
        output,
        DEFAULT_PALETTE,
        PrintSettings(width_mm=160.0, nozzle_mm=0.4, line_width_mm=0.4, minimum_feature_mm=0.8),
    )
    print(json.dumps({
        "raw": str(raw),
        "strict_preview": str(result.strict_preview),
        "clean_preview": str(result.clean_preview),
        "metadata": str(result.metadata),
        "metrics": result.metrics,
        "palette_usage": result.palette_usage,
    }, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
