"""Build the reviewed, source-bound parent-color candidate used by Orca."""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import shutil
from pathlib import Path


def digest_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def digest_json(value) -> str:
    return digest_bytes(json.dumps(value, ensure_ascii=True, allow_nan=False,
                                   separators=(",", ":"), sort_keys=True).encode())


def load_plan(path: Path) -> dict:
    with path.open(encoding="utf-8") as stream:
        value = json.load(stream)
    if value.get("schema") != "orca.portrait-color-plan/v1":
        raise ValueError("unexpected color plan schema")
    return value


def frozen_boundary(partition: dict, locks: dict) -> str:
    fields = ("label", "subject_id", "parent_label", "status", "view_support",
              "reasons", "locked_cells", "nested_cells", "periocular_cells")
    records = [{key: lock.get(key, []) for key in fields} for lock in locks["locks"]]
    records.sort(key=lambda row: (row["subject_id"], row["label"]))
    cells = []
    for face in partition["faces"]:
        for cell in face["cells"]:
            label = cell["label"]
            if label not in ("lb", "rb", "le", "re", "ulip", "llip", "imouth",
                             "lip-line-corner", "teeth") and not label.startswith(("iris-", "periocular-")):
                continue
            fixed = lambda ring: [[int(math.floor(v * 1_000_000_000 + .5)) for v in p] for p in ring]
            cells.append(dict(id=cell["id"], source_face_id=face["source_face_id"],
                              label=label, parent_label=cell["parent_label"],
                              subject_id=cell["subject_id"], polygon=fixed(cell["polygon"]),
                              holes=[fixed(ring) for ring in cell["holes"]]))
    cells.sort(key=lambda row: row["id"])
    return digest_json(dict(geometry_id=partition["geometry_id"],
                            source_sha256=partition["source_sha256"],
                            face_count=partition["face_count"], locks=records, cells=cells))


def bind_parent_ownership(candidate: dict, partition: dict) -> None:
    """Bind reviewed ownership changes to the unchanged source cell labels."""
    cells = {cell["id"]: (face["source_face_id"], cell)
             for face in partition["faces"] for cell in face["cells"]}
    relabels = {}
    for key, _, target, face, implicit, subject in candidate["cells"]:
        if implicit:
            continue
        source = cells.get(key)
        if source is None or source[0] != face or source[1]["subject_id"] != subject:
            raise ValueError("reviewed ownership source cell differs")
        cell = source[1]
        if cell["label"] not in ("face", "R6", "skin", "hair", "cloth"):
            raise ValueError("reviewed parent ownership crosses protected detail")
        if cell["label"] != target:
            relabels[key] = dict(source_label=cell["label"],
                                source_parent_label=cell["parent_label"], target_label=target)
    candidate["ownership_relabels"] = relabels


def build_candidate(plan3: dict, plan6: dict, siblings: tuple[dict, ...] = ()) -> dict:
    identity = ("source_sha256", "geometry_id", "face_count", "boundary_sha256",
                "detail_freeze_sha256", "partition_ref", "shape_lock_sha256",
                "runtime_sha256", "policy_sha256", "shape_lock_fingerprint")
    for key in identity:
        if plan3.get(key) != plan6.get(key):
            raise ValueError(f"color plan identity differs: {key}")

    def parent_rows(plan: dict) -> dict[str, tuple]:
        result = {}
        for row in plan.get("cells", []):
            if row.get("color_source") != "PARENT_UNIFORM":
                continue
            cell_id, role, label = row.get("id"), row.get("slot_uid"), row.get("label")
            face, implicit = row.get("source_face_id"), row.get("implicit_root")
            if not isinstance(cell_id, str) or len(cell_id) != 64 or \
                    any(c not in "0123456789abcdef" for c in cell_id) or not isinstance(role, str) or \
                    type(face) is not int or not 0 <= face < plan["face_count"] or type(implicit) is not bool or \
                    not isinstance(row.get("subject_id"), str) or not row["subject_id"]:
                raise ValueError("invalid parent cell identity")
            if role not in ("portrait-skin", "portrait-light", "portrait-mid"):
                raise ValueError("parent cleanup can only use skin/cloth roles")
            if (role == "portrait-skin" and label != "skin") or \
                    (role in ("portrait-light", "portrait-mid") and label != "cloth"):
                raise ValueError("parent role does not match its semantic label")
            if cell_id in result:
                raise ValueError("duplicate parent cell")
            result[cell_id] = (role, label, face, implicit, row.get("subject_id"))
        return result

    base = parent_rows(plan3)
    for sibling in siblings:
        if any(sibling.get(key) != plan3.get(key) for key in identity):
            raise ValueError("sibling color plan identity differs")
        # The approved four-color baseline is expected to share the same
        # ownership map even though its target RGB values differ.
        if parent_rows(sibling) != base:
            raise ValueError("parent cell map differs across color counts")
    six = parent_rows(plan6)
    if not base.items() <= six.items():
        raise ValueError("six-color plan removed a reviewed parent cell")
    rows = [[cell_id, *values] for cell_id, values in sorted(six.items())]
    return {
        "schema": "orca.portrait-parent-cleanup-candidate/v1",
        "algorithm": "frozen-parent-cleanup/v1",
        "source_sha256": plan3["source_sha256"],
        "geometry_id": plan3["geometry_id"],
        "face_count": plan3["face_count"],
        "partition_sha256": plan3["boundary_sha256"],
        "shape_lock_fingerprint": plan3["shape_lock_fingerprint"],
        "detail_freeze_sha256": plan3["detail_freeze_sha256"],
        "runtime_sha256": plan3["runtime_sha256"],
        "policy_sha256": plan3["policy_sha256"],
        "roles": ["portrait-skin", "portrait-light", "portrait-mid"],
        "optional_roles": ["portrait-mid"],
        "cells": rows,
        "production_enabled": False,
        "material_tree_changed": False,
    }


