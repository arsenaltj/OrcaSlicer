#!/usr/bin/env python3
"""Verify the packaged AI Python runtime without consulting user site packages."""

from __future__ import annotations

import argparse
from io import BytesIO
import json
from pathlib import Path
import platform
import sys
from typing import Sequence


def path_is_within(path: Path, root: Path) -> bool:
    """Return whether *path* resolves below *root*, including on Windows."""
    try:
        path.resolve(strict=True).relative_to(root.resolve(strict=True))
        return True
    except (OSError, ValueError):
        return False


def verify_runtime(
    python_root: Path,
    expected_python: str,
    expected_pillow: str,
    expect_yoyoclaw: bool = False,
) -> dict[str, object]:
    import PIL
    from PIL import Image

    if platform.python_version() != expected_python:
        raise RuntimeError("bundled Python version mismatch")
    if PIL.__version__ != expected_pillow:
        raise RuntimeError("bundled Pillow version mismatch")

    pillow_module = Path(PIL.__file__)
    if not path_is_within(pillow_module, python_root):
        raise RuntimeError("Pillow was imported from outside the bundled runtime")

    source = Image.new("RGBA", (2, 2), (1, 2, 3, 4))
    encoded = BytesIO()
    source.save(encoded, format="PNG")
    encoded.seek(0)
    with Image.open(encoded) as decoded:
        decoded.load()
        if decoded.mode != "RGBA" or decoded.size != (2, 2) or decoded.getpixel((0, 0)) != (1, 2, 3, 4):
            raise RuntimeError("Pillow native PNG round-trip failed")

    yoyoclaw_modules: dict[str, str] = {}
    if expect_yoyoclaw:
        import cryptography
        from cryptography.hazmat.bindings import _rust
        from cryptography.hazmat.primitives.ciphers.aead import AESGCM
        import requests

        for name, module in (("requests", requests), ("cryptography", cryptography), ("cryptography_native", _rust)):
            module_path = Path(module.__file__).resolve(strict=True)
            if not path_is_within(module_path, python_root):
                raise RuntimeError(f"{name} was imported from outside the bundled runtime")
            yoyoclaw_modules[name] = module_path.relative_to(python_root.resolve(strict=True)).as_posix()

        cipher = AESGCM(bytes(range(32)))
        nonce, plaintext, aad = bytes(range(12)), b"orca-yoyoclaw-runtime-check", b"runtime-check"
        if cipher.decrypt(nonce, cipher.encrypt(nonce, plaintext, aad), aad) != plaintext:
            raise RuntimeError("YOYOClaw native AES-GCM round-trip failed")

    relative_module = pillow_module.resolve(strict=True).relative_to(python_root.resolve(strict=True)).as_posix()
    return {
        "schema_version": 1,
        "python_version": platform.python_version(),
        "pillow_version": PIL.__version__,
        "pillow_module": relative_module,
        "native_png_roundtrip": True,
        "yoyoclaw_modules": yoyoclaw_modules,
        "yoyoclaw_native_roundtrip": expect_yoyoclaw,
    }


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--python-root", type=Path, required=True)
    parser.add_argument("--expect-python", required=True)
    parser.add_argument("--expect-pillow", required=True)
    parser.add_argument("--expect-yoyoclaw", action="store_true")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args(argv)

    try:
        report = verify_runtime(args.python_root, args.expect_python, args.expect_pillow, args.expect_yoyoclaw)
    except Exception as exc:
        print(f"Bundled AI runtime verification failed ({type(exc).__name__}).", file=sys.stderr)
        return 1

    if args.json:
        print(json.dumps(report, sort_keys=True))
    else:
        print(
            f"Bundled AI runtime: PASS (Python {report['python_version']}, "
            f"Pillow {report['pillow_version']}, isolated native PNG round-trip, "
            f"YOYOClaw dependencies={'present' if report['yoyoclaw_modules'] else 'not requested'})"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
