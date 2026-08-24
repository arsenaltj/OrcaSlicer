from __future__ import annotations

import json
import os
import time
from pathlib import Path
from typing import Any, Callable

try:
    from .openai_preprocessor import OpenAIPreprocessorError, complete_vision
    from .printable_model_views import ModelViewError, render_model_views
except ImportError:
    from openai_preprocessor import OpenAIPreprocessorError, complete_vision
    from printable_model_views import ModelViewError, render_model_views


REPORT_SCHEMA_VERSION = 1
REVIEW_VERSION = "visual-v1"
REPORT_FILENAME = "visual-quality.json"
CHECK_IDS = (
    "subject_complete",
    "semantic_coherence",
    "base_relationship",
    "detached_artifacts",
    "silhouette_readability",
    "color_region_clarity",
)
CHECK_WARNING_CODES = {
    "subject_complete": "visual_subject_incomplete",
    "semantic_coherence": "visual_semantic_incoherence",
    "base_relationship": "visual_base_relationship",
    "detached_artifacts": "visual_detached_artifacts",
    "silhouette_readability": "visual_silhouette_unclear",
    "color_region_clarity": "visual_color_regions_unclear",
}


class VisualQualityError(ValueError):
    pass


def _system_prompt(has_reference: bool) -> str:
    comparison = (
        "The first image is the original reference and the second image is the final model contact sheet. Compare identity, "
        "subject type, pose, visible signature features, and intended framing. "
        if has_reference
        else "There is no original reference image. Compare the contact sheet with the supplied user description only. "
    )
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
        '"color_region_clarity":{"status":"pass|review","score":0,"reason":"Chinese sentence"}}}. '
        "Scores are integers from 0 to 100 and confidence is from 0 to 1. Use review when a visible issue needs a person to inspect; "
        "never output reject. A base is expected for people, animals, statues, characters, or unstable props, but stable manufactured "
        "objects do not require a pedestal. Multiple meaningful connected parts are allowed; only flag visibly accidental floating fragments."
    )


def _user_prompt(description: str, style: str, has_reference: bool) -> str:
    source = "图生 3D，第一张图是原始参考" if has_reference else "文生 3D，无原始参考图"
    return (
        f"任务类型：{source}\n"
        f"目标风格：{style or '未记录'}\n"
        f"用户描述：{description.strip() or '未记录'}\n"
        "请检查最终模型五视图的主体完整性、语义一致性、底座关系、意外漂浮物、轮廓可读性和大色块清晰度。"
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


def _normalize_review(raw: dict[str, Any], manifest: dict[str, Any], model: str) -> dict[str, Any]:
    raw_checks = raw.get("checks") if isinstance(raw.get("checks"), dict) else {}
    checks: dict[str, Any] = {}
    warnings: list[str] = []
    for check_id in CHECK_IDS:
        candidate = raw_checks.get(check_id) if isinstance(raw_checks.get(check_id), dict) else {}
        status = "review" if candidate.get("status") == "review" else "pass"
        score = round(_number(candidate.get("score"), 0.0, 100.0, 50.0))
        reason = str(candidate.get("reason", "")).strip()[:240]
        if status == "review":
            warnings.append(CHECK_WARNING_CODES[check_id])
        checks[check_id] = {"status": status, "score": score, "reason": reason}
    total_score = round(_number(raw.get("score"), 0.0, 100.0, sum(item["score"] for item in checks.values()) / len(checks)))
    status = "review" if warnings or total_score < 75 else "pass"
    return {
        "schema_version": REPORT_SCHEMA_VERSION,
        "review_version": REVIEW_VERSION,
        "status": status,
        "score": total_score,
        "confidence": round(_number(raw.get("confidence"), 0.0, 1.0, 0.5), 3),
        "summary": str(raw.get("summary", "")).strip()[:500] or "视觉复核已完成。",
        "warnings": warnings,
        "errors": [],
        "checks": checks,
        "obj_sha256": manifest["obj_sha256"],
        "render_version": manifest["render_version"],
        "views": list(manifest["views"]),
        "sheet": manifest["sheet"],
        "provider": "openai-compatible",
        "model": model,
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


def review_model_visual_quality(
    obj_path: Path | str,
    job_directory: Path | str,
    *,
    description: str = "",
    style: str = "",
    reference_path: Path | str | None = None,
    force: bool = False,
    completion: Callable[[str, str, tuple[Path, ...]], str] = complete_vision,
) -> dict[str, Any]:
    root = Path(job_directory)
    report_path = root / REPORT_FILENAME
    manifest: dict[str, Any] | None = None
    try:
        manifest = render_model_views(obj_path, root, force=force)
        model = os.environ.get("OPENAI_TEXT_MODEL", "gpt-5.4")
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
                ):
                    cached["cached"] = True
                    return cached
            except (OSError, UnicodeError, json.JSONDecodeError):
                pass
        sheet = root / str(manifest["sheet"])
        images: list[Path] = []
        reference = Path(reference_path) if reference_path else None
        has_reference = bool(reference and reference.is_file())
        if has_reference and reference is not None:
            images.append(reference)
        images.append(sheet)
        response = completion(_system_prompt(has_reference), _user_prompt(description, style, has_reference), tuple(images))
        report = _normalize_review(_json_object(response), manifest, model)
    except (ModelViewError, OpenAIPreprocessorError, VisualQualityError) as exc:
        report = _unavailable("视觉复核暂不可用，可稍后重试；模型和结构检查结果不受影响。", manifest)
        report["diagnostic"] = str(exc)[:300]
    _write_report(report, report_path)
    return report
