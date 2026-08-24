from __future__ import annotations

import hashlib
import json
import math
import os
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any, Iterable

try:
    from PIL import Image, ImageDraw, ImageFilter, ImageFont
except ImportError as exc:  # pragma: no cover - exercised by packaged environment checks.
    raise RuntimeError("Pillow is required to render model quality views.") from exc


RENDER_VERSION = "obj-painter-v2"
VIEW_SPECS = (
    ("front", (1.0, 0.0, 0.0)),
    ("right", (0.0, 1.0, 0.0)),
    ("back", (-1.0, 0.0, 0.0)),
    ("left", (0.0, -1.0, 0.0)),
    ("isometric", (1.0, -1.0, 0.65)),
)


@dataclass(frozen=True)
class ModelViewSettings:
    width: int = 448
    height: int = 448
    margin_ratio: float = 0.08
    max_render_faces: int = 240_000
    background: tuple[int, int, int] = (242, 244, 247)


class ModelViewError(ValueError):
    pass


def _normalized(vector: tuple[float, float, float]) -> tuple[float, float, float]:
    length = math.sqrt(sum(value * value for value in vector))
    if length <= 1e-12:
        raise ModelViewError("The requested camera direction is invalid.")
    return tuple(value / length for value in vector)  # type: ignore[return-value]


def _cross(left: tuple[float, float, float], right: tuple[float, float, float]) -> tuple[float, float, float]:
    return (
        left[1] * right[2] - left[2] * right[1],
        left[2] * right[0] - left[0] * right[2],
        left[0] * right[1] - left[1] * right[0],
    )


def _dot(left: tuple[float, float, float], right: tuple[float, float, float]) -> float:
    return left[0] * right[0] + left[1] * right[1] + left[2] * right[2]


def _obj_index(token: str, vertex_count: int) -> int:
    try:
        raw = int(token.split("/", 1)[0])
    except (TypeError, ValueError):
        raise ModelViewError("The OBJ contains an invalid face index.") from None
    if raw == 0:
        raise ModelViewError("OBJ face indices cannot be zero.")
    index = raw - 1 if raw > 0 else vertex_count + raw
    if index < 0 or index >= vertex_count:
        raise ModelViewError("The OBJ contains an out-of-range face index.")
    return index


def _parse_obj(
    source: Path,
) -> tuple[list[tuple[float, float, float]], list[tuple[int, int, int]], list[tuple[int, int, int]]]:
    vertices: list[tuple[float, float, float]] = []
    colors: list[tuple[int, int, int]] = []
    faces: list[tuple[int, int, int]] = []
    try:
        with source.open("r", encoding="utf-8", errors="strict") as stream:
            for raw_line in stream:
                line = raw_line.strip()
                if line.startswith("v "):
                    parts = line.split()
                    if len(parts) < 4:
                        raise ModelViewError("The OBJ contains an invalid vertex.")
                    try:
                        vertices.append((float(parts[1]), float(parts[2]), float(parts[3])))
                        if len(parts) >= 7:
                            values = tuple(float(parts[index]) for index in range(4, 7))
                            scale = 255.0 if max(values) <= 1.0 else 1.0
                            colors.append(tuple(max(0, min(255, round(value * scale))) for value in values))
                        else:
                            colors.append((184, 190, 200))
                    except ValueError:
                        raise ModelViewError("The OBJ contains an invalid vertex value.") from None
                elif line.startswith("f "):
                    tokens = line.split()[1:]
                    if len(tokens) < 3:
                        raise ModelViewError("The OBJ contains an invalid face.")
                    indices = [_obj_index(token, len(vertices)) for token in tokens]
                    faces.extend((indices[0], indices[index], indices[index + 1]) for index in range(1, len(indices) - 1))
    except UnicodeError:
        raise ModelViewError("The OBJ must be UTF-8 text.") from None
    except OSError:
        raise ModelViewError("The OBJ could not be read.") from None
    if len(vertices) < 3 or not faces:
        raise ModelViewError("The OBJ does not contain a renderable triangle mesh.")
    return vertices, colors, faces


