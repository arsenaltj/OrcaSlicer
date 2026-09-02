"""Prepare the frozen non-realistic Image2 overnight benchmark manifest."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
from typing import Any, Iterable


REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_SOURCE_MANIFEST = REPOSITORY_ROOT / "generated_models" / "image2-community-500-v1" / "benchmark-manifest.json"
DEFAULT_OUTPUT = REPOSITORY_ROOT / "generated_models" / "image2-non-realistic-overnight-v1" / "benchmark-manifest.json"

DEFAULT_CASE_IDS = (
    "toy_wooden_mannequin",
    "cat_tabby",
    "bird_peacock",
    "vehicle_classic_car",
    "architecture_castle",
    "kitchen_teapot",
    "plant_bonsai",
    "thin_acoustic_guitar",
    "mechanism_industrial_robot",
    "landmark_pagoda",
    "animal_octopus",
    "dog_husky",
    "product_vintage_camera",
    "tool_cordless_drill",
    "wearable_sneaker",
    "sculpture_lion",
    "portrait_einstein",
    "pose_chaplin_cane",
    "vehicle_road_bicycle",
    "thin_umbrella",
)

TEXT_CASES: tuple[dict[str, Any], ...] = (
    {
        "id": "text_food_breakfast_tray",
        "input_mode": "text",
        "instruction": "A complete breakfast tray with a croissant, a cup, three berries and a folded napkin, all visible and count-preserved.",
        "label": "早餐托盘",
        "category": "food_scene",
        "challenges": ["multiple_objects", "thin_parts", "small_details"],
        "preserve": ["one tray", "one croissant", "one cup", "three berries", "one folded napkin"],
        "community_use": "食物摆件与桌面微缩",
    },
    {
        "id": "text_interior_corner_cafe",
        "input_mode": "text",
        "instruction": "A compact corner cafe scene with one counter, two stools, one hanging lamp and one potted plant, all physically connected to one floor base.",
        "label": "咖啡店角落",
        "category": "interior_scene",
        "challenges": ["occlusion", "thin_parts", "multiple_objects"],
        "preserve": ["one counter", "two stools", "one lamp", "one plant", "one shared floor"],
        "community_use": "室内微缩与建筑模型",
    },
    {
        "id": "text_outdoor_campsite",
        "input_mode": "text",
        "instruction": "A small mountain campsite with one tent, one pine tree, a ring of six stones and one backpack on a continuous terrain base.",
        "label": "山地露营地",
        "category": "outdoor_scene",
        "challenges": ["foliage", "multiple_objects", "terrain"],
        "preserve": ["one tent", "one pine tree", "six stones", "one backpack", "one shared base"],
        "community_use": "户外场景与地形微缩",
    },
    {
        "id": "text_translucent_jellyfish_lamp",
        "input_mode": "text",
        "instruction": "A jellyfish-shaped table lamp with one domed shade, eight sturdy curved tentacles and a single round base; convert transparency into solid printable layers.",
        "label": "水母台灯",
        "category": "transparent_product",
        "challenges": ["transparency", "thin_parts", "repeated_elements"],
        "preserve": ["one dome", "eight tentacles", "one round base"],
        "community_use": "透明材质替代与创意灯具",
    },
    {
        "id": "text_dark_chess_set",
        "input_mode": "text",
        "instruction": "A matte-black travel chess set with exactly six representative pieces arranged on a small folding board; keep every silhouette readable without relying on reflections.",
        "label": "深色旅行棋组",
        "category": "dark_product",
        "challenges": ["dark_subject", "repeated_elements", "silhouette"],
        "preserve": ["six pieces", "one folding board", "distinct piece silhouettes"],
        "community_use": "深色产品与重复部件",
    },
    {
        "id": "text_furniture_reading_corner",
        "input_mode": "text",
        "instruction": "A reading corner containing one armchair, one side table, one floor lamp and a stack of three books, all on one low rectangular base.",
        "label": "阅读角家具组",
        "category": "furniture_scene",
        "challenges": ["thin_parts", "negative_space", "multiple_objects"],
        "preserve": ["one chair", "one table", "one lamp", "three books", "one shared base"],
        "community_use": "家具与室内布置微缩",
    },
)


class NightlyManifestError(RuntimeError):
    pass


def _read_manifest(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise NightlyManifestError(f"Cannot read source manifest: {path}") from exc
    if not isinstance(value, dict) or not isinstance(value.get("cases"), list):
        raise NightlyManifestError("The source manifest must contain a cases array.")
    return value


def build_manifest(source: dict[str, Any], case_ids: Iterable[str] = DEFAULT_CASE_IDS) -> dict[str, Any]:
    requested = tuple(case_ids)
    if not requested or len(requested) != len(set(requested)):
        raise NightlyManifestError("Case IDs must be a non-empty unique sequence.")
    indexed = {
        case.get("id"): case
        for case in source["cases"]
        if isinstance(case, dict) and isinstance(case.get("id"), str)
    }
    missing = [case_id for case_id in requested if case_id not in indexed]
    if missing:
        raise NightlyManifestError("Missing source cases: " + ", ".join(missing))
    palettes = source.get("palettes")
    if not isinstance(palettes, dict) or "warm" not in palettes:
        raise NightlyManifestError("The source manifest must define the warm printable palette.")
    cases = [dict(indexed[case_id]) for case_id in requested]
    cases.extend(dict(case) for case in TEXT_CASES)
    return {
        "schema_version": 1,
        "description": (
            "Frozen ten-hour Image2 benchmark for non-realistic printable styles. "
            "The realistic style is deliberately excluded."
        ),
        "palettes": {"warm": palettes["warm"]},
        "style_runs": {
            "sculpture": [{"palette": None, "repetitions": 1}],
            "cartoon": [{"palette": "warm", "repetitions": 1}],
            "low_poly": [{"palette": "warm", "repetitions": 1}],
            "relief": [{"palette": "warm", "repetitions": 1}],
            "diorama": [{"palette": "warm", "repetitions": 1}],
        },
        "cases": cases,
    }


def write_manifest(path: Path, value: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".part")
    temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    os.replace(temporary, path)


def main() -> int:
    parser = argparse.ArgumentParser(description="Prepare the non-realistic overnight Image2 benchmark.")
    parser.add_argument("--source-manifest", type=Path, default=DEFAULT_SOURCE_MANIFEST)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--case", action="append", default=[])
    args = parser.parse_args()
    source = _read_manifest(args.source_manifest.resolve())
    manifest = build_manifest(source, args.case or DEFAULT_CASE_IDS)
    write_manifest(args.output.resolve(), manifest)
    print(json.dumps({
        "output": str(args.output.resolve()),
        "case_count": len(manifest["cases"]),
        "candidate_count": len(manifest["cases"]) * len(manifest["style_runs"]),
        "styles": list(manifest["style_runs"]),
    }, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
