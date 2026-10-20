#!/usr/bin/env python3
"""Verify that the public MobileSDK stays independent from desktop UI and vendor code."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import sys

FORBIDDEN = (
    "wx/",
    "wxString",
    "wxWindow",
    "pybind",
    "boost/filesystem",
    "libslic3r_gui",
    "slic3r/GUI",
    "Bambu",
    "BBL",
    "vendor DLL",
)


def issue(code: str, message: str, path: str | None = None) -> dict[str, str]:
    value = {"code": code, "message": message}
    if path:
        value["path"] = path
    return value


def verify(root: Path) -> list[dict[str, str]]:
    errors: list[dict[str, str]] = []
    sdk = root / "src" / "orca_sdk"
    if not sdk.is_dir():
        return [issue("missing_sdk", "src/orca_sdk does not exist")]

    source_files = list(sdk.rglob("*.cpp")) + list(sdk.rglob("*.hpp")) + list(sdk.rglob("*.in"))
    for path in source_files:
        text = path.read_text(encoding="utf-8")
        for token in FORBIDDEN:
            if token in text:
                errors.append(issue("forbidden_dependency", f"MobileSDK contains forbidden dependency token: {token}", str(path.relative_to(root))))
        if path.suffix == ".hpp" and "#include \"orca/sdk/" not in text:
            for line in text.splitlines():
                if line.startswith("#include \""):
                    errors.append(issue("private_header_include", "Public SDK headers may only include other public SDK headers or system headers", str(path.relative_to(root))))
                    break

    sdk_cmake = (sdk / "CMakeLists.txt").read_text(encoding="utf-8")
    if "add_library(OrcaSlicer::MobileSDK ALIAS orca_mobile_sdk)" not in sdk_cmake:
        errors.append(issue("missing_alias", "MobileSDK CMake target alias is missing", "src/orca_sdk/CMakeLists.txt"))
    if "target_link_libraries(orca_mobile_sdk PUBLIC nlohmann_json)" not in sdk_cmake:
        errors.append(issue("unexpected_dependency", "MobileSDK must keep its public dependency limited to nlohmann_json", "src/orca_sdk/CMakeLists.txt"))
    if "MobileSDKBuildInfo.hpp.in" not in sdk_cmake:
        errors.append(issue("missing_build_identity", "MobileSDK must generate a build identity header", "src/orca_sdk/CMakeLists.txt"))

    source_cmake = (root / "src" / "CMakeLists.txt").read_text(encoding="utf-8")
    if "add_subdirectory(orca_sdk)" not in source_cmake:
        errors.append(issue("sdk_not_in_desktop_build", "Desktop CMake must configure MobileSDK", "src/CMakeLists.txt"))
    if not re.search(r"target_link_libraries\(OrcaSlicer[^\n]*OrcaSlicer::MobileSDK", source_cmake):
        errors.append(issue("desktop_not_consuming_sdk", "The desktop executable must consume the same MobileSDK contract target", "src/CMakeLists.txt"))

    gui_cmake = (root / "src" / "slic3r" / "CMakeLists.txt").read_text(encoding="utf-8")
    if not re.search(r"target_link_libraries\(libslic3r_gui[^\n]*OrcaSlicer::MobileSDK", gui_cmake):
        errors.append(issue("gui_not_consuming_sdk", "The desktop GUI target must consume the same MobileSDK contract target", "src/slic3r/CMakeLists.txt"))

    build_info = (sdk / "MobileSDKBuildInfo.hpp.in").read_text(encoding="utf-8")
    if "@ORCA_SOURCE_COMMIT@" not in build_info or "@ORCA_GIT_COMMIT_HASH_SHORT@" not in build_info:
        errors.append(issue("missing_source_identity", "Generated SDK identity must include the desktop source commit", "src/orca_sdk/MobileSDKBuildInfo.hpp.in"))

    test_cmake = (root / "tests" / "mobile_sdk" / "CMakeLists.txt")
    if not test_cmake.is_file() or "OrcaSlicer::MobileSDK" not in test_cmake.read_text(encoding="utf-8"):
        errors.append(issue("missing_open_source_tests", "The independent MobileSDK contract test target is missing", "tests/mobile_sdk/CMakeLists.txt"))
    tests_cmake = (root / "tests" / "CMakeLists.txt").read_text(encoding="utf-8")
    if "add_subdirectory(mobile_sdk)" not in tests_cmake:
        errors.append(issue("mobile_sdk_tests_not_registered", "The independent MobileSDK test suite is not registered", "tests/CMakeLists.txt"))
    for documentation in ("Docs/mobile-sdk/README.md", "Docs/mobile-sdk/API.md"):
        if not (root / documentation).is_file():
            errors.append(issue("missing_documentation", "MobileSDK usage and API documentation is required", documentation))

    return errors


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()
    errors = verify(args.source_root.resolve())
    result = {"ok": not errors, "errors": errors}
    if args.json:
        print(json.dumps(result, ensure_ascii=False, indent=2))
    elif errors:
        for value in errors:
            print(f"{value['code']}: {value['message']} ({value.get('path', '')})")
    else:
        print("MobileSDK boundary check passed")
    return 0 if not errors else 1


if __name__ == "__main__":
    sys.exit(main())
