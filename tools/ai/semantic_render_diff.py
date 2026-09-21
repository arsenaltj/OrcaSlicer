"""Compare two fixed-camera semantic renders without changing their source data."""

from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import struct

from PIL import Image


VIEWS = ("front", "back", "left", "right", "top", "bottom")


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def compare(previous: Path, current: Path, view: str, stage: str, labels: str) -> dict:
    for suffix in ("original.ppm", "faces.u32"):
        if digest(previous / f"{view}-{suffix}") != digest(current / f"{view}-{suffix}"):
            raise ValueError(f"Different source camera or face IDs in {view}-{suffix}")
    with Image.open(previous / f"{view}-{stage}.ppm") as opened:
        before = opened.convert("RGB")
    with Image.open(current / f"{view}-{stage}.ppm") as opened:
        after = opened.convert("RGB")
    if before.size != after.size:
        raise ValueError(f"Different {stage} dimensions in {view}")
    face_ids = (previous / f"{view}-faces.u32").read_bytes()
    if len(face_ids) != before.width * before.height * 4:
        raise ValueError(f"Invalid face-ID buffer in {view}")
    transitions: Counter[tuple[tuple[int, ...], tuple[int, ...]]] = Counter()
    changed_faces: Counter[int] = Counter()
    changed_labels: Counter[str] = Counter()
    left, top = before.size
    right = bottom = -1
    for pixel, (old, new) in enumerate(zip(before.get_flattened_data(), after.get_flattened_data())):
        if old == new:
            continue
        transitions[old, new] += 1
        face_id = struct.unpack_from("<I", face_ids, pixel * 4)[0]
        if face_id < len(labels):
            changed_faces[face_id] += 1
            changed_labels[labels[face_id]] += 1
        x, y = pixel % before.width, pixel // before.width
        left, top, right, bottom = min(left, x), min(top, y), max(right, x), max(bottom, y)
    return {"view": view, "stage": stage, "size": list(before.size),
            "changed_pixels": sum(transitions.values()),
            "changed_box": [left, top, right + 1, bottom + 1] if transitions else None,
            "changed_label_pixels": changed_labels,
            "changed_faces": [{"face_id": face, "pixels": pixels, "label": labels[face]}
                              for face, pixels in changed_faces.most_common()],
            "transitions": [{"from_rgb": old, "to_rgb": new, "pixels": count}
                            for (old, new), count in transitions.most_common()]}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--previous", type=Path, required=True)
    parser.add_argument("--current", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    details = sorted(path.name.removesuffix("-candidate.ppm")
                     for path in args.previous.glob("*-4x-candidate.ppm")
                     if (args.current / path.name).is_file())
    analysis = json.loads((args.current / "analysis-candidate.json").read_text(encoding="utf-8"))
    labels = analysis["labels"]
    report = {"schema": "orca.semantic-render-diff/v1",
              "previous": str(args.previous.resolve()), "current": str(args.current.resolve()),
              "views": [compare(args.previous, args.current, view, stage, labels)
                        for view in (*VIEWS, *details) for stage in ("baseline", "candidate")]}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps({f'{row["view"]}-{row["stage"]}': row["changed_pixels"]
                      for row in report["views"]}))


if __name__ == "__main__":
    main()
