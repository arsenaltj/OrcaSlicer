from __future__ import annotations

import hashlib
import json
import os
import time
from pathlib import Path
from typing import Any, Callable

from PIL import Image, ImageDraw, ImageOps, UnidentifiedImageError

try:
    from .openai_preprocessor import OpenAIPreprocessorError, complete_vision
    from .printable_model_views import ModelViewError, ModelViewSettings, render_model_views
except ImportError:
    from openai_preprocessor import OpenAIPreprocessorError, complete_vision
    from printable_model_views import ModelViewError, ModelViewSettings, render_model_views


REPORT_SCHEMA_VERSION = 2
REVIEW_VERSION = "visual-v9"
REPORT_FILENAME = "visual-quality.json"
CHECK_IDS = (
    "subject_complete",
    "semantic_coherence",
    "base_relationship",
    "detached_artifacts",
    "silhouette_readability",
    "color_region_clarity",
    "identity_likeness",
    "material_color_ownership",
)
CHECK_WARNING_CODES = {
    "subject_complete": "visual_subject_incomplete",
    "semantic_coherence": "visual_semantic_incoherence",
    "base_relationship": "visual_base_relationship",
    "detached_artifacts": "visual_detached_artifacts",
    "silhouette_readability": "visual_silhouette_unclear",
    "color_region_clarity": "visual_color_regions_unclear",
    "identity_likeness": "visual_identity_mismatch",
    "material_color_ownership": "visual_material_color_mixing",
}

# Provider labels are advisory, so enforce a small deterministic floor as well.
# Portrait identity and material ownership deliberately have the strictest floors:
# a pleasant generic face or a clean palette with skin on a sleeve is not a
# successful image-to-model result.
CHECK_MINIMUM_SCORES = {
    "subject_complete": 75,
    "semantic_coherence": 72,
    "base_relationship": 70,
    "detached_artifacts": 75,
    "silhouette_readability": 72,
    "color_region_clarity": 78,
    "identity_likeness": 82,
    "material_color_ownership": 88,
}
IMPORT_BLOCKING_CHECKS = {
    "subject_complete",
    "semantic_coherence",
    "detached_artifacts",
    "identity_likeness",
    "material_color_ownership",
}


class VisualQualityError(ValueError):
    pass


