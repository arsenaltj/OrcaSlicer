"""Read the build-time product identity; never changes upstream compatibility IDs."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import re


def load_product(root: Path) -> dict:
    data = json.loads((root / "config/product.json").read_text(encoding="utf-8-sig"))
    if not isinstance(data, dict):
        raise ValueError("product.json must be an object")
    name, version, build = (data.get(key) for key in ("display_name", "version", "build_number"))
    if not isinstance(name, str) or not re.fullmatch(r"[A-Za-z0-9]+(?: [A-Za-z0-9]+)*", name):
        raise ValueError("product.json display_name must contain words separated by single spaces")
    if not isinstance(version, str) or not re.fullmatch(r"(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)", version):
        raise ValueError("product.json version must be major.minor.patch")
    if type(build) is not int or not 0 <= build <= 65535 or any(int(part) > 65535 for part in version.split(".")):
        raise ValueError("product.json version components must be integers between 0 and 65535")
    return {**data, "display_version": "V" + version, "file_version": f"{version}.{build}",
            "exe": name + ".exe", "dll": name + ".dll", "pdb": name + ".pdb",
            "package_stem": name.replace(" ", "_"), "command": name.lower().replace(" ", "-")}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--field")
    args = parser.parse_args()
    try:
        product = load_product(args.root)
        print(product[args.field] if args.field else json.dumps(product))
    except (OSError, ValueError, KeyError) as exc:
        parser.exit(1, f"Invalid product configuration: {exc}\n")
