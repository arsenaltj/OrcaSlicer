"""Bounded model archive, OBJ IO and artifact validation."""
from __future__ import annotations
import json
import math
import os
import re
import stat
import zipfile
from array import array
from collections import Counter
from glb_artifact import Glb, GlbError
from model_color_space import _palette_data
from model_contracts import (
    DEFAULT_MODEL_SIZE_MM,
    MAX_ARCHIVE_FILES,
    MAX_ARTIFACT_BYTES,
    MAX_UNPACKED_BYTES,
)
from model_job_support import MAX_MODEL_FACES
from pathlib import Path
from tripo_client import TripoError
from typing import Any


def _safe_package_path(root: Path, name: str) -> Path:
    normalized = name.replace("\\", "/")
    if not normalized or normalized.startswith("/") or re.match(r"^[A-Za-z]:", normalized):
        raise TripoError("The generated OBJ package contains an unsafe path.")
    parts = normalized.split("/")
    if any(part in {"", ".", ".."} for part in parts):
        raise TripoError("The generated OBJ package contains an unsafe path.")
    destination = root.joinpath(*parts).resolve()
    try:
        destination.relative_to(root.resolve())
    except ValueError:
        raise TripoError("The generated OBJ package contains an unsafe path.") from None
    return destination

def _extract_obj_package(archive: Path, package_dir: Path) -> Path:
    try:
        with zipfile.ZipFile(archive) as bundle:
            members = bundle.infolist()
            if not members or len(members) > MAX_ARCHIVE_FILES:
                raise TripoError("The generated OBJ package contains an invalid number of files.")
            total_size = 0
            destinations: set[Path] = set()
            for member in members:
                mode = member.external_attr >> 16
                if stat.S_ISLNK(mode):
                    raise TripoError("The generated OBJ package contains a symbolic link.")
                file_type = stat.S_IFMT(mode)
                if file_type not in {0, stat.S_IFREG, stat.S_IFDIR}:
                    raise TripoError("The generated OBJ package contains an unsupported file type.")
                if member.flag_bits & 0x1:
                    raise TripoError("The generated OBJ package must not be encrypted.")
                destination = _safe_package_path(package_dir, member.filename)
                if destination in destinations:
                    raise TripoError("The generated OBJ package contains duplicate paths.")
                destinations.add(destination)
                if not member.is_dir():
                    total_size += member.file_size
                    if total_size > MAX_UNPACKED_BYTES:
                        raise TripoError("The generated OBJ package is too large after extraction.")

            package_dir.mkdir(parents=True, exist_ok=False)
            extracted_size = 0
            for member in members:
                destination = _safe_package_path(package_dir, member.filename)
                if member.is_dir():
                    destination.mkdir(parents=True, exist_ok=True)
                    continue
                destination.parent.mkdir(parents=True, exist_ok=True)
                try:
                    source = bundle.open(member)
                    target = destination.open("xb")
                except (OSError, RuntimeError, zipfile.BadZipFile):
                    raise TripoError("The generated OBJ package could not be extracted.") from None
                with source, target:
                    while True:
                        chunk = source.read(1024 * 1024)
                        if not chunk:
                            break
                        extracted_size += len(chunk)
                        if extracted_size > MAX_UNPACKED_BYTES:
                            raise TripoError("The generated OBJ package is too large after extraction.")
                        target.write(chunk)
    except TripoError:
        raise
    except (OSError, RuntimeError, zipfile.BadZipFile, zipfile.LargeZipFile):
        raise TripoError("Tripo returned an invalid OBJ package.") from None

    objects = [path for path in package_dir.rglob("*") if path.is_file() and path.suffix.lower() == ".obj"]
    if len(objects) != 1:
        raise TripoError("The generated OBJ package must contain exactly one OBJ model.")
    return objects[0]

def _obj_dependency_path(package_dir: Path, parent: Path, value: str, kind: str) -> Path:
    value = value.strip().strip('"')
    if not value:
        raise TripoError(f"The generated OBJ has an invalid {kind} reference.")
    normalized = value.replace("\\", "/")
    if normalized.startswith("/") or re.match(r"^[A-Za-z]:", normalized):
        raise TripoError(f"The generated OBJ has an unsafe {kind} reference.")
    destination = parent.joinpath(*normalized.split("/")).resolve()
    try:
        destination.relative_to(package_dir.resolve())
    except ValueError:
        raise TripoError(f"The generated OBJ has an unsafe {kind} reference.") from None
    if not destination.is_file():
        raise TripoError(f"The generated OBJ is missing its {kind} file.")
    return destination

