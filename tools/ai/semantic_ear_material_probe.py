"""Trace a material-color patch in a validation render back to OBJ faces."""

from __future__ import annotations

import argparse
from collections import Counter, defaultdict
import json
from pathlib import Path
import struct

from PIL import Image


def obj_faces(path: Path):
    with path.open(encoding="ascii") as source:
        for line in source:
            if line.startswith("f "):
                yield tuple(int(part.split("/")[0]) - 1 for part in line.split()[1:4])


def lab(rgb):
    r, g, b = (c / 12.92 if c <= .04045 else ((c + .055) / 1.055) ** 2.4 for c in rgb)
    l = (.4122214708*r + .5363325363*g + .0514459929*b) ** (1/3)
    m = (.2119034982*r + .6806995451*g + .1073969566*b) ** (1/3)
    s = (.0883024619*r + .2817188376*g + .6299787005*b) ** (1/3)
    return (.2104542553*l + .793617785*m - .0040720468*s,
            1.9779984951*l - 2.428592205*m + .4505937099*s,
            .0259040371*l + .7827717662*m - .8086757662*s)


def appearance_distance(a, b):
    return ((a[0] - b[0]) * .25) ** 2 + (a[1] - b[1]) ** 2 + (a[2] - b[2]) ** 2


def confidence_at(encoded: str, face_id: int) -> float:
    bits = int(encoded[face_id * 8:face_id * 8 + 8][::-1], 16)
    return struct.unpack("<f", struct.pack("<I", bits))[0]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model-dir", type=Path, required=True)
    parser.add_argument("--obj", type=Path, required=True)
    parser.add_argument("--view", default="front-skin-boundaries-4x")
    parser.add_argument("--box", nargs=4, type=int, required=True, metavar=("L", "T", "R", "B"))
    parser.add_argument("--rgb", nargs=3, type=int, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    analysis = json.loads((args.model_dir / "analysis-candidate.json").read_text(encoding="utf-8"))
    with Image.open(args.model_dir / f"{args.view}-candidate.ppm") as opened:
        image = opened.convert("RGB")
    width, height = image.size
    left, top, right, bottom = args.box
    if not (0 <= left < right <= width and 0 <= top < bottom <= height):
        parser.error("Invalid image box")
    face_bytes = (args.model_dir / f"{args.view}-faces.u32").read_bytes()
    if len(face_bytes) != width * height * 4:
        parser.error("The face-id buffer does not match the rendered image")
    pixels = image.load()
    patch = Counter()
    for y in range(top, bottom):
        for x in range(left, right):
            if pixels[x, y] == tuple(args.rgb):
                face_id = struct.unpack_from("<I", face_bytes, (y * width + x) * 4)[0]
                if face_id < analysis["face_count"]:
                    patch[face_id] += 1
    if not patch:
        parser.error("No matching material pixels in the box")

    # Two streaming passes keep memory bounded on million-face OBJ fixtures.
    targets = {}
    count = 0
    for face_id, vertices in enumerate(obj_faces(args.obj)):
        if face_id in patch:
            targets[face_id] = vertices
        count = face_id + 1
    if count != analysis["face_count"]:
        parser.error(f"OBJ face order/count differs from analysis: {count}")
    by_vertex = defaultdict(set)
    for face_id, vertices in targets.items():
        for vertex in vertices:
            by_vertex[vertex].add(face_id)
    neighbors = defaultdict(Counter)
    edge_neighbors = defaultdict(Counter)
    nearby_vertices = {}
    for face_id, vertices in enumerate(obj_faces(args.obj)):
        attached = Counter(target for vertex in vertices for target in by_vertex.get(vertex, ()))
        if attached:
            nearby_vertices[face_id] = vertices
        for target, common in attached.items():
            if target != face_id:
                neighbors[target][face_id] = common
                if common >= 2:
                    edge_neighbors[target][face_id] = common

    selected_vertices = {vertex for face in nearby_vertices.values() for vertex in face}
    vertex_colors = {}
    with args.obj.open(encoding="ascii") as source:
        vertex_id = 0
        for line in source:
            if line.startswith("v "):
                if vertex_id in selected_vertices:
                    vertex_colors[vertex_id] = tuple(float(channel) for channel in line.split()[4:7])
                vertex_id += 1
    if len(vertex_colors) != len(selected_vertices):
        parser.error("OBJ vertices are missing color values")
    face_lab = {}
    for face_id, vertices in nearby_vertices.items():
        face_lab[face_id] = lab(tuple(sum(vertex_colors[vertex][channel] for vertex in vertices) / 3
                                  for channel in range(3)))

    labels = analysis["labels"]
    rows = []
    for face_id, pixels_count in patch.most_common():
        adjacent = Counter(labels[neighbor] for neighbor in neighbors[face_id])
        edges = Counter(labels[neighbor] for neighbor in edge_neighbors[face_id])
        reliable_skin = [neighbor for neighbor in edge_neighbors[face_id]
                         if labels[neighbor] in ("3", "4") and
                         confidence_at(analysis["confidence_f32"], neighbor) >= .70]
        rows.append({"face_id": face_id, "pixels": pixels_count,
                     "label": labels[face_id], "baseline_label": analysis["baseline_labels"][face_id],
                     "confidence": confidence_at(analysis["confidence_f32"], face_id),
                     "vertex_neighbor_labels": dict(adjacent), "edge_neighbor_labels": dict(edges),
                     "source_oklab": face_lab[face_id],
                     "reliable_skin_edge_neighbors": reliable_skin,
                     "nearest_skin_appearance_distance": min(
                         (appearance_distance(face_lab[face_id], face_lab[neighbor]) for neighbor in reliable_skin),
                         default=None)})
    report = {"schema": "orca.semantic-ear-material-probe/v1", "model": args.model_dir.name,
              "source": str(args.obj.resolve()), "view": args.view, "box": args.box, "rgb": args.rgb,
              "matching_pixels": sum(patch.values()), "face_count": len(patch), "faces": rows}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps({"matching_pixels": report["matching_pixels"], "faces": len(rows),
                      "labels": dict(Counter(labels[face_id] for face_id in patch)),
                      "skin_edge_faces": sum(bool(row["reliable_skin_edge_neighbors"]) for row in rows),
                      "report": str(args.output.resolve())}))


if __name__ == "__main__":
    main()
