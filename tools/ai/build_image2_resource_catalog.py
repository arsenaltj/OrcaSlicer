"""Build a traceable resource catalog for one or more Image2 benchmark runs."""

from __future__ import annotations

import argparse
import csv
import json
import os
from collections import Counter
from pathlib import Path
from typing import Any, Iterable

from PIL import Image, ImageDraw


REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_OUTPUT = REPOSITORY_ROOT / "generated_models" / "image2-non-realistic-overnight-resources-v1"


class ResourceCatalogError(RuntimeError):
    pass


def _read_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise ResourceCatalogError(f"Cannot read JSON: {path}") from exc
    if not isinstance(value, dict):
        raise ResourceCatalogError(f"Expected a JSON object: {path}")
    return value


def _relative(path: Path | None) -> str:
    if path is None:
        return ""
    resolved = path.resolve()
    try:
        return resolved.relative_to(REPOSITORY_ROOT).as_posix()
    except ValueError:
        return str(resolved)


def _atomic_text(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".part")
    temporary.write_text(content, encoding="utf-8", newline="")
    os.replace(temporary, path)


def _atomic_json(path: Path, value: Any) -> None:
    _atomic_text(path, json.dumps(value, ensure_ascii=False, indent=2) + "\n")


def _write_csv(path: Path, rows: list[dict[str, Any]], fieldnames: list[str], delimiter: str = ",") -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".part")
    with temporary.open("w", encoding="utf-8-sig", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fieldnames, extrasaction="ignore", delimiter=delimiter)
        writer.writeheader()
        writer.writerows(rows)
    os.replace(temporary, path)