def _system_prompt(has_original_reference: bool, has_modeling_reference: bool) -> str:
    if has_original_reference and has_modeling_reference:
        comparison = (
            "The first image is a reference comparison sheet: its left panel is the original source and its right panel is the approved "
            "3D modeling reference. The second image is the final model contact sheet. Compare real-person identity against both reference "
            "panels. The approved modeling reference "
            "defines the intentional crop, silhouette, included body region and base: never mark arms, lower body, accessories or other "
            "content outside that approved framing as missing. "
        )
    elif has_original_reference:
        comparison = (
            "The first image is the original reference and the second image is the final model contact sheet. Compare identity, "
            "subject type, pose, visible signature features, and intended framing. "
        )
    elif has_modeling_reference:
        comparison = (
            "The first image is the approved 3D modeling reference and the second image is the final model contact sheet. Compare "
            "identity, subject type, silhouette and framing against that approved reference. "
        )
    else:
        comparison = "There is no reference image. Compare the contact sheet with the supplied user description only. "
    return (
        "You are a conservative visual reviewer for a vertex-colored 3D-printable collectible. "
        + comparison
        + "The contact sheet contains Front, Right, Back, Left, and Isometric views rendered from the final OBJ. Minor raster "
        "pinholes or flat software lighting are rendering artifacts and must not be treated as mesh defects. Do not judge exact "
        "wall thickness, watertightness, support requirements, or printer compatibility; a deterministic structural gate handles "
        "those. Assess only visible semantic quality. Return exactly one JSON object without markdown using this schema: "
        '{"summary":"Chinese sentence","score":0,"confidence":0.0,"checks":{'
        '"subject_complete":{"status":"pass|review","score":0,"reason":"Chinese sentence"},'
        '"semantic_coherence":{"status":"pass|review","score":0,"reason":"Chinese sentence"},'
        '"base_relationship":{"status":"pass|review","score":0,"reason":"Chinese sentence"},'
        '"detached_artifacts":{"status":"pass|review","score":0,"reason":"Chinese sentence"},'
        '"silhouette_readability":{"status":"pass|review","score":0,"reason":"Chinese sentence"},'
        '"color_region_clarity":{"status":"pass|review","score":0,"reason":"Chinese sentence"},'
        '"identity_likeness":{"status":"pass|review","score":0,"reason":"Chinese sentence"},'
        '"material_color_ownership":{"status":"pass|review","score":0,"reason":"Chinese sentence"}}}. '
        "Scores are integers from 0 to 100 and confidence is from 0 to 1. Use review when a visible issue needs a person to inspect; "
        "never output reject. A base is expected for people, animals, statues, characters, or unstable props, but stable manufactured "
        "objects do not require a pedestal. Multiple meaningful connected parts are allowed; only flag visibly accidental floating fragments. "
        "A thin plate, tab, spike, coloured rectangle, or second footprint protruding below or beside an otherwise clean display base is "
        "an accidental artifact even when connected to the base; do not dismiss it as antialiasing or contact shadow when it appears "
        "consistently in more than one rendered view. A person-shaped flat wall, silhouette plate, photo cutout, or broad planar sheet "
        "directly behind a portrait is also a severe accidental artifact: mark both semantic_coherence and detached_artifacts as review "
        "even when the front view looks correct and the sheet is technically connected to the body or pedestal. A valid portrait bust "
        "must have real rounded head, hair, shoulders and torso volume in the profile and back views. "
        "For identity_likeness, when the reference contains a real person compare the front and isometric model face with the reference: "
        "face width and length, eye spacing and shape, eyebrow arc, nose bridge/width/tip, mouth width and smile, cheek volume, jaw, chin, "
        "hairline, adult age, and natural asymmetry. A generic, doll-like, caricatured, or merely similar face must be review even if attractive. "
        "For a non-person subject, compare its category-specific signature geometry; with no reference, pass this check as not applicable. "
        "For material_color_ownership, flag any visible large or repeated color transfer between semantic parts, especially skin on sleeves, "
        "garment color on hands/face, hair color on skin, or pedestal color on the body. Compare the locations, not merely whether all expected "
        "colors exist: a skin-coloured stripe below crossed sleeves, a skin sliver on a jacket waist, or broad black camouflage-like patches "
        "inside an otherwise single-colour accent blouse are material mixing and must be review. A deliberate eye, eyebrow, mouth line, watch, "
        "hair region or base may remain dark. Do not flag tiny antialiasing pixels from the renderer."
    )


def _user_prompt(
    description: str,
    style: str,
    has_original_reference: bool,
    has_modeling_reference: bool,
) -> str:
    if has_original_reference and has_modeling_reference:
        source = "图生 3D；第一张参考对照板左侧是原图、右侧是已确认的实际建模参考，第二张是最终模型五视图"
    elif has_original_reference:
        source = "图生 3D，第一张图是原始参考"
    elif has_modeling_reference:
        source = "图生 3D，第一张图是已确认的实际建模参考"
    else:
        source = "文生 3D，无原始参考图"
    return (
        f"任务类型：{source}\n"
        f"目标风格：{style or '未记录'}\n"
        f"用户描述：{description.strip() or '未记录'}\n"
        "请检查最终模型五视图的主体完整性、语义一致性、底座关系、意外漂浮物、轮廓可读性、大色块清晰度、"
        "与原图的身份/标志特征相似度，以及肤色、衣物、头发和底座是否各自属于正确部位。"
    )


def _json_object(text: str) -> dict[str, Any]:
    stripped = text.strip()
    if stripped.startswith("```"):
        lines = stripped.splitlines()
        if len(lines) >= 3 and lines[-1].strip() == "```":
            stripped = "\n".join(lines[1:-1]).strip()
    try:
        value = json.loads(stripped)
    except json.JSONDecodeError:
        start, end = stripped.find("{"), stripped.rfind("}")
        if start < 0 or end <= start:
            raise VisualQualityError("The visual review response did not contain JSON.") from None
        try:
            value = json.loads(stripped[start : end + 1])
        except json.JSONDecodeError:
            raise VisualQualityError("The visual review response contained invalid JSON.") from None
    if not isinstance(value, dict):
        raise VisualQualityError("The visual review response was not an object.")
    return value


def _number(value: Any, minimum: float, maximum: float, default: float) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return default
    return max(minimum, min(maximum, float(value)))


