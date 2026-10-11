"""Bake or quantize vertex colors on explicitly supplied model files."""
from __future__ import annotations

import re
import shutil
from array import array
from collections import deque
from model_color_space import (
    _portrait_material_palette_indices,
    _portrait_material_role_indices,
    _semantic_palette_index,
    _vertex_color_data,
)
from model_contracts import MAX_TEXTURE_PIXELS
from model_obj_io import _read_material_textures, _read_obj_geometry, _resolve_obj_index
from pathlib import Path
from tripo_client import TripoError
from typing import Any, Mapping

def _bake_obj_texture_to_vertex_colors(
    obj_path: Path,
    package_dir: Path,
    destination: Path,
    palette: tuple[str, ...],
    palette_roles: Mapping[str, str] | None = None,
    portrait_materials: bool = False,
    natural_destination: Path | None = None,
) -> None:
    try:
        from PIL import Image, UnidentifiedImageError
    except ImportError:
        raise TripoError("Pillow is required to convert OBJ textures into printable vertex colors.") from None

    positions, texcoords, material_references = _read_obj_geometry(obj_path)
    texture_paths = _read_material_textures(obj_path, package_dir, material_references)
    palette_rgb, palette_lab = _vertex_color_data(palette)
    nearest_cache: dict[tuple[int, int, int], int] = {}
    portrait_indices = _portrait_material_palette_indices(
        palette, palette_roles, portrait_materials
    )
    portrait_role_indices = _portrait_material_role_indices(
        palette, palette_roles, portrait_materials
    )
    images: dict[str, Any] = {}
    try:
        for material, texture_path in texture_paths.items():
            try:
                image = Image.open(texture_path)
                if image.width <= 0 or image.height <= 0 or image.width * image.height > MAX_TEXTURE_PIXELS:
                    image.close()
                    raise TripoError("The generated base-color texture has an invalid size.")
                image.load()
                images[material] = image.convert("RGB")
                image.close()
            except (OSError, UnidentifiedImageError, Image.DecompressionBombError):
                raise TripoError("The generated base-color texture could not be decoded.") from None

        face_path = destination.with_suffix(".faces.tmp")
        output_sources = array("I")
        output_color_counts: list[list[int]] = []
        output_color_sums = array("Q")
        output_sample_counts = array("I")
        output_vertices: dict[int, int] = {}
        face_vertices = array("I")
        face_count = 0
        current_material = ""
        try:
            source_stream = obj_path.open("r", encoding="utf-8", errors="strict")
            face_stream = face_path.open("w", encoding="ascii", newline="\n")
        except OSError:
            raise TripoError("The generated OBJ could not be converted.") from None
        with source_stream, face_stream:
            for line in source_stream:
                stripped = line.strip()
                if not stripped or stripped.startswith("#"):
                    continue
                fields = stripped.split()
                keyword = fields[0].lower()
                if keyword == "usemtl":
                    current_material = stripped[len(fields[0]) :].strip()
                    safe_material = re.sub(r"[^A-Za-z0-9_.-]+", "_", current_material).strip("_") or "material"
                    face_stream.write("g material_" + safe_material + "\n")
                    continue
                if keyword in {"o", "g"}:
                    name = stripped[len(fields[0]) :].strip()
                    safe_name = re.sub(r"[^A-Za-z0-9_.-]+", "_", name).strip("_") or keyword
                    face_stream.write(keyword + " " + safe_name + "\n")
                    continue
                if keyword != "f":
                    continue
                if len(fields) != 4:
                    raise TripoError("The generated OBJ must contain only triangular faces.")
                material = current_material
                if not material and len(images) == 1:
                    material = next(iter(images))
                image = images.get(material)
                if image is None:
                    raise TripoError("The generated OBJ face is missing a base-color material.")
                pixels = image.load()
                face_indices: list[str] = []
                for field in fields[1:]:
                    indices = field.split("/")
                    if len(indices) < 2 or not indices[0] or not indices[1]:
                        raise TripoError("The generated OBJ face is missing texture coordinates.")
                    vertex_index = _resolve_obj_index(indices[0], len(positions) // 3, "vertex")
                    texcoord_index = _resolve_obj_index(indices[1], len(texcoords) // 2, "texture coordinate")
                    output_index = output_vertices.get(vertex_index)
                    if output_index is None:
                        output_sources.append(vertex_index)
                        output_index = len(output_sources)
                        output_vertices[vertex_index] = output_index
                        output_color_counts.append([0] * len(palette_rgb))
                        output_color_sums.extend((0, 0, 0))
                        output_sample_counts.append(0)
                    u = max(0.0, min(1.0, texcoords[texcoord_index * 2]))
                    v = max(0.0, min(1.0, texcoords[texcoord_index * 2 + 1]))
                    x = min(image.width - 1, max(0, round(u * (image.width - 1))))
                    y = min(image.height - 1, max(0, round((1.0 - v) * (image.height - 1))))
                    sampled = tuple(pixels[x, y])
                    if palette_rgb:
                        palette_index = _semantic_palette_index(
                            sampled,
                            palette_lab,
                            nearest_cache,
                            portrait_indices,
                            portrait_role_indices,
                        )
                        output_color_counts[output_index - 1][palette_index] += 1
                    if not palette_rgb or natural_destination is not None:
                        color_offset = (output_index - 1) * 3
                        for channel in range(3):
                            output_color_sums[color_offset + channel] += sampled[channel]
                        output_sample_counts[output_index - 1] += 1
                    face_indices.append(str(output_index))
                    face_vertices.append(output_index - 1)
                face_stream.write("f " + " ".join(face_indices) + "\n")
                face_count += 1

        if not output_sources or not face_count:
            raise TripoError("The generated OBJ does not contain textured faces.")
        output_palette_indices: list[int] = []
        if palette_rgb:
            output_palette_indices = [
                max(range(len(counts)), key=lambda item: (counts[item], -item))
                for counts in output_color_counts
            ]
            # Orca intentionally uses two-color triangles to encode an MMU boundary. Three-color triangles are much harder to
            # print predictably. Relabel the highest palette index in each offending face to one of its two lower labels. Every
            # edit is monotonic, so adjacent faces cannot oscillate forever; shared vertices and watertight topology stay intact.
            vertex_faces: list[list[int]] = [[] for _ in output_palette_indices]
            pending: deque[int] = deque()
            queued = bytearray(face_count)
            for face_index, offset in enumerate(range(0, len(face_vertices), 3)):
                vertices = face_vertices[offset:offset + 3]
                for vertex in vertices:
                    vertex_faces[vertex].append(face_index)
                if len({output_palette_indices[index] for index in vertices}) == 3:
                    pending.append(face_index)
                    queued[face_index] = 1
            changes = 0
            max_changes = len(output_palette_indices) * max(1, len(palette_rgb) - 1)
            while pending:
                face_index = pending.popleft()
                queued[face_index] = 0
                offset = face_index * 3
                vertices = face_vertices[offset:offset + 3]
                labels = [output_palette_indices[index] for index in vertices]
                if len(set(labels)) < 3:
                    continue
                current = max(labels)
                corner = labels.index(current)
                vertex = vertices[corner]
                counts = output_color_counts[vertex]
                targets = [label for label in labels if label < current]
                target = max(targets, key=lambda label: (counts[label], -label))
                output_palette_indices[vertex] = target
                changes += 1
                if changes > max_changes:
                    raise TripoError("The printable vertex-color pass did not converge.")
                for adjacent_face in vertex_faces[vertex]:
                    if not queued[adjacent_face]:
                        pending.append(adjacent_face)
                        queued[adjacent_face] = 1
            remaining_three_color_faces = sum(
                len({output_palette_indices[index] for index in face_vertices[offset:offset + 3]}) == 3
                for offset in range(0, len(face_vertices), 3)
            )
            if remaining_three_color_faces:
                raise TripoError(
                    f"The printable vertex-color pass left {remaining_three_color_faces} three-color triangles."
                )
        def write_output(output_path: Path, *, use_palette: bool) -> None:
            with output_path.open("w", encoding="ascii", newline="\n") as output:
                output.write("# OrcaSlicer AI vertex-color OBJ\n")
                output.write(f"# Source package: {obj_path.name}\n")
                for output_index, source_index in enumerate(output_sources):
                    offset = source_index * 3
                    if use_palette:
                        palette_index = output_palette_indices[output_index]
                        red, green, blue = palette_rgb[palette_index]
                    else:
                        samples = max(1, output_sample_counts[output_index])
                        color_offset = output_index * 3
                        red, green, blue = (
                            round(output_color_sums[color_offset + channel] / samples) for channel in range(3)
                        )
                    output.write(
                        "v {:.9g} {:.9g} {:.9g} {:.6f} {:.6f} {:.6f}\n".format(
                            positions[offset], positions[offset + 1], positions[offset + 2],
                            red / 255.0,
                            green / 255.0,
                            blue / 255.0,
                        )
                    )
                with face_path.open("r", encoding="ascii") as faces:
                    shutil.copyfileobj(faces, output, length=1024 * 1024)

        try:
            write_output(destination, use_palette=bool(palette_rgb))
            if natural_destination is not None:
                if natural_destination == destination:
                    raise TripoError("The natural portrait reference path must be separate from the printable model.")
                write_output(natural_destination, use_palette=False)
        except OSError:
            raise TripoError("The vertex-color OBJ could not be saved.") from None
        finally:
            try:
                face_path.unlink(missing_ok=True)
            except OSError:
                pass
    finally:
        for image in images.values():
            image.close()

def _quantize_vertex_color_obj(
    source: Path,
    destination: Path,
    palette: tuple[str, ...],
    palette_roles: Mapping[str, str] | None = None,
    portrait_materials: bool = False,
) -> None:
    if not palette:
        try:
            shutil.copyfile(source, destination)
        except OSError:
            raise TripoError("The generated OBJ could not be copied.") from None
        return
    palette_rgb, palette_lab = _vertex_color_data(palette)
    nearest_cache: dict[tuple[int, int, int], int] = {}
    portrait_indices = _portrait_material_palette_indices(
        palette, palette_roles, portrait_materials
    )
    portrait_role_indices = _portrait_material_role_indices(
        palette, palette_roles, portrait_materials
    )
    try:
        with source.open("r", encoding="utf-8", errors="strict") as input_stream, destination.open(
            "w", encoding="ascii", newline="\n"
        ) as output:
            output.write(
                "# OrcaSlicer AI palette-constrained vertex-color OBJ\n"
                if palette
                else "# OrcaSlicer AI natural vertex-color OBJ\n"
            )
            for line in input_stream:
                fields = line.strip().split()
                if not fields or fields[0].startswith("#"):
                    continue
                keyword = fields[0].lower()
                if keyword == "v":
                    if len(fields) not in {7, 8}:
                        raise TripoError("The generated OBJ does not provide valid vertex colors.")
                    try:
                        values = [float(value) for value in fields[1:7]]
                    except ValueError:
                        raise TripoError("The generated OBJ has an invalid vertex.") from None
                    sampled = tuple(round(max(0.0, min(1.0, value)) * 255) for value in values[3:6])
                    palette_index = _semantic_palette_index(
                        sampled,
                        palette_lab,
                        nearest_cache,
                        portrait_indices,
                        portrait_role_indices,
                    )
                    red, green, blue = palette_rgb[palette_index]
                    output.write(
                        "v {:.9g} {:.9g} {:.9g} {:.6f} {:.6f} {:.6f}\n".format(
                            values[0], values[1], values[2], red / 255.0, green / 255.0, blue / 255.0
                        )
                    )
                elif keyword == "f":
                    output.write("f " + " ".join(field.split("/", 1)[0] for field in fields[1:]) + "\n")
                elif keyword in {"o", "g"}:
                    output.write(" ".join(fields) + "\n")
    except UnicodeDecodeError:
        raise TripoError("The generated OBJ is not valid UTF-8 text.") from None
    except OSError:
        raise TripoError("The generated OBJ could not be color constrained.") from None
