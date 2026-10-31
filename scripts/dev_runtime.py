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
from product_config import load_product


NATIVE_PORTRAIT_ARTIFACTS = {
    "libmediapipe.dll": (43301376, "a8970c645c8c87c25ec9965cb5c898e803c6c42f7192b7de9a0541c62ae48cef"),
    "selfie_multiclass_256x256.tflite": (16371837, "c6748b1253a99067ef71f7e26ca71096cd449baefa8f101900ea23016507e0e0"),
    "face_landmarker.task": (3758596, "64184e229b263107bc2b804c6625db1341ff2bb731874b0bcc2fe6544e0bc9ff"),
    "LICENSE": (12331, "8707eef0533987efc5b155d64761eeb6e20793f50b9bd1a68dad1cf4719d0ed8"),
    "NOTICE": (1540836, "d3b4a80a24a01fd445d4b70a610fd836ec3547c3a62eb835a1041956c38d9f56"),
    "runtime-manifest.json": (2276, "e3a3cf149163a263f5ac0e10a045a6a474096263170b65eba2d01f547728f588"),
    "MODEL_LICENSES.md": (2053, "da2614c3402f4f88478800032e61bb5a8004a5ce75073412f2043902d7e2bb28"),
    "providers.json": (85, "07c9b971c5bda8db4007d2396cdb9fb221c6328220f89d77159468b002dc9298"),
}


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


def verify_lightweight_portrait_runtime(root: Path, runtime: Path) -> None:
    """Validate the optional native portrait runtime staged beside the EXE.

    The production Python/weights bundle remains disabled for partition builds,
    but the small MediaPipe C ABI runtime is an independent, explicitly pinned
    capability.  Keep it outside the normal install manifest while still
    checking the same hashes as ``prepare_semantic_runtime.ps1``.
    """
    native = runtime / "ai/portrait_semantics"
    manifest_path = native / "runtime-manifest.json"
    if not manifest_path.is_file():
        raise ValueError("lightweight portrait runtime is missing runtime-manifest.json")
    try:
        staged = read_json(manifest_path)
        expected = read_json(root / "cmake/semantic_runtime_manifest.json")
    except (OSError, json.JSONDecodeError) as exc:
        raise ValueError("lightweight portrait runtime has an invalid manifest") from exc
    for key in ("schema", "platform", "provider", "execution", "source_commit"):
        if staged.get(key) != expected.get(key):
            raise ValueError(f"lightweight portrait runtime manifest mismatch: {key}")
    files = {path.relative_to(native).as_posix() for path in native.rglob("*") if path.is_file()}
    if files != set(NATIVE_PORTRAIT_ARTIFACTS):
        raise ValueError("lightweight portrait runtime must contain exactly the eight sealed native files")
    try:
        providers = read_json(native / "providers.json")
    except (OSError, json.JSONDecodeError) as exc:
        raise ValueError("lightweight portrait runtime has invalid providers.json") from exc
    if providers != {"body_provider": "mediapipe.cpu.v1", "face_provider": "mediapipe.cpu.v1"}:
        raise ValueError("lightweight portrait runtime has an invalid provider configuration")
    for name, (size, sha256) in NATIVE_PORTRAIT_ARTIFACTS.items():
        path = native / name
        if not path.is_file():
            raise ValueError(f"lightweight portrait runtime is missing {name}")
        if path.stat().st_size != size or digest(path) != sha256:
            raise ValueError(f"lightweight portrait runtime checksum mismatch: {name}")


def identity(root: Path) -> dict:
    """Hash changed bytes without requiring a handoff, commit or remote access."""
    head = git(root, "rev-parse", "HEAD").decode().strip()
    changed = git_paths(root, "-c", "core.safecrlf=false", "diff", "--no-ext-diff", "--no-renames", "--name-only", "-z", "HEAD")
    untracked = git_paths(root, "ls-files", "--others", "--exclude-standard", "-z")
    # Untracked research/images are not program inputs. Packaged snapshots still
    # use package_source_identity.py's stricter, complete manifest contract.
    program = {p for p in untracked if p.startswith(("src/", "tools/", "scripts/", "cmake/", "config/", "deps/", "deps_src/", "resources/", "localization/", "tests/"))
               or p in {"CMakeLists.txt", "dev.ps1"}}
    # Generated catalogs are ignored by Git, but are installed application inputs.
    catalogs = {p.relative_to(root).as_posix() for p in (root / "resources/i18n").rglob("*.mo")}
    files = [{"path": name, "sha256": digest(path) if path.is_file() else None}
             for name in sorted(changed | program | catalogs) for path in [source_path(root, name)]]
    native = [f for f in files if f["path"].startswith(("src/", "cmake/", "deps/", "deps_src/", "resources/", "localization/"))
              or f["path"] in {"CMakeLists.txt", "version.inc", "config/product.json"} or f["path"].endswith(".cmake")]
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