def _normalize_review(
    raw: dict[str, Any],
    manifest: dict[str, Any],
    model: str,
    *,
    has_reference: bool,
    review_context_sha256: str,
) -> dict[str, Any]:
    raw_checks = raw.get("checks") if isinstance(raw.get("checks"), dict) else {}
    checks: dict[str, Any] = {}
    warnings: list[str] = []
    for check_id in CHECK_IDS:
        candidate = raw_checks.get(check_id) if isinstance(raw_checks.get(check_id), dict) else {}
        score = round(_number(candidate.get("score"), 0.0, 100.0, 50.0))
        minimum_score = CHECK_MINIMUM_SCORES[check_id]
        score_requires_review = score < minimum_score and not (
            check_id == "identity_likeness" and not has_reference
        )
        status = "review" if candidate.get("status") == "review" or score_requires_review else "pass"
        reason = str(candidate.get("reason", "")).strip()[:240]
        if status == "review":
            warnings.append(CHECK_WARNING_CODES[check_id])
        checks[check_id] = {"status": status, "score": score, "reason": reason}
    total_score = round(_number(raw.get("score"), 0.0, 100.0, sum(item["score"] for item in checks.values()) / len(checks)))
    blocking_warnings = [
        CHECK_WARNING_CODES[check_id]
        for check_id in CHECK_IDS
        if checks[check_id]["status"] == "review"
        and check_id in IMPORT_BLOCKING_CHECKS
        and not (check_id == "identity_likeness" and not has_reference)
    ]
    status = "review" if warnings or total_score < 80 else "pass"
    return {
        "schema_version": REPORT_SCHEMA_VERSION,
        "review_version": REVIEW_VERSION,
        "status": status,
        "score": total_score,
        "confidence": round(_number(raw.get("confidence"), 0.0, 1.0, 0.5), 3),
        "summary": str(raw.get("summary", "")).strip()[:500] or "视觉复核已完成。",
        "warnings": warnings,
        "blocking_warnings": blocking_warnings,
        "import_recommended": not blocking_warnings,
        "errors": [],
        "checks": checks,
        "obj_sha256": manifest["obj_sha256"],
        "render_version": manifest["render_version"],
        "views": list(manifest["views"]),
        "sheet": manifest["sheet"],
        "provider": "openai-compatible",
        "model": model,
        "review_context_sha256": review_context_sha256,
        "generated_at": time.time(),
        "cached": False,
    }


def _unavailable(message: str, manifest: dict[str, Any] | None = None) -> dict[str, Any]:
    return {
        "schema_version": REPORT_SCHEMA_VERSION,
        "review_version": REVIEW_VERSION,
        "status": "unavailable",
        "score": 0,
        "confidence": 0.0,
        "summary": message,
        "warnings": [],
        "blocking_warnings": [],
        "import_recommended": True,
        "errors": ["visual_review_unavailable"],
        "checks": {},
        "obj_sha256": manifest.get("obj_sha256", "") if manifest else "",
        "render_version": manifest.get("render_version", "") if manifest else "",
        "views": list(manifest.get("views", [])) if manifest else [],
        "sheet": manifest.get("sheet", "") if manifest else "",
        "provider": "openai-compatible",
        "model": os.environ.get("OPENAI_TEXT_MODEL", "gpt-5.4"),
        "generated_at": time.time(),
        "cached": False,
    }


def _write_report(report: dict[str, Any], destination: Path) -> None:
    temporary = destination.with_name(destination.name + ".part")
    try:
        temporary.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
        os.replace(temporary, destination)
    except OSError:
        try:
            temporary.unlink(missing_ok=True)
        except OSError:
            pass
        raise VisualQualityError("The visual quality report could not be written.") from None


def _file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as stream:
            while chunk := stream.read(1024 * 1024):
                digest.update(chunk)
    except OSError:
        return ""
    return digest.hexdigest()


def _compose_reference_sheet(
    original: Path,
    modeling_reference: Path,
    destination: Path,
) -> Path:
    """Combine both references so the vision endpoint still receives two images."""

    temporary = destination.with_name(destination.name + ".part")
    try:
        with Image.open(original) as opened_original:
            original_image = opened_original.convert("RGB")
        with Image.open(modeling_reference) as opened_modeling:
            modeling_image = opened_modeling.convert("RGB")
        panel_width, panel_height, label_height = 768, 768, 34
        sheet = Image.new(
            "RGB",
            (panel_width * 2, panel_height + label_height),
            (242, 244, 247),
        )
        drawing = ImageDraw.Draw(sheet)
        drawing.text((12, 10), "Original source", fill=(31, 41, 55))
        drawing.text((panel_width + 12, 10), "Approved 3D framing", fill=(31, 41, 55))
        for panel_index, source in enumerate((original_image, modeling_image)):
            fitted = ImageOps.contain(
                source,
                (panel_width - 24, panel_height - 24),
                Image.Resampling.LANCZOS,
            )
            left = panel_index * panel_width + (panel_width - fitted.width) // 2
            top = label_height + (panel_height - fitted.height) // 2
            sheet.paste(fitted, (left, top))
        destination.parent.mkdir(parents=True, exist_ok=True)
        sheet.save(temporary, format="PNG", optimize=True)
        os.replace(temporary, destination)
    except (OSError, ValueError, UnidentifiedImageError):
        try:
            temporary.unlink(missing_ok=True)
        except OSError:
            pass
        raise VisualQualityError("The visual review references could not be composed.") from None
    return destination