def _read_obj_geometry(obj_path: Path) -> tuple[array, array, list[str]]:
    positions = array("d")
    texcoords = array("d")
    material_libraries: list[str] = []
    try:
        with obj_path.open("r", encoding="utf-8", errors="strict") as stream:
            for line in stream:
                stripped = line.strip()
                if not stripped or stripped.startswith("#"):
                    continue
                fields = stripped.split()
                keyword = fields[0].lower()
                if keyword == "v":
                    if len(fields) < 4:
                        raise TripoError("The generated OBJ has an invalid vertex.")
                    try:
                        values = [float(value) for value in fields[1:4]]
                    except ValueError:
                        raise TripoError("The generated OBJ has an invalid vertex.") from None
                    if not all(math.isfinite(value) for value in values):
                        raise TripoError("The generated OBJ has an invalid vertex.")
                    positions.extend(values)
                elif keyword == "vt":
                    if len(fields) < 3:
                        raise TripoError("The generated OBJ has an invalid texture coordinate.")
                    try:
                        values = [float(value) for value in fields[1:3]]
                    except ValueError:
                        raise TripoError("The generated OBJ has an invalid texture coordinate.") from None
                    if not all(math.isfinite(value) for value in values):
                        raise TripoError("The generated OBJ has an invalid texture coordinate.")
                    texcoords.extend(values)
                elif keyword == "mtllib":
                    reference = stripped[len(fields[0]) :].strip()
                    if reference:
                        material_libraries.append(reference)
    except UnicodeDecodeError:
        raise TripoError("The generated OBJ is not valid UTF-8 text.") from None
    except OSError:
        raise TripoError("The generated OBJ could not be read.") from None
    if not positions or not texcoords:
        raise TripoError("The generated OBJ does not contain textured geometry.")
    if not material_libraries:
        raise TripoError("The generated OBJ is missing its material library.")
    return positions, texcoords, material_libraries

def _read_material_textures(obj_path: Path, package_dir: Path, references: list[str]) -> dict[str, Path]:
    textures: dict[str, Path] = {}
    for reference in references:
        material_path = _obj_dependency_path(package_dir, obj_path.parent, reference, "material")
        current_material = ""
        try:
            with material_path.open("r", encoding="utf-8", errors="strict") as stream:
                for line in stream:
                    stripped = line.strip()
                    if not stripped or stripped.startswith("#"):
                        continue
                    fields = stripped.split(maxsplit=1)
                    keyword = fields[0].lower()
                    value = fields[1].strip() if len(fields) > 1 else ""
                    if keyword == "newmtl":
                        current_material = value
                    elif keyword == "map_kd" and current_material:
                        # Tripo emits a plain filename. Taking the final token also
                        # tolerates standard map_Kd options such as -s or -o.
                        texture_reference = value.strip().strip('"')
                        direct = material_path.parent / texture_reference
                        if not direct.is_file():
                            texture_reference = value.split()[-1].strip('"') if value.split() else ""
                        textures[current_material] = _obj_dependency_path(
                            package_dir, material_path.parent, texture_reference, "base-color texture"
                        )
        except UnicodeDecodeError:
            raise TripoError("The generated material library is not valid UTF-8 text.") from None
        except OSError:
            raise TripoError("The generated material library could not be read.") from None
    if not textures:
        raise TripoError("The generated OBJ is missing its base-color texture.")
    return textures

def _resolve_obj_index(value: str, count: int, kind: str) -> int:
    try:
        index = int(value)
    except ValueError:
        raise TripoError(f"The generated OBJ has an invalid {kind} index.") from None
    if index == 0:
        raise TripoError(f"The generated OBJ has an invalid {kind} index.")
    resolved = index - 1 if index > 0 else count + index
    if resolved < 0 or resolved >= count:
        raise TripoError(f"The generated OBJ references a missing {kind}.")
    return resolved

