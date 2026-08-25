from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import tempfile
from typing import Mapping, Sequence


DEFAULT_NAMES = (
    "OPENAI_API_KEY",
    "OPENAI_BASE_URL",
    "OPENAI_IMAGE_MODEL",
    "OPENAI_TEXT_MODEL",
    "TRIPO_API_BASE",
    "TRIPO_API_KEY",
    "TRIPO_MODEL",
)
DEFAULT_TRIPO_API_BASE = "https://openapi.tripo3d.com/v3"
INTERNAL_CONFIG_MODE = "internal_locked"
REQUIRED_NAMES = ("OPENAI_API_KEY", "OPENAI_BASE_URL", "TRIPO_API_KEY")


def build_payload(environ: Mapping[str, str]) -> dict[str, object]:
    payload: dict[str, object] = {"version": 1, "mode": INTERNAL_CONFIG_MODE}
    for name in DEFAULT_NAMES:
        value = environ.get(name, "")
        if value:
            if len(value) > 8192 or any(char in value for char in "\0\r\n"):
                raise ValueError(f"{name} contains an unsupported value")
            payload[name] = value
    missing_names = [name for name in REQUIRED_NAMES if name not in payload]
    if missing_names:
        raise ValueError(f"Missing required internal setting(s): {', '.join(missing_names)}")
    payload.setdefault("TRIPO_API_BASE", DEFAULT_TRIPO_API_BASE)
    return payload


def write_payload(output_path: Path, payload: Mapping[str, object]) -> None:
    output_path = output_path.resolve()
    output_path.parent.mkdir(parents=True, exist_ok=True)
    encoded = (json.dumps(payload, ensure_ascii=False, indent=2) + "\n").encode("utf-8")
    if len(encoded) > 32 * 1024:
        raise ValueError("Internal defaults payload exceeds 32 KiB")

    temporary_path: Path | None = None
    try:
        with tempfile.NamedTemporaryFile(dir=output_path.parent, prefix=f".{output_path.name}.", delete=False) as stream:
            temporary_path = Path(stream.name)
            stream.write(encoded)
        temporary_path.replace(output_path)
    finally:
        if temporary_path is not None:
            temporary_path.unlink(missing_ok=True)


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Create a package-only OrcaSlicer AI defaults payload.")
    parser.add_argument("output", type=Path)
    arguments = parser.parse_args(argv)
    try:
        payload = build_payload(os.environ)
        write_payload(arguments.output, payload)
    except (OSError, ValueError) as exc:
        parser.error(str(exc))
    print(f"Created internal defaults with {len(payload) - 2} configured setting(s).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
