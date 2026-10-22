#!/usr/bin/env python3
"""Verify that the shared libslic3r source manifest does not silently drift."""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

# These translation units belong to separate targets or platform-specific
# additions. Every other libslic3r implementation file must be listed in the
# shared core manifest so an upstream source addition cannot be silently omitted.
NON_CORE_SOURCES = {
    "CutSurface.cpp",
    "ExPolygonCollection.cpp",
    "Format/ModelIO.mm",
    "GCodeSender.cpp",
    "IntersectionPoints.cpp",
    "JumpPointSearch.cpp",
    "MacUtils.mm",
    "MeshBoolean.cpp",
    "MobileTriangulation.cpp",
    "OpenVDBUtils.cpp",
    "Triangulation.cpp",
    "TryCatchSignal.cpp",
    "TryCatchSignalSEH.cpp",
}


def manifest_sources(source_root: Path) -> set[str]:
    manifest = source_root / "src" / "libslic3r" / "core_sources.cmake"
    text = manifest.read_text(encoding="utf-8")
    entries = re.findall(r"\$\{ORCA_SLIC3R_CORE_SOURCE_DIR\}/([^\")]+)", text)
    if len(entries) != len(set(entries)):
        duplicates = sorted(entry for entry in set(entries) if entries.count(entry) > 1)
        raise ValueError(f"duplicate manifest entries: {', '.join(duplicates)}")
    paths = set(entries)
    missing = sorted(entry for entry in paths if not (source_root / "src" / "libslic3r" / entry).is_file())
    if missing:
        raise ValueError(f"manifest entries do not exist: {', '.join(missing)}")
    return paths


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    source_root = args.source_root.resolve()
    libslic3r = source_root / "src" / "libslic3r"
    manifest = manifest_sources(source_root)
    implementation_files = {
        str(path.relative_to(libslic3r)).replace("\\", "/")
        for path in libslic3r.rglob("*")
        if path.is_file() and path.suffix in {".cpp", ".mm"}
    }
    manifest_implementations = {path for path in manifest if path.endswith((".cpp", ".mm"))}
    unclassified = sorted(implementation_files - manifest_implementations - NON_CORE_SOURCES)
    stale_allowlist = sorted(NON_CORE_SOURCES - implementation_files)
    if unclassified:
        raise ValueError(
            "implementation files are neither in core_sources.cmake nor the "
            f"non-core allowlist: {', '.join(unclassified)}"
        )
    if stale_allowlist:
        raise ValueError(f"non-core allowlist contains missing files: {', '.join(stale_allowlist)}")

    mobile_cmake = (source_root / "mobile-slicing" / "CMakeLists.txt").read_text(encoding="utf-8")
    for extra in ("ExPolygonCollection.cpp", "JumpPointSearch.cpp", "MobileTriangulation.cpp"):
        if extra not in mobile_cmake:
            raise ValueError(f"mobile CMake does not declare mobile core source: {extra}")
    print(
        f"Core source manifest verified: {len(manifest)} entries, "
        f"{len(manifest_implementations)} implementation files, "
        f"{len(NON_CORE_SOURCES)} explicit non-core files"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except ValueError as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(1)