def write_candidate(run: Path, output: Path) -> tuple[Path, dict]:
    color_root = run / "colors-r2" / "uniform"
    plans = {count: load_plan(color_root / f"colors-{count}" / "portrait-color-plan.json")
             for count in (3, 4, 5, 6)}
    # Build sibling identity checks without retaining large plan documents.
    base = plans[3]
    # The plan records the SHA of the shape-lock sidecar; the C++ loader uses
    # the derived lock fingerprint as its stronger runtime check.
    lock_path = run / "partition" / "shape-locks.json"
    lock_document = json.loads(lock_path.read_text(encoding="utf-8"))
    reference = lock_document["partition_ref"]
    if reference["path"] != "surface-partitions/" + reference["sha256"] + ".json":
        raise ValueError("unsafe partition reference")
    partition_path = run / "partition" / reference["path"]
    partition_document = json.loads(partition_path.read_text(encoding="utf-8"))
    if partition_document["partition_sha256"] != base["boundary_sha256"]:
        raise ValueError("partition boundary identity differs")
    boundary = dict(lock_document)
    boundary.pop("partition_ref", None)
    boundary["partition_sha256"] = base["boundary_sha256"]
    for plan in plans.values():
        plan["face_count"] = partition_document["face_count"]
        plan["shape_lock_fingerprint"] = digest_json(boundary)
    candidate = build_candidate(base, plans[6], tuple(plans[count] for count in (4, 5)))
    bind_parent_ownership(candidate, partition_document)
    output.mkdir(parents=True, exist_ok=True)
    if digest_bytes(partition_path.read_bytes()) != lock_document["partition_ref"]["sha256"]:
        raise ValueError("partition reference hash differs")
    partition_name = "portrait_parent_cleanup_partition.json"
    lock_name = "portrait_parent_cleanup_locks.json"
    shutil.copyfile(partition_path, output / partition_name)
    shutil.copyfile(lock_path, output / lock_name)
    candidate.update(partition_path=partition_name,
                     partition_file_sha256=digest_bytes(partition_path.read_bytes()),
                     lock_path=lock_name, lock_sha256=digest_bytes(lock_path.read_bytes()),
                     frozen_boundary_sha256=frozen_boundary(partition_document, lock_document),
                     boundary_runtime_sha256=partition_document["runtime_sha256"],
                     boundary_policy_sha256=partition_document["boundary_policy_sha256"])
    candidate_path = output / "portrait_parent_cleanup_fangfei.json"
    candidate_bytes = json.dumps(candidate, ensure_ascii=True, allow_nan=False,
                                 separators=(",", ":"), sort_keys=True).encode()
    candidate_path.write_bytes(candidate_bytes)
    catalog = {
        "schema": "orca.portrait-parent-cleanup-catalog/v1",
        "algorithm": "frozen-parent-cleanup/v1",
        "candidates": [{
            "source_sha256": candidate["source_sha256"],
            "geometry_id": candidate["geometry_id"],
            "face_count": candidate["face_count"],
            "partition_sha256": candidate["partition_sha256"],
            "shape_lock_fingerprint": candidate["shape_lock_fingerprint"],
            "detail_freeze_sha256": candidate["detail_freeze_sha256"],
            "runtime_sha256": candidate["runtime_sha256"],
            "policy_sha256": candidate["policy_sha256"],
            **{key: candidate[key] for key in ("partition_path", "partition_file_sha256",
                                              "lock_path", "lock_sha256", "frozen_boundary_sha256",
                                              "boundary_runtime_sha256", "boundary_policy_sha256")},
            "path": candidate_path.name,
            "sha256": digest_bytes(candidate_bytes),
        }],
    }
    (output / "portrait_parent_cleanup_catalog.json").write_bytes(
        json.dumps(catalog, ensure_ascii=True, allow_nan=False,
                   separators=(",", ":"), sort_keys=True).encode())
    return candidate_path, catalog


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--run", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    path, catalog = write_candidate(args.run.resolve(), args.output.resolve())
    print(json.dumps({"candidate": str(path), "sha256": digest_bytes(path.read_bytes()),
                      "cells": len(json.loads(path.read_text(encoding="utf-8"))["cells"]),
                      "catalog": catalog}, ensure_ascii=True, sort_keys=True))


if __name__ == "__main__":
    main()
