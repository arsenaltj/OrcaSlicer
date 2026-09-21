"""Export MobileSAM's TinyViT image encoder for offline boundary evaluation."""

from __future__ import annotations

import argparse
import json
from pathlib import Path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True, type=Path,
                        help="Fixed MobileSAM source checkout")
    parser.add_argument("--checkpoint", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--opset", type=int, default=17)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.opset < 17:
        raise ValueError("TinyViT export requires ONNX opset 17 or newer")
    import sys
    sys.path.insert(0, str(args.source.resolve()))

    import onnx
    import torch
    from mobile_sam import sam_model_registry

    torch.manual_seed(0)
    model = sam_model_registry["vit_t"](checkpoint=str(args.checkpoint.resolve()))
    encoder = model.image_encoder.eval()
    sample = torch.zeros((1, 3, 1024, 1024), dtype=torch.float32)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with torch.inference_mode():
        torch.onnx.export(
            encoder,
            sample,
            str(args.output),
            export_params=True,
            opset_version=args.opset,
            do_constant_folding=True,
            input_names=["images"],
            output_names=["image_embeddings"],
        )
    exported = onnx.load(str(args.output))
    onnx.checker.check_model(exported)
    print(json.dumps({
        "output": str(args.output.resolve()),
        "bytes": args.output.stat().st_size,
        "torch": torch.__version__,
        "onnx": onnx.__version__,
        "opset": args.opset,
    }))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