def modules(root: Path, build: Path | None = None) -> list[str]:
    cmake = (root / "CMakeLists.txt").read_text(encoding="utf-8")
    block = cmake.split("set(ORCA_AI_SIDECAR_RUNTIME_FILES", 1)[1].split("install(FILES", 1)[0]
    components = {
        "ORCA_LOCAL_SEMANTIC_RUNTIME_FILES": "tools/ai/local_semantic_runtime_files.cmake",
        "ORCA_MODEL_CAPABILITY_RUNTIME_FILES": "tools/ai/model_runtime_files.cmake",
    }
    for variable, relative_path in components.items():
        marker = "${" + variable + "}"
        if marker in block:
            component = root / relative_path
            content = component.read_text(encoding="utf-8")
            cache = (build / "CMakeCache.txt").read_text(encoding="utf-8") if build else ""
            for option in ("ORCA_AI_PORTRAIT_RECOGNITION", "ORCA_AI_LEGACY_FARL_RUNTIME"):
                conditional = re.compile(
                    rf"^if\({option}\)\s*\n(.*?)^endif\(\)[ \t]*$",
                    re.MULTILINE | re.DOTALL)
                if conditional.search(content):
                    enabled = re.search(rf"^{option}:BOOL=(ON|OFF)$", cache, re.MULTILINE)
                    if enabled is None:
                        raise ValueError(f"Cannot resolve configured runtime option: {option}")
                    content = conditional.sub(lambda match: match[1] if enabled[1] == "ON" else "", content)
            if re.search(r"^[ \t]*(?:if|elseif|else|endif)\s*\(", content, re.MULTILINE):
                raise ValueError("Unsupported sidecar runtime condition; update modules() before refreshing")
            block = block.replace(marker, content)
    # Never silently omit a newly introduced component variable.
    remainder = block.replace("${CMAKE_SOURCE_DIR}", "")
    if re.search(r"\$\{[^}]+\}", remainder):
        raise ValueError("Unsupported sidecar runtime variable; update modules() before refreshing")
    names = re.findall(r'\$\{CMAKE_SOURCE_DIR\}/tools/ai/([A-Za-z0-9_]+\.(?:py|json))', block)
    if not names:
        raise ValueError("Invalid CMake sidecar runtime list")
    # CMake permits the same source in a shared component and its caller.
    # All matches use the same canonical tools/ai directory; check each once.
    return list(dict.fromkeys(names))


def prepared_portrait_source(build: Path) -> Path:
    """Resolve the CPU bundle from the generated installation rules."""
    script = (build / "src/cmake_install.cmake").read_text(encoding="utf-8")
    sources = re.findall(
        r'file\(INSTALL DESTINATION "\$\{CMAKE_INSTALL_PREFIX\}/(?:\./)?resources/beauty-runtime"'
        r'\s+TYPE DIRECTORY FILES "([^"]+)"\)', script)
    if len(sources) != 1 or not Path(sources[0]).is_absolute() or not Path(sources[0]).is_dir():
        raise ValueError("Cannot identify configured portrait runtime source")
    return Path(sources[0]).resolve()


def verify_sealed_portrait_manifest(source: Path, runtime: Path, raster: Path) -> None:
    """Allow only the install-time seal of the exact Release accelerator."""
    expected = read_json(source / "runtime-manifest.json")
    if expected.get("schema") != "orca.offline-portrait-runtime/v1" or not all(
            isinstance(expected.get(key), dict) for key in ("files", "modules")):
        raise ValueError("Invalid prepared portrait runtime manifest")
    record = {"size": raster.stat().st_size, "sha256": digest(raster)}
    expected["files"]["modules/local_semantic_raster.dll"] = record
    expected["modules"]["local_semantic_raster.dll"] = record
    expected["raster_required"] = True
    if read_json(runtime / "runtime-manifest.json") != expected:
        raise ValueError("Installed portrait runtime manifest does not match source/build")


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