def _normalize_obj_for_orca(path: Path, target_size_mm: float = DEFAULT_MODEL_SIZE_MM) -> None:
    minimum = [math.inf, math.inf, math.inf]
    maximum = [-math.inf, -math.inf, -math.inf]
    try:
        with path.open("r", encoding="utf-8", errors="strict") as stream:
            for line in stream:
                fields = line.strip().split()
                if not fields or fields[0].lower() != "v" or len(fields) not in {7, 8}:
                    continue
                values = [float(value) for value in fields[1:4]]
                for axis, value in enumerate(values):
                    minimum[axis] = min(minimum[axis], value)
                    maximum[axis] = max(maximum[axis], value)
    except (OSError, UnicodeDecodeError, ValueError):
        raise TripoError("The generated OBJ could not be normalized for OrcaSlicer.") from None
    spans = [maximum[axis] - minimum[axis] for axis in range(3)]
    largest_span = max(spans)
    if not math.isfinite(largest_span) or largest_span <= 1e-9 or target_size_mm <= 0:
        raise TripoError("The generated OBJ has invalid dimensions.")

    scale = target_size_mm / largest_span
    center_x = (minimum[0] + maximum[0]) * 0.5
    center_z = (minimum[2] + maximum[2]) * 0.5
    temporary = path.with_name(path.name + ".normalized")
    try:
        with path.open("r", encoding="utf-8", errors="strict") as source, temporary.open(
            "w", encoding="ascii", newline="\n"
        ) as output:
            output.write("# OrcaSlicer AI normalized: Z-up, centered, on-bed, 100 mm maximum dimension\n")
            for line in source:
                fields = line.strip().split()
                if fields and fields[0].lower() == "v" and len(fields) in {7, 8}:
                    values = [float(value) for value in fields[1:]]
                    x = (values[0] - center_x) * scale
                    y = -(values[2] - center_z) * scale
                    z = (values[1] - minimum[1]) * scale
                    output.write(
                        "v {:.9g} {:.9g} {:.9g} {}\n".format(
                            x, y, z, " ".join("{:.6f}".format(value) for value in values[3:])
                        )
                    )
                elif fields and fields[0].lower() == "f":
                    output.write("f " + " ".join(fields[1:]) + "\n")
                elif fields and fields[0].lower() in {"o", "g"}:
                    output.write(" ".join(fields) + "\n")
        os.replace(temporary, path)
    except (OSError, UnicodeDecodeError, ValueError):
        raise TripoError("The generated OBJ could not be normalized for OrcaSlicer.") from None
    finally:
        try:
            temporary.unlink(missing_ok=True)
        except OSError:
            pass

def _validate_obj_vertex_colors(path: Path) -> None:
    vertices: list[bool] = []
    referenced_vertices: set[int] = set()
    try:
        with path.open("r", encoding="utf-8", errors="strict") as stream:
            for line in stream:
                stripped = line.strip()
                if not stripped or stripped.startswith("#"):
                    continue
                fields = stripped.split()
                keyword = fields[0].lower()
                if keyword in {"mtllib", "usemtl", "vt", "vn", "map_kd"}:
                    raise TripoError("The generated OBJ depends on external materials or textures.")
                if keyword == "v":
                    if len(fields) not in {7, 8}:
                        vertices.append(False)
                        continue
                    try:
                        values = [float(value) for value in fields[1:]]
                    except ValueError:
                        vertices.append(False)
                        continue
                    if not all(math.isfinite(value) for value in values):
                        vertices.append(False)
                        continue
                    colors = values[3:]
                    vertices.append(all(0.0 <= value <= 1.0 for value in colors))
                elif keyword == "f":
                    if len(fields) < 4:
                        raise TripoError("The generated OBJ has an invalid face.")
                    for field in fields[1:]:
                        if "/" in field:
                            raise TripoError("The generated OBJ contains unsupported texture or normal references.")
                        try:
                            index = int(field)
                        except ValueError:
                            raise TripoError("The generated OBJ has an invalid vertex index.") from None
                        if index == 0:
                            raise TripoError("The generated OBJ has an invalid vertex index.")
                        resolved = index - 1 if index > 0 else len(vertices) + index
                        if resolved < 0 or resolved >= len(vertices):
                            raise TripoError("The generated OBJ references a missing vertex.")
                        referenced_vertices.add(resolved)
    except UnicodeDecodeError:
        raise TripoError("The generated OBJ is not valid UTF-8 text.") from None
    except OSError:
        raise TripoError("The generated OBJ could not be read.") from None
    if not referenced_vertices or any(not vertices[index] for index in referenced_vertices):
        raise TripoError("The generated OBJ does not provide valid vertex colors.")