def _sample_faces(faces: list[tuple[int, int, int]], maximum: int) -> Iterable[tuple[int, int, int]]:
    if maximum <= 0:
        raise ModelViewError("The render face limit must be positive.")
    if len(faces) <= maximum:
        return faces
    # Even sampling is deterministic and retains coverage when providers group faces by material.
    return (faces[index * len(faces) // maximum] for index in range(maximum))


def _shade(color: tuple[int, int, int], normal: tuple[float, float, float]) -> tuple[int, int, int]:
    light = _normalized((-0.35, -0.45, 1.0))
    intensity = 0.78 + 0.22 * abs(_dot(normal, light))
    return tuple(max(0, min(255, round(channel * intensity))) for channel in color)


def _render_view(
    vertices: list[tuple[float, float, float]],
    colors: list[tuple[int, int, int]],
    faces: list[tuple[int, int, int]],
    direction: tuple[float, float, float],
    settings: ModelViewSettings,
) -> Image.Image:
    camera = _normalized(direction)
    right = _normalized(_cross((0.0, 0.0, 1.0), camera))
    up = _normalized(_cross(camera, right))
    projected = [(_dot(vertex, right), _dot(vertex, up), _dot(vertex, camera)) for vertex in vertices]
    minimum_x = min(value[0] for value in projected)
    maximum_x = max(value[0] for value in projected)
    minimum_y = min(value[1] for value in projected)
    maximum_y = max(value[1] for value in projected)
    span_x = max(maximum_x - minimum_x, 1e-9)
    span_y = max(maximum_y - minimum_y, 1e-9)
    usable_width = settings.width * (1.0 - 2.0 * settings.margin_ratio)
    usable_height = settings.height * (1.0 - 2.0 * settings.margin_ratio)
    scale = min(usable_width / span_x, usable_height / span_y)
    center_x = (minimum_x + maximum_x) * 0.5
    center_y = (minimum_y + maximum_y) * 0.5

    screen = [
        (
            settings.width * 0.5 + (value[0] - center_x) * scale,
            settings.height * 0.5 - (value[1] - center_y) * scale,
            value[2],
        )
        for value in projected
    ]
    polygons: list[tuple[float, tuple[tuple[float, float], ...], tuple[int, int, int]]] = []
    for first, second, third in _sample_faces(faces, settings.max_render_faces):
        a, b, c = vertices[first], vertices[second], vertices[third]
        ab = (b[0] - a[0], b[1] - a[1], b[2] - a[2])
        ac = (c[0] - a[0], c[1] - a[1], c[2] - a[2])
        normal_raw = _cross(ab, ac)
        length = math.sqrt(_dot(normal_raw, normal_raw))
        if length <= 1e-12:
            continue
        normal = tuple(value / length for value in normal_raw)
        base = tuple(round((colors[first][channel] + colors[second][channel] + colors[third][channel]) / 3.0) for channel in range(3))
        points = tuple((screen[index][0], screen[index][1]) for index in (first, second, third))
        depth = (screen[first][2] + screen[second][2] + screen[third][2]) / 3.0
        polygons.append((depth, points, _shade(base, normal)))
    polygons.sort(key=lambda item: item[0])
    image = Image.new("RGB", (settings.width, settings.height), settings.background)
    draw = ImageDraw.Draw(image)
    for _, points, color in polygons:
        draw.polygon(points, fill=color)
    if len(faces) > settings.max_render_faces:
        # Sub-pixel triangles omitted by deterministic sampling can leave isolated pinholes.
        # A small median pass closes those holes without inventing geometry beyond the silhouette.
        image = image.filter(ImageFilter.MedianFilter(size=3))
    return image


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as stream:
            while block := stream.read(1024 * 1024):
                digest.update(block)
    except OSError:
        raise ModelViewError("The OBJ could not be hashed.") from None
    return digest.hexdigest()


def _write_json(payload: dict[str, Any], destination: Path) -> None:
    temporary = destination.with_name(destination.name + ".part")
    try:
        temporary.write_text(json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8")
        os.replace(temporary, destination)
    except OSError:
        try:
            temporary.unlink(missing_ok=True)
        except OSError:
            pass
        raise ModelViewError("The model view manifest could not be written.") from None


def _contact_sheet(images: list[tuple[str, Image.Image]], background: tuple[int, int, int]) -> Image.Image:
    label_height = 28
    gap = 12
    tile_width, tile_height = images[0][1].size
    sheet = Image.new("RGB", (tile_width * 3 + gap * 4, (tile_height + label_height) * 2 + gap * 3), background)
    draw = ImageDraw.Draw(sheet)
    font = ImageFont.load_default(size=16)
    for index, (name, image) in enumerate(images):
        column = index % 3
        row = index // 3
        x = gap + column * (tile_width + gap)
        y = gap + row * (tile_height + label_height + gap)
        sheet.paste(image, (x, y + label_height))
        draw.text((x + 8, y + 5), name.replace("isometric", "Isometric").title(), fill=(40, 45, 55), font=font)
    return sheet


def render_model_views(
    obj_path: Path | str,
    job_directory: Path | str,
    settings: ModelViewSettings | None = None,
    *,
    force: bool = False,
) -> dict[str, Any]:
    source = Path(obj_path)
    root = Path(job_directory)
    config = settings or ModelViewSettings()
    if config.width < 128 or config.height < 128 or not (0.0 <= config.margin_ratio < 0.4):
        raise ModelViewError("The model view settings are invalid.")
    settings_payload = asdict(config)
    settings_payload["background"] = list(config.background)
    digest = _sha256(source)
    views_directory = root / "model-views"
    manifest_path = views_directory / "manifest.json"
    sheet_path = root / "model-view-sheet.png"
    expected = [views_directory / f"{name}.png" for name, _ in VIEW_SPECS]
    if not force and manifest_path.is_file() and sheet_path.is_file() and all(path.is_file() for path in expected):
        try:
            cached = json.loads(manifest_path.read_text(encoding="utf-8"))
            if cached.get("render_version") == RENDER_VERSION and cached.get("obj_sha256") == digest and cached.get("settings") == settings_payload:
                cached["cached"] = True
                return cached
        except (OSError, UnicodeError, json.JSONDecodeError):
            pass

    vertices, colors, faces = _parse_obj(source)
    try:
        views_directory.mkdir(parents=True, exist_ok=True)
    except OSError:
        raise ModelViewError("The model view directory could not be created.") from None
    images: list[tuple[str, Image.Image]] = []
    for name, direction in VIEW_SPECS:
        image = _render_view(vertices, colors, faces, direction, config)
        destination = views_directory / f"{name}.png"
        try:
            image.save(destination, format="PNG", optimize=True)
        except OSError:
            raise ModelViewError("A model quality view could not be written.") from None
        images.append((name, image))
    sheet = _contact_sheet(images, config.background)
    try:
        sheet.save(sheet_path, format="PNG", optimize=True)
    except OSError:
        raise ModelViewError("The model view sheet could not be written.") from None
    payload: dict[str, Any] = {
        "render_version": RENDER_VERSION,
        "obj_sha256": digest,
        "settings": settings_payload,
        "vertex_count": len(vertices),
        "face_count": len(faces),
        "rendered_face_count": min(len(faces), config.max_render_faces),
        "views": [f"model-views/{name}.png" for name, _ in VIEW_SPECS],
        "sheet": sheet_path.name,
        "cached": False,
    }
    _write_json(payload, manifest_path)
    return payload
