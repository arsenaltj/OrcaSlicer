"""Distribute verified Windows CPU build inputs without checking binaries into Git.

Only standard-library Python is needed to download/unpack. Application algorithms
are deliberately excluded: CMake stages those from the current checkout.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import shutil
import stat
import subprocess
import sys
import tempfile
import urllib.parse
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]
SCHEMA = "orca.portrait-build-dependencies/v1"
LOCK = ROOT / "cmake/portrait_dependencies.json"
MANIFEST = "dependency-manifest.json"
INIT = "portrait-dependencies.cmake"


def digest(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def read_json(path):
    return json.loads(Path(path).read_text(encoding="utf-8"))


def write_json(path, value):
    Path(path).write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def relative_path(name):
    path = PurePosixPath(name)
    if (not name or path.is_absolute() or "\\" in name or ":" in name
            or str(path) != name or any(part in (".", "..") for part in path.parts)
            or any(part.endswith((" ", ".")) for part in path.parts)):
        raise ValueError(f"Unsafe dependency path: {name}")
    return path


def record(path):
    if path.is_symlink() or not path.is_file():
        raise ValueError(f"Not a regular dependency file: {path}")
    return {"bytes": path.stat().st_size, "sha256": digest(path)}


def check_file(path, expected):
    if record(path) != expected:
        raise ValueError(f"Dependency size/hash mismatch: {path.name}")


def load_lock(path):
    lock = read_json(path)
    if lock["schema"] != SCHEMA or lock["platform"] != "windows-x64-cp312-cpu":
        raise ValueError("Unsupported dependency descriptor")
    relative_path(lock["archive"]["name"])
    if "/" in lock["archive"]["name"]:
        raise ValueError("Archive name must be a filename")
    return lock


def verify(root, lock, imports=False):
    root = Path(root).resolve(strict=True)
    check_file(root / MANIFEST, lock["manifest"])
    manifest = read_json(root / MANIFEST)
    if manifest["schema"] != SCHEMA or manifest["version"] != lock["version"]:
        raise ValueError("Dependency manifest version mismatch")
    expected = {MANIFEST, INIT}
    for name, spec in manifest["files"].items():
        path = root.joinpath(*relative_path(name).parts)
        if not path.resolve().is_relative_to(root):
            raise ValueError(f"Dependency escapes destination: {name}")
        check_file(path, spec)
        expected.add(name)
    # Python is invoked with -B; no ambient files may enter later staging.
    for path in root.rglob("*"):
        if path.is_symlink() or (path.is_file() and path.relative_to(root).as_posix() not in expected):
            raise ValueError(f"Unexpected dependency file: {path.relative_to(root)}")
    if imports:
        if sys.platform != "win32":
            raise ValueError("Windows x64 is required for the CPU import check")
        subprocess.run([str(root / "python/python.exe"), "-I", "-B", "-c", "\n".join([
            "import importlib, pathlib, sys, struct",
            "root = pathlib.Path(sys.executable).resolve().parent",
            "assert sys.version_info[:2] == (3, 12) and struct.calcsize('P') == 8",
            "for name in ('numpy','cv2','scipy','PIL','torch','torchvision','facer','mediapipe'):",
            "    module = importlib.import_module(name)",
            "    assert pathlib.Path(module.__file__).resolve().is_relative_to(root), name",
            "import torch, numpy, cv2",
            "assert torch.version.cuda is None, 'CPU-only PyTorch required'",
            "assert cv2.countNonZero(numpy.ones((2,2), dtype=numpy.uint8)) == 4",
            "print('Verified isolated Python 3.12 x64 / CPU portrait imports')",
        ])], check=True)
    return manifest


def download(lock, cache):
    cache.mkdir(parents=True, exist_ok=True)
    archive = cache / lock["archive"]["name"]
    spec = {key: lock["archive"][key] for key in ("bytes", "sha256")}
    if archive.exists():
        check_file(archive, spec)
        return archive
    url = lock["archive"]["url"]
    parsed = urllib.parse.urlsplit(url)
    if parsed.scheme != "https" or parsed.username or parsed.password or parsed.query:
        raise ValueError("A public HTTPS dependency URL without credentials is required")
    request = urllib.request.Request(url, headers={"User-Agent": "OrcaSlicer-dependency-setup"})
    with tempfile.NamedTemporaryFile(dir=cache, prefix=archive.name + ".partial-", delete=False) as output:
        partial = Path(output.name)
        try:
            with urllib.request.urlopen(request, timeout=60) as response:
                if urllib.parse.urlsplit(response.url).scheme != "https":
                    raise ValueError("Insecure dependency download redirect")
                total, reported = 0, 0
                while block := response.read(1024 * 1024):
                    total += len(block)
                    if total > spec["bytes"]:
                        raise ValueError("Dependency download exceeds expected size")
                    output.write(block)
                    if total - reported >= 32 * 1024 * 1024:
                        print(f"Downloaded {total // (1024 * 1024)} / {spec['bytes'] // (1024 * 1024)} MiB", flush=True)
                        reported = total
            output.close()
            check_file(partial, spec)
            partial.rename(archive)
        finally:
            output.close()
            partial.unlink(missing_ok=True)
    return archive


def extract(archive, destination, lock):
    check_file(archive, {key: lock["archive"][key] for key in ("bytes", "sha256")})
    with zipfile.ZipFile(archive) as source:
        members = source.infolist()
        names = [item.filename for item in members]
        if len(names) != len({name.casefold() for name in names}):
            raise ValueError("Duplicate dependency archive entries")
        for item in members:
            relative_path(item.filename)
            if item.is_dir() or stat.S_ISLNK(item.external_attr >> 16):
                raise ValueError("Directories/symlinks are not allowed as archive entries")
        info = source.getinfo(MANIFEST)
        if info.file_size != lock["manifest"]["bytes"]:
            raise ValueError("Dependency manifest size mismatch")
        raw = source.read(MANIFEST)
        if hashlib.sha256(raw).hexdigest() != lock["manifest"]["sha256"]:
            raise ValueError("Dependency manifest hash mismatch")
        manifest = json.loads(raw)
        if manifest["schema"] != SCHEMA or manifest["version"] != lock["version"]:
            raise ValueError("Dependency archive version mismatch")
        if set(names) != set(manifest["files"]) | {MANIFEST}:
            raise ValueError("Dependency archive contains unlisted or missing files")
        for item in members:
            spec = lock["manifest"] if item.filename == MANIFEST else manifest["files"][item.filename]
            if item.file_size != spec["bytes"]:
                raise ValueError(f"Dependency expanded size mismatch: {item.filename}")
            target = destination.joinpath(*relative_path(item.filename).parts)
            target.parent.mkdir(parents=True, exist_ok=True)
            with source.open(item) as incoming, target.open("xb") as outgoing:
                shutil.copyfileobj(incoming, outgoing, 1024 * 1024)
            check_file(target, spec)


def write_init(root):
    # Bracket literals avoid interpreting Windows paths as CMake escapes/variables.
    path = root.resolve().as_posix()
    if "]==]" in path or ";" in path or "\n" in path:
        raise ValueError("Unsupported CMake dependency path")
    lines = ["# Generated locally by scripts/portrait_dependencies.py; do not commit."]
    for key, suffix in (("ORCA_AI_WEIGHTS_DIR", "weights"),
                        ("ORCA_AI_PORTRAIT_SITE_PACKAGES", "python/Lib/site-packages"),
                        ("ORCA_SEMANTIC_RUNTIME_DIR", "native-semantic")):
        lines.append(f'set({key} [==[{path}/{suffix}]==] CACHE PATH "Verified portrait dependency input" FORCE)')
    # Some existing C++ dependency layouts omit the stable ABI DLL from libpython.
    # Supply the pinned CPython 3.12 binary already verified in this dependency pack.
    lines.append(f'set(ORCA_AI_PYTHON_STABLE_ABI_DLL [==[{path}/python/python3.dll]==] CACHE FILEPATH "Verified CPython stable ABI DLL" FORCE)')
    # Existing CMake uses its own build-local wheel cache. Seed it without changing
    # global CMake cache policy or reusing another checkout's objects.
    lines += [f'file(GLOB _portrait_wheels [==[{path}/wheels/*.whl]==])',
              'file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/_deps/orca_ai_wheels")',
              'foreach(_portrait_wheel IN LISTS _portrait_wheels)',
              '  get_filename_component(_portrait_name "${_portrait_wheel}" NAME)',
              '  configure_file("${_portrait_wheel}" "${CMAKE_BINARY_DIR}/_deps/orca_ai_wheels/${_portrait_name}" COPYONLY)',
              'endforeach()']
    (root / INIT).write_text("\n".join(lines) + "\n", encoding="utf-8")


def prepare(args):
    lock = load_lock(args.lock)
    destination = args.destination.absolute()
    if destination.is_symlink():
        raise ValueError("Dependency destination must not be a symlink")
    if destination.exists():
        verify(destination, lock, imports=True)
    else:
        archive = args.archive or download(lock, args.cache)
        destination.parent.mkdir(parents=True, exist_ok=True)
        # Publish only a completely verified tree. Failure never replaces a prior tree.
        with tempfile.TemporaryDirectory(dir=destination.parent, prefix=".portrait-prepare-") as temp:
            staged = Path(temp) / "payload"
            staged.mkdir()
            extract(archive, staged, lock)
            verify(staged, lock, imports=True)
            staged.rename(destination)
    write_init(destination)
    print(f"Prepared dependencies: {destination}")
    print(f"CMake initial cache: {destination / INIT}")


def pack(args):
    """Maintainer-only: take only file-hashed inputs from a verified runtime."""
    runtime = args.runtime.resolve(strict=True)
    original = read_json(runtime / "runtime-manifest.json")
    if original["schema"] != "orca.offline-portrait-runtime/v1":
        raise ValueError("Unsupported source runtime")
    files = {}
    for name, spec in original["files"].items():
        relative_path(name)
        if name.startswith(("python/", "weights/")) and "__pycache__" not in PurePosixPath(name).parts:
            path = runtime / name
            check_file(path, {"bytes": spec["size"], "sha256": spec["sha256"]})
            files[name] = path
    # Explicit native allowlist; no local provider credentials or app configuration.
    native_names = ("libmediapipe.dll", "selfie_multiclass_256x256.tflite", "face_landmarker.task",
                    "LICENSE", "NOTICE", "runtime-manifest.json", "MODEL_LICENSES.md", "providers.json")
    for name in native_names:
        files["native-semantic/" + name] = args.native / name
    expected_native = read_json(ROOT / "cmake/semantic_runtime_manifest.json")
    if read_json(args.native / "providers.json") != expected_native["default_providers"]:
        raise ValueError("Native provider metadata must contain only the default provider IDs")
    for spec in expected_native["wheel_entries"] + [x for x in expected_native["downloads"] if x["key"] != "wheel"]:
        check_file(args.native / spec["filename"], {key: spec[key] for key in ("bytes", "sha256")})
    wheels = {name: sha for name, sha in original["packages"].items() if name.endswith(".whl")}
    if len(wheels) != 3:
        raise ValueError("Expected the three pinned NumPy/OpenCV/Pillow wheels")
    for name, sha in wheels.items():
        path = args.wheels / name
        if digest(path) != sha:
            raise ValueError(f"Pinned wheel hash mismatch: {name}")
        files["wheels/" + name] = path
    if len([name for name in files if name.startswith("weights/")]) != 4:
        raise ValueError("Expected all four portrait weights")
    manifest = {"schema": SCHEMA, "version": args.version, "platform": "windows-x64-cp312-cpu",
                "source_runtime_manifest_sha256": digest(runtime / "runtime-manifest.json"),
                "packages": original["packages"], "files": {name: record(path) for name, path in sorted(files.items())}}
    args.output.mkdir(parents=True, exist_ok=True)
    manifest_path = args.output / MANIFEST
    archive = args.output / (args.version + ".zip")
    if archive.exists() or manifest_path.exists():
        raise ValueError("Use an empty output directory; published dependency versions are immutable")
    write_json(manifest_path, manifest)
    print(f"Packing {len(files)} dependency files (application modules excluded)...", flush=True)
    with zipfile.ZipFile(archive, "x", zipfile.ZIP_DEFLATED, compresslevel=1) as target:
        target.write(manifest_path, MANIFEST)
        for name, path in sorted(files.items()):
            target.write(path, name)
    lock = {"schema": SCHEMA, "version": args.version, "platform": manifest["platform"],
            "archive": {"name": archive.name, "url": args.url, **record(archive)},
            "manifest": record(manifest_path)}
    write_json(args.output / "portrait_dependencies.json", lock)
    (args.output / "SHA256SUMS").write_text(f"{lock['archive']['sha256']}  {archive.name}\n", encoding="ascii")
    print(json.dumps(lock, indent=2), flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    setup = commands.add_parser("prepare", help="Download/verify dependencies and generate local CMake paths")
    setup.add_argument("--lock", type=Path, default=LOCK)
    setup.add_argument("--archive", type=Path, help="Use an already downloaded ZIP without network access")
    setup.add_argument("--cache", type=Path, default=ROOT / ".tmp/dependency-downloads")
    setup.add_argument("--destination", type=Path, default=ROOT / ".tmp/portrait-dependencies")
    setup.set_defaults(run=prepare)
    check = commands.add_parser("verify", help="Verify all files and isolated CPU imports")
    check.add_argument("--lock", type=Path, default=LOCK)
    check.add_argument("--destination", type=Path, default=ROOT / ".tmp/portrait-dependencies")
    check.set_defaults(run=lambda args: (verify(args.destination, load_lock(args.lock), imports=True), print("Dependencies verified")))
    bundle = commands.add_parser("pack", help="Maintainer: create a versioned dependency ZIP from verified inputs")
    for name in ("runtime", "native", "wheels", "output"):
        bundle.add_argument("--" + name, type=Path, required=True)
    bundle.add_argument("--version", required=True)
    bundle.add_argument("--url", required=True)
    bundle.set_defaults(run=pack)
    args = parser.parse_args()
    try:
        args.run(args)
    except (OSError, ValueError, KeyError, zipfile.BadZipFile, subprocess.CalledProcessError) as error:
        print(f"Portrait dependency setup failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