def verify_runtime(root: Path, build: Path, installed: list[str], runtime: Path | None = None) -> dict:
    # A separate trial can retain an open older runtime while using the same
    # source/cache checks. Never accept an external or linked install directory.
    runtime = runtime if runtime is not None else root / ".tmp/dev/run"
    if not runtime.resolve().is_relative_to((root / ".tmp").resolve()):
        raise ValueError("Trial runtime must remain inside this checkout's .tmp directory")
    for path in (runtime, *runtime.parents):
        if path == root:
            break
        if path.is_symlink() or path.is_junction():
            raise ValueError("Trial runtime must remain unlinked inside this checkout")
    for name in installed:
        if not source_path(runtime, name).is_file():
            raise ValueError(f"Runtime is incomplete: {name}; run dev.ps1 again")
    checks = {name: digest(source_path(runtime, name)) for name in installed}
    product = load_product(root)
    pairs = {name: build / "src/Release" / name for name in (product["exe"], product["dll"])}
    pairs.update({"resources/tools/ai/" + name: root / "tools/ai" / name for name in modules(root, build)})
    pairs["resources/tools/ai/orca_ai_build_info.json"] = build / "orca_ai_build_info.json"
    pairs["resources/tools/ai/orca_ai_runtime_dependencies.json"] = build / "orca_ai_runtime_dependencies.json"
    cache = (build / "CMakeCache.txt").read_text(encoding="utf-8")
    portrait_enabled = bool(re.search(r"^ORCA_AI_PORTRAIT_RECOGNITION:BOOL=ON$", cache, re.MULTILINE))
    legacy_farl_enabled = bool(re.search(r"^ORCA_AI_LEGACY_FARL_RUNTIME:BOOL=ON$", cache, re.MULTILINE))
    metadata = read_json(build / "orca_ai_runtime_dependencies.json")
    portrait_metadata = metadata.get("portrait_recognition", {})
    if portrait_metadata.get("enabled") is not portrait_enabled:
        raise ValueError("Installed runtime has inconsistent portrait capabilities")
    if portrait_metadata.get("legacy_farl_enabled", False) is not legacy_farl_enabled:
        raise ValueError("Installed runtime has inconsistent legacy FaRL capability")
    native = runtime / "ai/portrait_semantics"
    if portrait_enabled:
        if portrait_metadata.get("provider") != "mediapipe.cpu.v1" or \
                portrait_metadata.get("runtime_manifest") != "ai/portrait_semantics/runtime-manifest.json":
            raise ValueError("Installed runtime has invalid native portrait metadata")
        verify_lightweight_portrait_runtime(root, runtime)
    elif native.exists():
        raise ValueError("Portrait-disabled runtime contains native portrait resources")
    if not legacy_farl_enabled:
        leftovers = ("resources/beauty-runtime",
                     "resources/tools/ai/local_semantic_raster.dll",
                     *("resources/tools/ai/portrait_parent_cleanup_" + name + ".json"
                       for name in ("catalog", "fangfei", "partition", "locks")))
        if any((runtime / name).exists() for name in leftovers):
            raise ValueError("Native-only runtime contains retired portrait resources")
    raster = re.search(r"^ORCA_BEAUTY_RASTER_BINARY:FILEPATH=(.+)$", cache, re.MULTILINE)
    if legacy_farl_enabled and raster:
        source = Path(raster[1].strip())
        expected = re.search(r"^ORCA_BEAUTY_RASTER_SHA256:STRING=([0-9a-f]{64})$", cache, re.MULTILINE)
        if expected is None or not source.is_file() or digest(source) != expected[1]:
            raise ValueError("Preserved Beauty raster binary identity changed")
        pairs["resources/tools/ai/local_semantic_raster.dll"] = source
    elif legacy_farl_enabled:
        source = build / "Release/local_semantic_raster.dll"
    if legacy_farl_enabled:
        for name in ("resources/tools/ai/local_semantic_raster.dll",
                     "resources/beauty-runtime/modules/local_semantic_raster.dll"):
            if name in installed:
                pairs[name] = source
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
    beauty = re.search(r"^ORCA_BEAUTY_RUNTIME_ROOT:PATH=(.+)$", cache, re.MULTILINE) if legacy_farl_enabled else None
    beauty_root = Path(beauty[1].strip()) if beauty else None
    sealed_manifest = "resources/beauty-runtime/runtime-manifest.json"
    if beauty_root is None and any(name.startswith("resources/beauty-runtime/") for name in installed):
        beauty_root = prepared_portrait_source(build)
        raster_name = "resources/beauty-runtime/modules/local_semantic_raster.dll"
        if sealed_manifest not in installed or raster_name not in pairs:
            raise ValueError("Prepared portrait runtime is missing its manifest or accelerator")
        verify_sealed_portrait_manifest(beauty_root, runtime / "resources/beauty-runtime", pairs[raster_name])
    else:
        sealed_manifest = None
    for name in installed:
        if name.startswith("resources/") and not name.startswith("resources/tools/ai/") and name not in pairs:
            if name == sealed_manifest:
                continue  # Its complete parsed contents were checked above.
            source = source_path(root, name)
            if name.startswith("resources/beauty-runtime/"):
                if beauty_root is None:
                    raise ValueError("Cannot identify configured beauty runtime source")
                source = source_path(beauty_root, name.removeprefix("resources/beauty-runtime/"))
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
    refreshable = {"resources/tools/ai/" + name for name in modules(root, build)}
    for name in state["installed_files"]:
        target = source_path(runtime, name)
        if not target.is_file():
            raise ValueError(f"Runtime is incomplete: {name}; use dev.ps1 Run")
        if name not in refreshable and digest(target) != state["hashes"].get(name):
            raise ValueError(f"Installed runtime file changed: {name}; use dev.ps1 Run")
    product = load_product(root)
    for name in (product["exe"], product["dll"]):
        if digest(runtime / name) != state["hashes"][name] or digest(build / "src/Release" / name) != state["hashes"][name]:
            raise ValueError("Native binaries changed; use dev.ps1 Run")
    copied = []
    for name in modules(root, build):
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