def _validate_obj_palette(path: Path, palette: tuple[str, ...]) -> None:
    allowed = set(_palette_data(palette)[0])
    found = False
    try:
        with path.open("r", encoding="utf-8", errors="strict") as stream:
            for line in stream:
                fields = line.strip().split()
                if not fields or fields[0].lower() != "v":
                    continue
                if len(fields) not in {7, 8}:
                    raise TripoError("The generated OBJ does not provide valid vertex colors.")
                try:
                    color = tuple(round(float(value) * 255) for value in fields[4:7])
                except ValueError:
                    raise TripoError("The generated OBJ has an invalid vertex color.") from None
                if color not in allowed:
                    raise TripoError("The generated OBJ contains colors outside the printable filament palette.")
                found = True
    except UnicodeDecodeError:
        raise TripoError("The generated OBJ is not valid UTF-8 text.") from None
    except OSError:
        raise TripoError("The generated OBJ could not be read.") from None
    if not found:
        raise TripoError("The generated OBJ does not provide valid vertex colors.")

def _obj_vertex_color_metrics(path: Path) -> dict[str, Any]:
    positions: list[tuple[float, float, float]] = []
    colors: list[tuple[int, int, int]] = []
    faces: list[tuple[int, int, int]] = []
    try:
        with path.open("r", encoding="utf-8", errors="strict") as stream:
            for line in stream:
                fields = line.strip().split()
                if not fields or fields[0].startswith("#"):
                    continue
                if fields[0].lower() == "v":
                    if len(fields) not in {7, 8}:
                        raise TripoError("The generated OBJ does not provide valid vertex colors.")
                    positions.append(tuple(float(value) for value in fields[1:4]))
                    colors.append(tuple(round(float(value) * 255) for value in fields[4:7]))
                elif fields[0].lower() == "f":
                    if len(fields) != 4:
                        raise TripoError("The generated OBJ must contain only triangular faces.")
                    faces.append(tuple(_resolve_obj_index(value, len(colors), "vertex") for value in fields[1:]))
    except (OSError, UnicodeDecodeError, ValueError):
        raise TripoError("The generated OBJ color metrics could not be calculated.") from None
    distribution = Counter(len({colors[index] for index in face}) for face in faces)
    vertex_usage = Counter(colors)
    face_areas = [_obj_triangle_area(positions, face) for face in faces]
    surface_area = sum(face_areas)
    mixed_surface_area = sum(
        area for face, area in zip(faces, face_areas) if len({colors[index] for index in face}) > 1
    )
    total = max(1, len(faces))
    return {
        "vertex_count": len(colors),
        "face_count": len(faces),
        "vertex_color_count": len(vertex_usage),
        "uniform_faces": distribution[1],
        "two_color_faces": distribution[2],
        "three_color_faces": distribution[3],
        "two_color_face_ratio": round(distribution[2] / total, 6),
        "three_color_face_ratio": round(distribution[3] / total, 6),
        "mixed_face_count": distribution[2] + distribution[3],
        "mixed_face_ratio": round((distribution[2] + distribution[3]) / total, 6),
        "surface_area_mm2": round(surface_area, 6),
        "mixed_face_surface_area_mm2": round(mixed_surface_area, 6),
        "mixed_face_surface_area_ratio": round(mixed_surface_area / surface_area, 6) if surface_area > 0.0 else 0.0,
        "vertex_color_usage": {
            "#{:02X}{:02X}{:02X}".format(*color): count for color, count in sorted(vertex_usage.items())
        },
    }

def _write_obj_vertex_color_metrics(path: Path, report_path: Path) -> dict[str, Any]:
    metrics = _obj_vertex_color_metrics(path)
    temporary = report_path.with_suffix(report_path.suffix + ".part")
    try:
        temporary.write_text(json.dumps(metrics, ensure_ascii=False, indent=2), encoding="utf-8")
        os.replace(temporary, report_path)
    except OSError:
        try:
            temporary.unlink(missing_ok=True)
        except OSError:
            pass
        raise TripoError("The generated OBJ color metrics could not be saved.") from None
    return metrics

def _obj_triangle_area(
    positions: list[tuple[float, float, float]], face: tuple[int, int, int]
) -> float:
    left, right, third = (positions[index] for index in face)
    ab = tuple(right[axis] - left[axis] for axis in range(3))
    ac = tuple(third[axis] - left[axis] for axis in range(3))
    cross = (
        ab[1] * ac[2] - ab[2] * ac[1],
        ab[2] * ac[0] - ab[0] * ac[2],
        ab[0] * ac[1] - ab[1] * ac[0],
    )
    return 0.5 * math.sqrt(sum(value * value for value in cross))

