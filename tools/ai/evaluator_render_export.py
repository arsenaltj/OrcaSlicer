"""Private trial exporter for paired Hi3DEval RGB and camera-space normal views.

Run against a generated, opaque GLB. This module is deliberately not connected
to the production sidecar or installed application menus.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import tempfile
from typing import Callable

import numpy as np
from PIL import Image

from local_semantic_render import double_sided_faces, load, project, raster, shade


SIZE = 512
VIEW_COUNT = 8
BACKGROUND = (245, 245, 245)
ELEVATION = math.radians(15)
MARGIN = 0.08
VERSION = "hi3deval-trial-v1"


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _camera_bases() -> list[np.ndarray]:
    bases = []
    for index in range(VIEW_COUNT):
        yaw = index * 2 * math.pi / VIEW_COUNT
        forward = np.array((math.cos(yaw) * math.cos(ELEVATION),
                            math.sin(yaw) * math.cos(ELEVATION), math.sin(ELEVATION)))
        right = np.cross((0.0, 0.0, 1.0), forward)
        right /= np.linalg.norm(right)
        up = np.cross(forward, right)
        bases.append(np.stack((right, up, forward)))
    return bases


def _framing(vertices: np.ndarray, bases: list[np.ndarray]) -> tuple[np.ndarray, float]:
    low, high = vertices.min(axis=0).astype(np.float64), vertices.max(axis=0).astype(np.float64)
    center = (low + high) / 2
    if not np.isfinite(center).all() or np.array_equal(low, high):
        raise ValueError("Model has no finite camera extent")
    # Bounding-box corners give a conservative common scale for all views.
    corners = np.array([(x, y, z) for x in (low[0], high[0])
                        for y in (low[1], high[1]) for z in (low[2], high[2])]) - center
    half_extent = max(float(np.abs(corners @ basis[:2].T).max()) for basis in bases)
    if not math.isfinite(half_extent) or half_extent <= 0:
        raise ValueError("Model has no finite camera extent")
    return center, half_extent / (1 - 2 * MARGIN)


def _face_normals(vertices: np.ndarray, faces: np.ndarray) -> np.ndarray:
    triangles = vertices[faces].astype(np.float64)
    normals = np.cross(triangles[:, 1] - triangles[:, 0], triangles[:, 2] - triangles[:, 0])
    lengths = np.linalg.norm(normals, axis=1)
    normals /= np.maximum(lengths[:, None], 1e-30)
    return normals


def _pixel_normals(ids: np.ndarray, normals: np.ndarray, basis: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    visible = ids >= 0
    camera_normals = normals[ids[visible]] @ basis.T
    # Two-sided material backs are visible too; orient their normals toward the camera.
    camera_normals[camera_normals[:, 2] < 0] *= -1
    return visible, camera_normals


def _normal_image(visible: np.ndarray, camera_normals: np.ndarray) -> np.ndarray:
    image = np.zeros((*visible.shape, 3), dtype=np.uint8)
    image[visible] = np.rint(np.clip(camera_normals * 0.5 + 0.5, 0, 1) * 255).astype(np.uint8)
    return image


def _studio_image(base: np.ndarray, visible: np.ndarray, camera_normals: np.ndarray) -> np.ndarray:
    image = base.copy()
    image[~visible] = BACKGROUND
    if not visible.any():
        return image
    key = np.array((-0.45, 0.55, 0.70))
    fill = np.array((0.65, 0.10, 0.75))
    key /= np.linalg.norm(key)
    fill /= np.linalg.norm(fill)
    illumination = (0.52 + 0.36 * np.maximum(camera_normals @ key, 0) +
                    0.18 * np.maximum(camera_normals @ fill, 0))
    srgb = base[visible].astype(np.float32) / 255
    linear = np.where(srgb <= 0.04045, srgb / 12.92, ((srgb + 0.055) / 1.055) ** 2.4)
    lit = np.clip(linear * illumination[:, None], 0, 1)
    encoded = np.where(lit <= 0.0031308, lit * 12.92, 1.055 * lit ** (1 / 2.4) - 0.055)
    image[visible] = np.rint(encoded * 255).astype(np.uint8)
    return image


def export_evaluator_views(source: Path | str, destination: Path | str,
                           progress: Callable[[int, int], None] | None = None) -> dict:
    source, destination = Path(source).resolve(), Path(destination).resolve()
    if not source.is_file() or source.suffix.lower() != ".glb":
        raise ValueError("Choose an existing opaque .glb model")
    if destination.exists():
        raise FileExistsError(f"Output directory already exists: {destination}")
    destination.parent.mkdir(parents=True, exist_ok=True)

    vertices, faces, uv, colors, materials, material_ids = load(source)
    bases = _camera_bases()
    center, half_height = _framing(vertices, bases)
    normals = _face_normals(vertices, faces)
    sided = double_sided_faces(materials, material_ids)
    manifest = {
        "version": VERSION, "source_file": source.name, "source_sha256": _sha256(source),
        "size": [SIZE, SIZE], "rgb_mode": "studio", "normal_space": "camera",
        "normal_encoding": "round(255 * (normal * 0.5 + 0.5))",
        "normal_background": [0, 0, 0], "rgb_background": list(BACKGROUND),
        "center": center.tolist(), "half_height": half_height,
        "elevation_degrees": 15, "views": [],
    }
    with tempfile.TemporaryDirectory(prefix=".eval-render-", dir=destination.parent) as temporary:
        staging = Path(temporary) / "package"
        rgb_dir, normal_dir = staging / "rgb_renders", staging / "normal_renders"
        rgb_dir.mkdir(parents=True)
        normal_dir.mkdir()
        for index, basis in enumerate(bases):
            ids, _depths, bary = raster(vertices, faces, basis, center, half_height, SIZE, sided)
            if not np.any(ids >= 0):
                raise ValueError(f"No visible mesh in view {index:02d}")
            projected = project(vertices, basis, center, half_height, SIZE)
            base = shade(faces, uv, colors, materials, material_ids, ids, bary, projected)
            visible, camera_normals = _pixel_normals(ids, normals, basis)
            name = f"view_{index:02d}.png"
            Image.fromarray(_studio_image(base, visible, camera_normals), "RGB").save(rgb_dir / name)
            Image.fromarray(_normal_image(visible, camera_normals), "RGB").save(normal_dir / name)
            manifest["views"].append({"rgb": f"rgb_renders/{name}",
                                      "normal": f"normal_renders/{name}",
                                      "azimuth_degrees": index * 45, "basis": basis.tolist()})
            if progress is not None:
                progress(index + 1, VIEW_COUNT)
        (staging / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8")
        os.replace(staging, destination)
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser(description="Export paired 512px RGB/Normal renders from one generated GLB")
    parser.add_argument("input", type=Path, help="source model.glb")
    parser.add_argument("output", type=Path, help="new output directory")
    args = parser.parse_args()
    result = export_evaluator_views(args.input, args.output,
                                    progress=lambda done, total: print(f"Rendered {done}/{total}", flush=True))
    print(f"Exported {len(result['views'])} paired views to {args.output.resolve()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
