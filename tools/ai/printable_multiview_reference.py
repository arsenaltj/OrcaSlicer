from __future__ import annotations

from dataclasses import asdict
import json
from pathlib import Path
from typing import Any, Callable, Mapping

from PIL import Image

try:
    from .printable_image_pipeline import PrintSettings, process_printable_image
    from .printable_palette import normalize_palette
except ImportError:
    from printable_image_pipeline import PrintSettings, process_printable_image
    from printable_palette import normalize_palette


SCHEMA_VERSION = 1
VIEW_ORDER = ("front", "left", "back", "right")
VIEW_POSITIONS = {
    "front": "top-left",
    "left": "top-right",
    "back": "bottom-left",
    "right": "bottom-right",
}
CHECK_IDS = ("identity", "view_order", "geometry", "palette", "completeness")


class MultiviewReferenceError(RuntimeError):
    pass


def build_multiview_sheet_prompt(description: str, palette: tuple[str, ...]) -> str:
    colors = normalize_palette(palette)
    if len(colors) != 4:
        raise MultiviewReferenceError("A multiview sheet requires exactly four printable colors.")
    return (
        "Create one precise 2-by-2 orthographic turntable reference sheet of the exact supplied finished collectible. "
        "First infer one single coherent solid 3D sculpture, then render that exact same unchanged sculpture four times; do not draw "
        "four independent interpretations. Do not redesign, simplify, add, remove, recolor, mirror, or change any part. Preserve "
        "identical identity, dimensions, proportions, pose, base, accessories, attachment points, material boundaries, and silhouette "
        "in every panel. Occluded parts must remain fixed in object space instead of moving to stay visible. The subject specification is: "
        + description.strip()
        + "\nPanel order is mandatory: top-left FRONT, top-right LEFT SIDE, bottom-left BACK, bottom-right RIGHT SIDE. "
        "Use exact camera yaw angles 0, +90, 180, and -90 degrees with zero pitch and zero roll. Every panel must show the complete "
        "subject at the same scale, centered, upright, level, and viewed with a true orthographic camera. Left and right describe the "
        "subject's own left and right sides. All protrusions must keep exactly the same depth, thickness, attachment and elevation in "
        "front, side and back views. Held tools must remain rigidly fused to the same hand with the same grip and orientation. Use the "
        "same transparent or plain neutral background "
        "and flat lighting in all four panels. Use only these four solid material colors: "
        + ", ".join(colors)
        + ". Do not add labels, text, arrows, borders, dividers, shadows, scenery, duplicate subjects, extra views, perspective drama, "
        "or cut-off geometry. Output exactly one 2-by-2 sheet."
    )


def split_multiview_sheet(sheet_path: Path | str, output_directory: Path | str) -> dict[str, Path]:
    source = Path(sheet_path)
    output = Path(output_directory)
    output.mkdir(parents=True, exist_ok=True)
    try:
        with Image.open(source) as opened:
            image = opened.convert("RGBA")
    except (OSError, ValueError):
        raise MultiviewReferenceError("The multiview sheet could not be read.") from None
    if image.width < 512 or image.height < 512:
        raise MultiviewReferenceError("The multiview sheet must be at least 512 by 512 pixels.")
    middle_x, middle_y = image.width // 2, image.height // 2
    boxes = {
        "front": (0, 0, middle_x, middle_y),
        "left": (middle_x, 0, image.width, middle_y),
        "back": (0, middle_y, middle_x, image.height),
        "right": (middle_x, middle_y, image.width, image.height),
    }
    result: dict[str, Path] = {}
    for view in VIEW_ORDER:
        destination = output / f"{view}-raw.png"
        image.crop(boxes[view]).save(destination)
        result[view] = destination
    return result


def process_multiview_crops(
    crops: Mapping[str, Path],
    output_directory: Path | str,
    palette: tuple[str, ...],
    settings: PrintSettings | None = None,
) -> tuple[dict[str, Path], dict[str, Any]]:
    if set(crops) != set(VIEW_ORDER):
        raise MultiviewReferenceError("All four named multiview crops are required.")
    output = Path(output_directory)
    references: dict[str, Path] = {}
    metrics: dict[str, Any] = {}
    for view in VIEW_ORDER:
        result = process_printable_image(crops[view], output / view, palette, settings or PrintSettings())
        references[view] = result.model_reference
        metrics[view] = result.metrics
    return references, metrics