def _write_mesh_repair_report(path: Path, report: dict[str, Any]) -> None:
    temporary = path.with_suffix(path.suffix + ".part")
    try:
        temporary.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
        os.replace(temporary, path)
    except OSError:
        try:
            temporary.unlink(missing_ok=True)
        except OSError:
            pass

def _validate_obj_topology(
    path: Path, allow_repairable: bool = False, *, quality_advisory: bool = False
) -> tuple[int, int, int]:
    vertex_count = 0
    faces: list[tuple[int, int, int]] = []
    try:
        with path.open("r", encoding="utf-8", errors="strict") as stream:
            for line in stream:
                fields = line.strip().split()
                if not fields or fields[0].startswith("#"):
                    continue
                keyword = fields[0].lower()
                if keyword == "v":
                    vertex_count += 1
                elif keyword == "f":
                    if len(fields) != 4:
                        raise TripoError("The generated OBJ must contain only triangular faces.")
                    face = tuple(_resolve_obj_index(value, vertex_count, "vertex") for value in fields[1:])
                    if len(set(face)) != 3 and not quality_advisory:
                        raise TripoError("The generated OBJ contains a degenerate triangle.")
                    faces.append(face)
                    if len(faces) > MAX_MODEL_FACES:
                        raise TripoError(f"The generated OBJ exceeds the {MAX_MODEL_FACES}-triangle limit.")
    except UnicodeDecodeError:
        raise TripoError("The generated OBJ is not valid UTF-8 text.") from None
    except OSError:
        raise TripoError("The generated OBJ could not be read.") from None
    if vertex_count == 0 or not faces:
        raise TripoError("The generated OBJ does not contain usable geometry.")

    # Production checks file integrity here. The quality report already
    # calculates components and edge topology; repeating its large edge map
    # twice during delivery both wastes time and turns advice into a gate.
    if quality_advisory:
        return len(faces), 0, 0

    parent = list(range(vertex_count))

    def find(index: int) -> int:
        while parent[index] != index:
            parent[index] = parent[parent[index]]
            index = parent[index]
        return index

    def unite(left: int, right: int) -> None:
        left_root, right_root = find(left), find(right)
        if left_root != right_root:
            parent[right_root] = left_root

    referenced: set[int] = set()
    edge_uses: dict[tuple[int, int], list[tuple[int, int]]] = {}
    for face in faces:
        referenced.update(face)
        unite(face[0], face[1])
        unite(face[1], face[2])
        for left, right in ((face[0], face[1]), (face[1], face[2]), (face[2], face[0])):
            edge = (left, right) if left < right else (right, left)
            edge_uses.setdefault(edge, []).append((left, right))

    component_count = len({find(index) for index in referenced})
    invalid_edges = sum(
        len(uses) != 2 or (len(uses) == 2 and uses[0] == uses[1])
        for uses in edge_uses.values()
    )
    repairable_edge_limit = max(64, len(faces) // 100)
    if invalid_edges and (
        not allow_repairable or len(faces) < 4 or invalid_edges > repairable_edge_limit
    ):
        raise TripoError(
            "Tripo generated a non-watertight, non-manifold, or inconsistently wound mesh. "
            "Regenerate before importing into OrcaSlicer."
        )
    return len(faces), component_count, invalid_edges

def _validate_artifact(path: Path, format_name: str, allow_repairable_obj: bool = False) -> int:
    try:
        size = path.stat().st_size
        with path.open("rb") as stream:
            signature = stream.read(84)
    except OSError:
        raise TripoError("The generated artifact could not be read.") from None
    if size <= 0 or size > MAX_ARTIFACT_BYTES:
        raise TripoError("The generated artifact has an invalid size.")
    if format_name == "obj":
        _validate_obj_vertex_colors(path)
        _validate_obj_topology(path, allow_repairable=allow_repairable_obj, quality_advisory=True)
    if format_name == "glb":
        try:
            Glb(path).mesh(with_colors=False)
        except (GlbError, OSError, ValueError, KeyError, TypeError) as exc:
            raise TripoError(f"The generated GLB is invalid: {exc}") from None
    if format_name == "3mf" and not signature.startswith(b"PK\x03\x04"):
        raise TripoError("Tripo returned an invalid 3MF artifact.")
    if format_name == "stl":
        ascii_stl = signature.lstrip().lower().startswith(b"solid")
        binary_stl = len(signature) >= 84 and int.from_bytes(signature[80:84], "little") > 0
        if not ascii_stl and not binary_stl:
            raise TripoError("Tripo returned an invalid STL artifact.")
    return size