def _comparison_tile(path: Path | None, label: str, tile_size: int = 220) -> Image.Image:
    tile = Image.new("RGB", (tile_size, tile_size + 28), (238, 240, 243))
    draw = ImageDraw.Draw(tile)
    if path is not None and path.is_file():
        with Image.open(path) as opened:
            image = opened.convert("RGBA")
            image.thumbnail((tile_size - 16, tile_size - 16), Image.Resampling.LANCZOS)
        checker = Image.new("RGBA", image.size, (246, 246, 246, 255))
        checker_draw = ImageDraw.Draw(checker)
        for y in range(0, image.height, 14):
            for x in range(0, image.width, 14):
                if (x // 14 + y // 14) % 2:
                    checker_draw.rectangle(
                        (x, y, min(x + 13, image.width - 1), min(y + 13, image.height - 1)),
                        fill=(225, 225, 225, 255),
                    )
        checker.alpha_composite(image)
        tile.paste(checker.convert("RGB"), ((tile_size - image.width) // 2, (tile_size - image.height) // 2))
    else:
        draw.rectangle((12, 12, tile_size - 13, tile_size - 13), outline=(180, 184, 190), width=2)
        status = "TEXT INPUT" if label.startswith("TEXT INPUT") else "MISSING"
        left, top, right, bottom = draw.textbbox((0, 0), status)
        draw.text(((tile_size - right + left) // 2, (tile_size - bottom + top) // 2), status, fill=(110, 114, 120))
    draw.text((8, tile_size + 7), label[:38], fill="black")
    return tile


def write_fix_comparison_sheets(output: Path, catalog: dict[str, Any], cases_per_page: int = 6) -> int:
    """Compare relief and diorama model references across the first two runs."""

    run_names = [run.get("run") for run in catalog.get("runs", []) if isinstance(run, dict)]
    if len(run_names) < 2 or cases_per_page < 1:
        return 0
    before, after = run_names[:2]
    images = {
        (row.get("run"), row.get("case_id"), row.get("style")): row.get("model_reference", "")
        for row in catalog.get("image_resources", [])
        if isinstance(row, dict)
    }
    sources = {
        row.get("case_id"): row
        for row in catalog.get("source_resources", [])
        if isinstance(row, dict) and isinstance(row.get("case_id"), str)
    }
    shared_cases = sorted({
        case_id
        for run_name, case_id, style in images
        if run_name == after and style == "relief" and (before, case_id, "relief") in images
    })
    if not shared_cases:
        return 0
    headers = ["Source", f"{before} relief", f"{after} relief", f"{before} diorama", f"{after} diorama"]
    tile_size = 220
    header_height = 40
    destination = output / "comparison-sheets"
    destination.mkdir(parents=True, exist_ok=True)
    page_count = 0
    for start in range(0, len(shared_cases), cases_per_page):
        cases = shared_cases[start:start + cases_per_page]
        canvas = Image.new(
            "RGB",
            (len(headers) * tile_size, header_height + len(cases) * (tile_size + 28)),
            (232, 234, 238),
        )
        draw = ImageDraw.Draw(canvas)
        for column, header in enumerate(headers):
            draw.text((column * tile_size + 8, 14), header, fill="black")
        for row_index, case_id in enumerate(cases):
            source = sources.get(case_id, {})
            source_path = source.get("source") if isinstance(source, dict) else ""
            source_label = case_id if source_path else f"TEXT INPUT · {case_id}"
            cells = [
                (REPOSITORY_ROOT / source_path if source_path else None, source_label),
                (REPOSITORY_ROOT / images.get((before, case_id, "relief"), ""), "relief before"),
                (REPOSITORY_ROOT / images.get((after, case_id, "relief"), ""), "relief after"),
                (REPOSITORY_ROOT / images.get((before, case_id, "diorama"), ""), "diorama before"),
                (REPOSITORY_ROOT / images.get((after, case_id, "diorama"), ""), "diorama after"),
            ]
            for column, (path, label) in enumerate(cells):
                tile = _comparison_tile(path, label, tile_size)
                canvas.paste(tile, (column * tile_size, header_height + row_index * (tile_size + 28)))
        page_count += 1
        canvas.save(destination / f"page-{page_count:02d}.jpg", quality=92, optimize=True)
    return page_count


def _list_text(value: Any) -> str:
    return " | ".join(str(item) for item in value) if isinstance(value, list) else ""


def _resource_path(run_root: Path, row: dict[str, Any], kind: str) -> Path | None:
    candidate_id = row.get("candidate_id")
    if not isinstance(candidate_id, str) or row.get("status") != "complete":
        return None
    directory = run_root / "candidates" / candidate_id
    if kind == "raw":
        path = directory / "image2-output.png"
    else:
        printable = row.get("printable")
        name = printable.get("model_reference") if isinstance(printable, dict) else None
        if not isinstance(name, str):
            return None
        path = directory / name
    return path if path.is_file() else None


def build_catalog(runs: Iterable[tuple[str, Path]]) -> dict[str, Any]:
    run_summaries: list[dict[str, Any]] = []
    source_resources: dict[str, dict[str, Any]] = {}
    image_resources: list[dict[str, Any]] = []

    for run_name, unresolved_root in runs:
        run_root = unresolved_root.resolve()
        summary = _read_json(run_root / "benchmark-summary.json")
        rows = summary.get("rows")
        if not isinstance(rows, list):
            raise ResourceCatalogError(f"Summary rows are missing: {run_root}")
        case_order: list[str] = []
        for row in rows:
            if not isinstance(row, dict) or not isinstance(row.get("case_id"), str):
                continue
            if row["case_id"] not in case_order:
                case_order.append(row["case_id"])
        page_by_case = {case_id: index // 10 + 1 for index, case_id in enumerate(case_order)}

        run_summaries.append({
            "run": run_name,
            "root": _relative(run_root),
            "candidate_count": summary.get("candidate_count", 0),
            "paid_image2_calls": summary.get("paid_image2_calls", 0),
            "paid_tripo_calls": summary.get("paid_tripo_calls", 0),
            "statuses": summary.get("statuses", {}),
            "by_style": summary.get("by_style", {}),
            "quality": summary.get("quality", {}),
        })

        for value in rows:
            if not isinstance(value, dict):
                continue
            case_id = value.get("case_id")
            candidate_id = value.get("candidate_id")
            style = value.get("style")
            if not all(isinstance(item, str) for item in (case_id, candidate_id, style)):
                continue
            source_key = case_id
            source = value.get("source") if isinstance(value.get("source"), str) else ""
            source_resources.setdefault(source_key, {
                "case_id": case_id,
                "label": value.get("label", ""),
                "category": value.get("category", ""),
                "input_mode": value.get("input_mode", "image"),
                "instruction": value.get("instruction", ""),
                "source": _relative(REPOSITORY_ROOT / source) if source else "",
                "source_page": value.get("source_page", ""),
                "license": value.get("license", ""),
                "license_url": value.get("license_url", ""),
                "attribution": value.get("attribution", ""),
                "challenges": _list_text(value.get("challenges")),
                "preserve": _list_text(value.get("preserve")),
                "community_use": value.get("community_use", ""),
                "runs": [],
            })
            if run_name not in source_resources[source_key]["runs"]:
                source_resources[source_key]["runs"].append(run_name)

            quality = value.get("quality") if isinstance(value.get("quality"), dict) else {}
            raw_path = _resource_path(run_root, value, "raw")
            model_path = _resource_path(run_root, value, "model")
            contact_path = run_root / "contact-sheets" / "model-reference" / f"{case_id}__{style}.jpg"
            overview_path = run_root / "overview-sheets" / "primary" / f"page-{page_by_case[case_id]:02d}.jpg"
            image_resources.append({
                "run": run_name,
                "candidate_id": candidate_id,
                "case_id": case_id,
                "label": value.get("label", ""),
                "category": value.get("category", ""),
                "input_mode": value.get("input_mode", "image"),
                "style": style,
                "palette_id": value.get("palette_id", ""),
                "palette": _list_text(value.get("palette")),
                "status": value.get("status", ""),
                "error": value.get("error", ""),
                "paid_image2_calls": (value.get("paid_calls") or {}).get("image2", 0),
                "paid_tripo_calls": (value.get("paid_calls") or {}).get("tripo", 0),
                "provider_prompt_sha256": value.get("provider_prompt_sha256", ""),
                "quality_score": quality.get("score", ""),
                "model_input_eligible": quality.get("model_input_eligible", ""),
                "quality_flags": _list_text(quality.get("flags")),
                "raw_image": _relative(raw_path),
                "model_reference": _relative(model_path),
                "contact_sheet": _relative(contact_path) if contact_path.is_file() else "",
                "overview_sheet": _relative(overview_path) if overview_path.is_file() else "",
            })

    sources = list(source_resources.values())
    for resource in sources:
        resource["runs"] = " | ".join(resource["runs"])
    sources.sort(key=lambda item: item["case_id"])
    image_resources.sort(key=lambda item: (item["run"], item["case_id"], item["style"], item["candidate_id"]))
    return {
        "schema_version": 1,
        "runs": run_summaries,
        "source_resources": sources,
        "image_resources": image_resources,
        "totals": {
            "runs": len(run_summaries),
            "unique_cases": len(sources),
            "licensed_image_sources": sum(1 for item in sources if item["source_page"]),
            "text_cases": sum(1 for item in sources if item["input_mode"] == "text"),
            "image_candidates": len(image_resources),
            "complete_images": sum(1 for item in image_resources if item["status"] == "complete"),
            "failed_images": sum(1 for item in image_resources if item["status"] == "failed"),
            "paid_image2_calls": sum(int(item.get("paid_image2_calls", 0)) for item in run_summaries),
            "paid_tripo_calls": sum(int(item.get("paid_tripo_calls", 0)) for item in run_summaries),
            "styles": dict(Counter(item["style"] for item in image_resources)),
        },
    }


SOURCE_FIELDS = [
    "case_id", "label", "category", "input_mode", "instruction", "source", "source_page", "license",
    "license_url", "attribution", "challenges", "preserve", "community_use", "runs",
]
IMAGE_FIELDS = [
    "run", "candidate_id", "case_id", "label", "category", "input_mode", "style", "palette_id", "palette",
    "status", "error", "paid_image2_calls", "paid_tripo_calls", "provider_prompt_sha256", "quality_score",
    "model_input_eligible", "quality_flags", "raw_image", "model_reference", "contact_sheet", "overview_sheet",
]


def write_catalog(output: Path, catalog: dict[str, Any]) -> None:
    output.mkdir(parents=True, exist_ok=True)
    _atomic_json(output / "resource-catalog.json", catalog)
    _write_csv(output / "source-resources.csv", catalog["source_resources"], SOURCE_FIELDS)
    _write_csv(output / "image-resources.csv", catalog["image_resources"], IMAGE_FIELDS)
    _write_csv(output / "feishu-image-resources.tsv", catalog["image_resources"], IMAGE_FIELDS, delimiter="\t")
    totals = catalog["totals"]
    summary_rows = [
        {"项目": "运行批次", "数值": totals["runs"], "说明": "基线 v1 + 定向修正 v2"},
        {"项目": "覆盖风格", "数值": len(totals["styles"]), "说明": "单色雕塑、手办、低多边形、浮雕、微缩场景"},
        {"项目": "唯一用例", "数值": totals["unique_cases"], "说明": "跨人物、动物、建筑、机械、器物和场景"},
        {"项目": "许可图片来源", "数值": totals["licensed_image_sources"], "说明": "来源页、作者和许可证均在 source-resources.csv"},
        {"项目": "文本压力用例", "数值": totals["text_cases"], "说明": "食物、室内、户外、透明、深色、家具"},
        {"项目": "Image2 候选", "数值": totals["image_candidates"], "说明": f"完成 {totals['complete_images']}，失败 {totals['failed_images']}"},
        {"项目": "Image2 调用", "数值": totals["paid_image2_calls"], "说明": "候选级冻结状态，无隐式重试"},
        {"项目": "Tripo 调用", "数值": totals["paid_tripo_calls"], "说明": "本轮未调用任何 3D 付费服务"},
    ]
    _write_csv(output / "feishu-summary.tsv", summary_rows, ["项目", "数值", "说明"], delimiter="\t")
    comparison_pages = write_fix_comparison_sheets(output, catalog)
    lines = [
        "# 非写实 Image2 图片资源包",
        "",
        f"- 运行批次：{totals['runs']}",
        f"- 唯一用例：{totals['unique_cases']}（许可图片来源 {totals['licensed_image_sources']}，文本用例 {totals['text_cases']}）",
        f"- 候选图片：{totals['image_candidates']}（完成 {totals['complete_images']}，失败 {totals['failed_images']}）",
        f"- Image2 调用：{totals['paid_image2_calls']}；Tripo 调用：{totals['paid_tripo_calls']}",
        "",
        "文件说明：",
        "",
        "- `resource-catalog.json`：完整机器可读清单与分批统计。",
        "- `source-resources.csv`：来源、作者、许可证、用例和保留要素。",
        "- `image-resources.csv`：每个候选的提示词哈希、状态、评分、图片和联络表路径。",
        "- `feishu-image-resources.tsv`：可直接粘贴/导入在线表格的同内容制表符版本。",
        "- `feishu-summary.tsv`：在线表格首页使用的精简汇总。",
        f"- `comparison-sheets/`：浮雕/微缩修正前后对照，共 {comparison_pages} 页。",
        "",
    ]
    _atomic_text(output / "README.md", "\n".join(lines))


def _parse_run(value: str) -> tuple[str, Path]:
    name, separator, root = value.partition("=")
    if not separator or not name.strip() or not root.strip():
        raise argparse.ArgumentTypeError("Run must use NAME=PATH.")
    return name.strip(), Path(root.strip())


def main() -> int:
    parser = argparse.ArgumentParser(description="Build a traceable Image2 benchmark resource catalog.")
    parser.add_argument("--run", action="append", required=True, type=_parse_run)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()
    catalog = build_catalog(args.run)
    write_catalog(args.output.resolve(), catalog)
    print(json.dumps({"output": str(args.output.resolve()), **catalog["totals"]}, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
