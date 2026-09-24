"""Local development identity and runtime checks; never submits provider jobs."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from package_source_identity import git, git_paths, source_path


def digest(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def json_digest(value: object) -> str:
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(",", ":")).encode()).hexdigest()


def read_json(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8-sig"))


def write_json(path: Path, value: object) -> None:
    temporary = path.with_suffix(".new")
    temporary.write_text(json.dumps(value, ensure_ascii=True, indent=2) + "\n", encoding="utf-8")
    temporary.replace(path)


def identity(root: Path) -> dict:
    """Hash changed bytes without requiring a handoff, commit or remote access."""
    head = git(root, "rev-parse", "HEAD").decode().strip()
    changed = git_paths(root, "-c", "core.safecrlf=false", "diff", "--no-ext-diff", "--no-renames", "--name-only", "-z", "HEAD")
    untracked = git_paths(root, "ls-files", "--others", "--exclude-standard", "-z")
    # Untracked research/images are not program inputs. Packaged snapshots still
    # use package_source_identity.py's stricter, complete manifest contract.
    program = {p for p in untracked if p.startswith(("src/", "tools/", "scripts/", "cmake/", "deps/", "deps_src/", "resources/", "localization/", "tests/"))
               or p in {"CMakeLists.txt", "dev.ps1"}}
    # Generated catalogs are ignored by Git, but are installed application inputs.
    catalogs = {p.relative_to(root).as_posix() for p in (root / "resources/i18n").rglob("*.mo")}
    files = [{"path": name, "sha256": digest(path) if path.is_file() else None}
             for name in sorted(changed | program | catalogs) for path in [source_path(root, name)]]
    native = [f for f in files if f["path"].startswith(("src/", "cmake/", "deps/", "deps_src/", "resources/", "localization/"))
              or f["path"] == "CMakeLists.txt" or f["path"].endswith(".cmake")]
    return {"source_commit": head, "source_clean": not bool(changed or untracked), "files": files,
            "source_identity": json_digest([head, files]), "native_identity": json_digest([head, native])}


def preflight(root: Path, build: Path) -> dict:
    cache = dict(re.findall(r"^([^#/:][^:\n]*):[^=\n]+=(.*)$",
                            (build / "CMakeCache.txt").read_text(encoding="utf-8"), re.MULTILINE))
    configured = Path(cache["CMAKE_HOME_DIRECTORY"])
    if not configured.samefile(root):
        raise ValueError(f"Build cache belongs to another checkout: {configured}")
    for key in ("CMAKE_COMMAND", "Python3_EXECUTABLE"):
        if not Path(cache[key]).is_file():
            raise ValueError(f"Missing configured tool: {key}={cache[key]}")
    if cache.get("ORCA_AI_INTERNAL_DEFAULTS_FILE"):
        raise ValueError("Use provider environment variables; the runtime must not embed private defaults")
    local = root / ".tmp/dev"
    for path in (local, local / "run", local / "data", local / "logs"):
        if not path.resolve().is_relative_to(root) or path.is_symlink() or path.is_junction():
            raise ValueError("Managed development directories must remain unlinked inside this checkout")
    return {"source": str(configured), "build": str(build), "cmake": cache["CMAKE_COMMAND"],
            "python": cache["Python3_EXECUTABLE"], "generator": cache["CMAKE_GENERATOR"],
            "needs_runtime_configuration": cache.get("ORCA_AI_WINDOWS_INSTALLER") != "ON",
            "native_configuration": json_digest({k: v for k, v in cache.items()
                if k.startswith(("SLIC3R_", "ORCA_AI_", "ORCA_BEAUTY_", "CMAKE_CXX_", "CMAKE_C_", "CMAKE_GENERATOR", "CMAKE_PREFIX_PATH", "Python3_"))}),
            "runtime": str(local / "run"), "data": str(local / "data"), **identity(root)}


def setup_identity(root: Path, build: Path) -> dict:
    cache_path = build / "CMakeCache.txt"
    if cache_path.is_file():
        match = re.search(r"^CMAKE_HOME_DIRECTORY:[^=]+=(.+)$", cache_path.read_text(encoding="utf-8"), re.MULTILINE)
        if not match or not Path(match[1].strip()).samefile(root):
            raise ValueError("Build cache belongs to another checkout or has no source identity")
    return identity(root)


def modules(root: Path) -> list[str]:
    cmake = (root / "CMakeLists.txt").read_text(encoding="utf-8")
    block = cmake.split("set(ORCA_AI_SIDECAR_RUNTIME_FILES", 1)[1].split("install(FILES", 1)[0]
    if "${ORCA_LOCAL_SEMANTIC_RUNTIME_FILES}" in block:
        component = root / "tools/ai/local_semantic_runtime_files.cmake"
        block = block.replace("${ORCA_LOCAL_SEMANTIC_RUNTIME_FILES}", component.read_text(encoding="utf-8"))
    # Never silently omit a newly introduced component variable.
    remainder = block.replace("${CMAKE_SOURCE_DIR}", "")
    if re.search(r"\$\{[^}]+\}", remainder):
        raise ValueError("Unsupported sidecar runtime variable; update modules() before refreshing")
    names = re.findall(r'\$\{CMAKE_SOURCE_DIR\}/tools/ai/([A-Za-z0-9_]+\.py)', block)
    if not names or len(names) != len(set(names)):
        raise ValueError("Invalid CMake sidecar runtime list")
    return names


def refresh_catalogs(root: Path) -> dict:
    """Compile only stale PO catalogs; never regenerate or modify translations."""
    compiler = root / "tools/msgfmt.exe"
    updated = []
    for source in sorted((root / "localization/i18n").rglob("OrcaSlicer_*.po")):
        locale = source.stem.removeprefix("OrcaSlicer_")
        target = source_path(root, f"resources/i18n/{locale}/OrcaSlicer.mo")
        # Hash PO inputs, not mtimes: restored files can have older timestamps.
        stamp = root / ".tmp/dev" / f"catalog-{locale}.json"
        expected = {"source": digest(source), "compiler": digest(compiler)}
        if target.is_file() and stamp.is_file():
            previous = read_json(stamp)
            if previous == {**expected, "output": digest(target)}:
                continue
        target.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run([str(compiler), "--check-format", "-o", str(target), str(source)], check=True)
        stamp.parent.mkdir(parents=True, exist_ok=True)
        write_json(stamp, {**expected, "output": digest(target)})
        updated.append(locale)
    return {"updated_catalogs": updated}


def verify_runtime(root: Path, build: Path, installed: list[str]) -> dict:
    runtime = root / ".tmp/dev/run"
    for name in installed:
        if not source_path(runtime, name).is_file():
            raise ValueError(f"Runtime is incomplete: {name}; run dev.ps1 again")
    checks = {name: digest(source_path(runtime, name)) for name in installed}
    pairs = {name: build / "src/Release" / name for name in ("orca-slicer.exe", "OrcaSlicer.dll")}
    pairs.update({"resources/tools/ai/" + name: root / "tools/ai" / name for name in modules(root)})
    pairs["resources/tools/ai/orca_ai_build_info.json"] = build / "orca_ai_build_info.json"
    pairs["resources/tools/ai/orca_ai_runtime_dependencies.json"] = build / "orca_ai_runtime_dependencies.json"
    # uv is installed from CMake's configured/downloaded tool, not resources/.
    uv_name = "resources/tools/uv/uv.exe"
    if uv_name in installed:
        install_script = (build / "cmake_install.cmake").read_text(encoding="utf-8")
        uv_sources = re.findall(
            r'file\(INSTALL DESTINATION "\$\{CMAKE_INSTALL_PREFIX\}/(?:\./)?resources/tools/uv"'
            r' TYPE PROGRAM RENAME "uv\.exe" FILES "([^"]+)"\)', install_script)
        if len(uv_sources) != 1:
            raise ValueError("Cannot identify configured uv installation source")
        pairs[uv_name] = Path(uv_sources[0])
    for name, source in pairs.items():
        target = runtime / name
        if not source.is_file() or not target.is_file() or digest(source) != digest(target):
            raise ValueError(f"Runtime does not match the build/source: {name}")
        checks[name] = digest(target)
    # The install manifest proves membership; compare resource bytes as well.
    cache = (build / "CMakeCache.txt").read_text(encoding="utf-8")
    beauty = re.search(r"^ORCA_BEAUTY_RUNTIME_ROOT:PATH=(.+)$", cache, re.MULTILINE)
    for name in installed:
        if name.startswith("resources/") and not name.startswith("resources/tools/ai/") and name not in pairs:
            source = source_path(root, name)
            if name.startswith("resources/beauty-runtime/"):
                if beauty is None:
                    raise ValueError("Cannot identify configured beauty runtime source")
                source = source_path(Path(beauty[1].strip()), name.removeprefix("resources/beauty-runtime/"))
            if not source.is_file() or digest(source) != checks[name]:
                raise ValueError(f"Runtime resource does not match source: {name}")
    for name in ("python/python.exe", "python/pythonw.exe", "resources/i18n/zh_CN/OrcaSlicer.mo"):
        if not (runtime / name).is_file():
            raise ValueError(f"Required runtime file missing: {name}")
    for name in ("resources/tools/ai/orca_ai_internal_defaults.json", "resources/generated_models"):
        if (runtime / name).exists():
            raise ValueError(f"Private configuration/assets do not belong in the runtime: {name}")
    return checks


def finish_install(root: Path, build: Path, before: dict) -> dict:
    after = preflight(root, build)
    if any(after[key] != before[key] for key in ("source_identity", "native_configuration")):
        raise ValueError("Source changed while preparing the runtime; rerun after edits finish")
    runtime = root / ".tmp/dev/run"
    installed = []
    for line in (build / "install_manifest.txt").read_text(encoding="utf-8").splitlines():
        installed.append(Path(line).resolve().relative_to(runtime).as_posix())
    if not installed:
        raise ValueError("CMake did not record any installed runtime files")
    state = {"schema_version": 2, "build": str(build), "source": after,
             "installed_files": installed, "hashes": verify_runtime(root, build, installed)}
    write_json(root / ".tmp/dev/runtime-state.json", state)
    return {"status": "PREPARED", "source_identity": after["source_identity"],
            "installed_files": len(installed), "runtime": str(runtime)}


def update_sidecar(root: Path, build: Path) -> dict:
    state = read_json(root / ".tmp/dev/runtime-state.json")
    if state.get("schema_version") != 2:
        raise ValueError("Runtime identity needs migration; use dev.ps1 Run")
    current = preflight(root, build)
    if not Path(state["build"]).samefile(build) or any(
            current[key] != state["source"][key] for key in ("native_identity", "native_configuration")):
        raise ValueError("Native sources/resources/build configuration changed; use dev.ps1 Run")
    runtime = root / ".tmp/dev/run"
    refreshable = {"resources/tools/ai/" + name for name in modules(root)}
    for name in state["installed_files"]:
        target = source_path(runtime, name)
        if not target.is_file():
            raise ValueError(f"Runtime is incomplete: {name}; use dev.ps1 Run")
        if name not in refreshable and digest(target) != state["hashes"].get(name):
            raise ValueError(f"Installed runtime file changed: {name}; use dev.ps1 Run")
    for name in ("orca-slicer.exe", "OrcaSlicer.dll"):
        if digest(runtime / name) != state["hashes"][name] or digest(build / "src/Release" / name) != state["hashes"][name]:
            raise ValueError("Native binaries changed; use dev.ps1 Run")
    copied = []
    for name in modules(root):
        source = source_path(root, "tools/ai/" + name)
        target = source_path(runtime, "resources/tools/ai/" + name)
        if not target.is_file() or digest(source) != digest(target):
            shutil.copy2(source, target)
            copied.append(name)
    if identity(root)["source_identity"] != current["source_identity"]:
        raise ValueError("Source changed during sidecar update; rerun after edits finish")
    state.update(source=current, hashes=verify_runtime(root, build, state["installed_files"]))
    write_json(root / ".tmp/dev/runtime-state.json", state)
    return {"status": "PREPARED", "source_identity": current["source_identity"], "updated_modules": copied,
            "runtime": str(runtime)}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("identity", "setup-check", "catalogs", "check", "finish", "sidecar"))
    parser.add_argument("--root", required=True, type=Path)
    parser.add_argument("--build", required=True, type=Path)
    parser.add_argument("--before", type=Path)
    args = parser.parse_args()
    root, build = args.root.resolve(), args.build.resolve()
    try:
        if args.action == "identity":
            result = identity(root)
        elif args.action == "setup-check":
            result = setup_identity(root, build)
        elif args.action == "catalogs":
            result = refresh_catalogs(root)
        elif args.action == "finish":
            result = finish_install(root, build, read_json(args.before))
        elif args.action == "sidecar":
            result = update_sidecar(root, build)
        else:
            result = preflight(root, build)
        print(json.dumps(result, ensure_ascii=True))
    except (OSError, ValueError, KeyError, IndexError, subprocess.CalledProcessError) as exc:
        parser.exit(1, f"Development runtime check failed: {exc}\n")


if __name__ == "__main__":
    main()