def review_model_visual_quality(
    obj_path: Path | str,
    job_directory: Path | str,
    *,
    description: str = "",
    style: str = "",
    reference_path: Path | str | None = None,
    modeling_reference_path: Path | str | None = None,
    force: bool = False,
    completion: Callable[[str, str, tuple[Path, ...]], str] = complete_vision,
) -> dict[str, Any]:
    root = Path(job_directory)
    report_path = root / REPORT_FILENAME
    manifest: dict[str, Any] | None = None
    try:
        # Do not sparsely sample a dense mesh for the delivery gate. Omitting
        # triangles leaves pinholes through which hair, skin, the inner blouse,
        # or the dark base become visible. Those renderer artifacts look exactly
        # like the material cross-colour this review is meant to detect. The
        # high-quality profile permits at most two million faces, so render the
        # full accepted mesh and trade a few minutes for a truthful inspection.
        manifest = render_model_views(
            obj_path,
            root,
            ModelViewSettings(max_render_faces=2_000_000),
            force=force,
        )
        model = os.environ.get("OPENAI_TEXT_MODEL", "gpt-5.4")
        reference = Path(reference_path) if reference_path else None
        modeling_reference = Path(modeling_reference_path) if modeling_reference_path else None
        has_original_reference = bool(reference and reference.is_file())
        has_modeling_reference = bool(modeling_reference and modeling_reference.is_file())
        has_reference = has_original_reference or has_modeling_reference
        reference_sha256 = (
            _file_sha256(reference)
            if has_original_reference and reference is not None else ""
        )
        modeling_reference_sha256 = (
            _file_sha256(modeling_reference)
            if has_modeling_reference and modeling_reference is not None else ""
        )
        review_context_sha256 = hashlib.sha256(json.dumps(
            {
                "description": description,
                "style": style,
                "reference_sha256": reference_sha256,
                "modeling_reference_sha256": modeling_reference_sha256,
            },
            ensure_ascii=False,
            sort_keys=True,
            separators=(",", ":"),
        ).encode("utf-8")).hexdigest()
        if not force and report_path.is_file():
            try:
                cached = json.loads(report_path.read_text(encoding="utf-8"))
                if (
                    isinstance(cached, dict)
                    and cached.get("status") in {"pass", "review"}
                    and cached.get("review_version") == REVIEW_VERSION
                    and cached.get("obj_sha256") == manifest["obj_sha256"]
                    and cached.get("render_version") == manifest["render_version"]
                    and cached.get("model") == model
                    and cached.get("review_context_sha256") == review_context_sha256
                ):
                    cached["cached"] = True
                    return cached
            except (OSError, UnicodeError, json.JSONDecodeError):
                pass
        sheet = root / str(manifest["sheet"])
        images: list[Path] = []
        if (
            has_original_reference
            and has_modeling_reference
            and reference is not None
            and modeling_reference is not None
        ):
            images.append(_compose_reference_sheet(
                reference,
                modeling_reference,
                root / "visual-reference-sheet.png",
            ))
        elif has_original_reference and reference is not None:
            images.append(reference)
        elif has_modeling_reference and modeling_reference is not None:
            images.append(modeling_reference)
        images.append(sheet)
        response = completion(
            _system_prompt(has_original_reference, has_modeling_reference),
            _user_prompt(
                description,
                style,
                has_original_reference,
                has_modeling_reference,
            ),
            tuple(images),
        )
        report = _normalize_review(
            _json_object(response), manifest, model,
            has_reference=has_reference,
            review_context_sha256=review_context_sha256,
        )
    except (ModelViewError, OpenAIPreprocessorError, VisualQualityError) as exc:
        report = _unavailable("视觉复核暂不可用，可稍后重试；模型和结构检查结果不受影响。", manifest)
        report["diagnostic"] = str(exc)[:300]
    _write_report(report, report_path)
    return report
