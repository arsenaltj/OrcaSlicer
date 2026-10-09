"""Integrity check for an installed, self-contained offline portrait runtime."""
from __future__ import annotations

import hashlib
import importlib
import json
from pathlib import Path, PurePosixPath
import shutil
import sys


def digest(path):
    result = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(chunk)
    return result.hexdigest()


def checked_file(root, relative, record):
    path = PurePosixPath(relative)
    if path.is_absolute() or ".." in path.parts or "\\" in relative or ":" in relative:
        raise ValueError("unsafe_runtime_path")
    target = root.joinpath(*path.parts)
    if target.is_symlink() or not target.is_file() or not target.resolve().is_relative_to(root.resolve()):
        raise ValueError("runtime_file_missing")
    if target.stat().st_size != record["size"] or digest(target) != record["sha256"]:
        raise ValueError("runtime_hash_mismatch")


def verify(root, modules=None, imports=True):
    root = Path(root).resolve(strict=True)
    manifest = json.loads((root / "runtime-manifest.json").read_text(encoding="utf-8"))
    if manifest.get("schema") != "orca.offline-portrait-runtime/v1" or manifest.get("network") != "offline":
        raise ValueError("invalid_runtime_manifest")
    if manifest.get("provider_calls") is not False or manifest.get("model_download") is not False:
        raise ValueError("invalid_offline_policy")
    for required in ("glb_artifact.py", "local_semantic_worker.py", "local_semantic_request.py",
                     "local_contour_proposals.py", "local_surface_contours.py"):
        if required not in manifest.get("modules", {}):
            raise ValueError("incomplete_portrait_modules")
    if manifest.get("raster_required", False):
        raster = manifest.get("modules", {}).get("local_semantic_raster.dll")
        if not raster or manifest["files"].get("modules/local_semantic_raster.dll") != raster:
            raise ValueError("incomplete_portrait_accelerator")
    for relative, record in manifest["files"].items():
        checked_file(root, relative, record)
    module_root = Path(modules).resolve(strict=True) if modules else root / "modules"
    for relative, record in manifest["modules"].items():
        checked_file(module_root, relative, record)
    if imports:
        if not Path(sys.executable).resolve().is_relative_to(root / "python"):
            raise ValueError("external_python")
        for name in ("numpy", "cv2", "scipy", "PIL", "torch", "torchvision", "facer", "mediapipe"):
            module = importlib.import_module(name)
            if not Path(module.__file__).resolve().is_relative_to(root / "python"):
                raise ValueError("external_dependency")
        import cv2
        import numpy as np
        if cv2.countNonZero(np.ones((2, 2), dtype=np.uint8)) != 4:
            raise ValueError("opencv_native_unavailable")
        if str(module_root) not in sys.path:
            sys.path.insert(0, str(module_root))
        for name in ("glb_artifact", "local_semantic_request", "local_contour_proposals"):
            module = importlib.import_module(name)
            if Path(module.__file__).resolve().parent != module_root:
                raise ValueError("external_portrait_module")
        if manifest.get("raster_required", False):
            renderer = importlib.import_module("local_semantic_render")
            if Path(renderer.__file__).resolve().parent != module_root or renderer._native_raster_kernel() is None:
                raise ValueError("portrait_accelerator_unavailable")
    return {"schema":manifest["schema"],"file_count":len(manifest["files"]),
            "packages":manifest["packages"],"network":"offline","provider_calls":False,
            "raster_required":manifest.get("raster_required",False)}


def install_raster(root, source):
    """Build/install step: include the built accelerator in the isolated bundle.

    Legacy bundles remain readable. New Windows installers require the exact
    sibling DLL, so their million-face requests cannot silently use Python raster.
    """
    root = Path(root).resolve(strict=True)
    verify(root, imports=False)
    source = Path(source)
    if source.is_symlink() or not source.is_file() or not 0 < source.stat().st_size <= 2 * 1024 * 1024:
        raise ValueError("invalid_portrait_accelerator")
    expected = digest(source)
    target = root / "modules/local_semantic_raster.dll"
    if target.is_symlink() or (target.exists() and digest(target) != expected):
        raise ValueError("portrait_accelerator_identity_changed")
    if not target.exists():
        shutil.copyfile(source, target)
    if digest(target) != expected:
        raise ValueError("portrait_accelerator_identity_changed")
    manifest_path = root / "runtime-manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    record = {"size":target.stat().st_size,"sha256":expected}
    manifest["files"]["modules/local_semantic_raster.dll"] = record
    manifest["modules"]["local_semantic_raster.dll"] = record
    manifest["raster_required"] = True
    pending = manifest_path.with_suffix(".json.partial")
    with pending.open("x", encoding="utf-8") as stream:
        json.dump(manifest, stream, sort_keys=True, separators=(",", ":"))
    pending.replace(manifest_path)
    verify(root, imports=False)


if __name__ == "__main__":
    import argparse
    parser=argparse.ArgumentParser()
    parser.add_argument("--root",type=Path,required=True)
    parser.add_argument("--modules",type=Path)
    parser.add_argument("--raster",type=Path,help="Build/install only: seal the compiled raster DLL into this bundle")
    args=parser.parse_args()
    if args.raster:
        install_raster(args.root, args.raster)
    print(json.dumps(verify(args.root,args.modules),sort_keys=True))