def _review_system_prompt() -> str:
    return (
        "You review a source reference plus a 2-by-2 multiview reference sheet before paid 3D generation. If two images are "
        "provided, the first is the original source and the second is the sheet. The required sheet order is top-left front, "
        "top-right subject-left, bottom-left back, bottom-right subject-right. Judge whether all panels depict the same object with "
        "the same identity, proportions, pose, base, accessories, geometry and four material colors. A back view may reveal unseen "
        "surfaces but must not invent large structures. Orthographic FRONT in the sheet intentionally supersedes any three-quarter "
        "view wording in the source description; never flag that intended change. If the two side panels are clearly opposite views "
        "and geometrically consistent, do not flag them merely because subject-left versus camera-left naming is visually ambiguous. "
        "Different surfaces may legitimately use different proportions of the same four colors; flag palette only for actual color "
        "substitution or changed material boundaries, not coverage ratios. Return exactly one JSON object without markdown: "
        '{"summary":"Chinese sentence","score":0,"confidence":0.0,"checks":{'
        '"identity":{"status":"pass|review","reason":"Chinese sentence"},'
        '"view_order":{"status":"pass|review","reason":"Chinese sentence"},'
        '"geometry":{"status":"pass|review","reason":"Chinese sentence"},'
        '"palette":{"status":"pass|review","reason":"Chinese sentence"},'
        '"completeness":{"status":"pass|review","reason":"Chinese sentence"}}}. '
        "Use review for any duplicated/missing view, orientation uncertainty, identity drift, changed accessory, changed base, "
        "large invented back geometry, crop, or material-color mismatch. Scores are integers 0 to 100."
    )


def _parse_review(response: str) -> dict[str, Any]:
    stripped = response.strip()
    if stripped.startswith("```"):
        lines = stripped.splitlines()
        if len(lines) >= 3 and lines[-1].strip() == "```":
            stripped = "\n".join(lines[1:-1]).strip()
    try:
        raw = json.loads(stripped)
    except json.JSONDecodeError:
        raise MultiviewReferenceError("The multiview review returned invalid JSON.") from None
    if not isinstance(raw, dict):
        raise MultiviewReferenceError("The multiview review did not return an object.")
    checks: dict[str, Any] = {}
    warnings: list[str] = []
    source_checks = raw.get("checks") if isinstance(raw.get("checks"), dict) else {}
    for check_id in CHECK_IDS:
        candidate = source_checks.get(check_id) if isinstance(source_checks.get(check_id), dict) else {}
        status = "review" if candidate.get("status") != "pass" else "pass"
        if status == "review":
            warnings.append(check_id)
        checks[check_id] = {"status": status, "reason": str(candidate.get("reason", ""))[:300]}
    score_value = raw.get("score", 0)
    score = int(max(0, min(100, score_value))) if isinstance(score_value, (int, float)) and not isinstance(score_value, bool) else 0
    confidence_value = raw.get("confidence", 0.0)
    confidence = float(max(0.0, min(1.0, confidence_value))) if isinstance(confidence_value, (int, float)) else 0.0
    return {
        "status": "review" if warnings or score < 80 else "pass",
        "score": score,
        "confidence": round(confidence, 3),
        "summary": str(raw.get("summary", ""))[:500],
        "warnings": warnings,
        "checks": checks,
    }


def review_multiview_sheet(
    sheet_path: Path | str,
    description: str,
    *,
    source_path: Path | str | None = None,
    completion: Callable[[str, str, tuple[Path, ...]], str],
) -> dict[str, Any]:
    images = (Path(sheet_path),) if source_path is None else (Path(source_path), Path(sheet_path))
    response = completion(
        _review_system_prompt(),
        "对象描述：" + description.strip() + "\n请核对四个面板是否可安全用于同一模型的多视图建模。",
        images,
    )
    return _parse_review(response)


def write_multiview_manifest(
    output_directory: Path | str,
    *,
    sheet: Path,
    references: Mapping[str, Path],
    metrics: Mapping[str, Any],
    review: Mapping[str, Any],
    palette: tuple[str, ...],
    settings: PrintSettings,
) -> Path:
    destination = Path(output_directory) / "multiview-reference.json"
    value = {
        "schema_version": SCHEMA_VERSION,
        "sheet": str(sheet.resolve()),
        "layout": VIEW_POSITIONS,
        "views": {view: str(references[view].resolve()) for view in VIEW_ORDER},
        "palette": list(normalize_palette(palette)),
        "print_settings": asdict(settings),
        "metrics": metrics,
        "review": dict(review),
    }
    temporary = destination.with_name(destination.name + ".part")
    temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2), encoding="utf-8")
    temporary.replace(destination)
    return destination


def create_multiview_sheet(
    source_path: Path | str,
    output_path: Path | str,
    description: str,
    palette: tuple[str, ...],
    *,
    editor: Callable[[Path | str, str, Path | str], Path],
) -> Path:
    return editor(source_path, build_multiview_sheet_prompt(description, palette), output_path)
