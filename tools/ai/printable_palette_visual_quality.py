"""Advisory GPT vision review for printable four-color reference images."""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import time
from typing import Any, Callable, Mapping

try:
    from .openai_preprocessor import OpenAIPreprocessorError, complete_vision
except ImportError:
    from openai_preprocessor import OpenAIPreprocessorError, complete_vision


REPORT_SCHEMA_VERSION = 1
REVIEW_VERSION = "palette-visual-v1"
REPORT_FILENAME = "palette-visual-quality.json"
CHECK_IDS = (
    "semantic_palette_fit",
    "role_usage",
    "subject_fidelity",
    "silhouette_readability",
    "large_color_regions",
    "modeling_reference",
)
CHECK_WARNING_CODES = {
    "semantic_palette_fit": "palette_semantic_fit_unclear",
    "role_usage": "palette_role_usage_unclear",
    "subject_fidelity": "palette_subject_fidelity_unclear",
    "silhouette_readability": "palette_silhouette_unclear",
    "large_color_regions": "palette_color_regions_fragmented",
    "modeling_reference": "palette_modeling_reference_unclear",
}


class PaletteVisualQualityError(ValueError):
    pass


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(chunk)
    except OSError as exc:
        raise PaletteVisualQualityError(f"Cannot read review image: {path}") from exc
    return digest.hexdigest()


def _system_prompt(has_reference: bool) -> str:
    comparison = (
        "The first image is the user reference and the second is the strict four-color preview. "
        if has_reference
        else "The supplied image is the strict four-color preview for a text-described subject. "
    )
    checks = ",".join(
        f'"{check}":{{"status":"pass|review","score":0,"reason":"Chinese sentence"}}'
        for check in CHECK_IDS
    )
    return (
        "You are a conservative design reviewer for a four-filament 3D-printable collectible. "
        + comparison
        + "Judge semantic suitability and visible reference-image quality only. Do not claim exact wall thickness, topology, "
        "support requirements, printer compatibility, or physical filament color matching. Broad solid material regions are good; "
        "gradients, dithering, tiny isolated accents, incomplete silhouettes, invented clutter, and colors used only as lighting need review. "
        "Return exactly one JSON object without markdown using this schema: "
        + '{"summary":"Chinese sentence","score":0,"confidence":0.0,"checks":{'
        + checks
        + "}}. Scores are integers from 0 to 100, confidence is from 0 to 1, and every check is required. "
        "Never output reject."
    )


def _user_prompt(prompt: str, style: str, recommendation: Mapping[str, Any]) -> str:
    colors = recommendation.get("colors")
    palette_lines: list[str] = []
    if isinstance(colors, list):
        for item in colors:
            if isinstance(item, dict):
                palette_lines.append(
                    f"{item.get('role', '')}={item.get('hex', '')} {item.get('name', '')}; "
                    f"用途={item.get('usage', '')}; 原因={item.get('reason', '')}"
                )
    return (
        f"用户描述：{prompt.strip() or '未记录'}\n"
        f"目标风格：{style or '未记录'}\n"
        f"推荐摘要：{str(recommendation.get('summary', '')).strip()}\n"
        "推荐四色：\n- " + "\n- ".join(palette_lines) + "\n"
        "请检查配色语义、角色使用、主体忠实度、轮廓、大色块和作为后续图生 3D 参考的适合程度。"
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
            raise PaletteVisualQualityError("The palette visual review did not contain JSON.") from None
        try:
            value = json.loads(stripped[start : end + 1])
        except json.JSONDecodeError:
            raise PaletteVisualQualityError("The palette visual review contained invalid JSON.") from None
    if not isinstance(value, dict):
        raise PaletteVisualQualityError("The palette visual review must be a JSON object.")
    return value


def _number(value: Any, minimum: float, maximum: float, field: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise PaletteVisualQualityError(f"The palette visual review {field} is invalid.")
    return max(minimum, min(maximum, float(value)))


def _text(value: Any, field: str, maximum: int) -> str:
    if not isinstance(value, str) or not value.strip():
        raise PaletteVisualQualityError(f"The palette visual review {field} is required.")
    return value.strip()[:maximum]


def _normalize_review(
    raw: dict[str, Any], strict_sha: str, reference_sha: str, model: str
) -> dict[str, Any]:
    raw_checks = raw.get("checks")
    if not isinstance(raw_checks, dict):
        raise PaletteVisualQualityError("The palette visual review checks are required.")
    checks: dict[str, Any] = {}
    warnings: list[str] = []
    for check_id in CHECK_IDS:
        candidate = raw_checks.get(check_id)
        if not isinstance(candidate, dict):
            raise PaletteVisualQualityError(f"The palette visual review check is missing: {check_id}")
        status = candidate.get("status")
        if status not in {"pass", "review"}:
            raise PaletteVisualQualityError(f"The palette visual review check status is invalid: {check_id}")
        score = round(_number(candidate.get("score"), 0.0, 100.0, f"{check_id} score"))
        reason = _text(candidate.get("reason"), f"{check_id} reason", 240)
        if status == "review":
            warnings.append(CHECK_WARNING_CODES[check_id])
        checks[check_id] = {"status": status, "score": score, "reason": reason}
    total_score = round(_number(raw.get("score"), 0.0, 100.0, "score"))
    if total_score < 80:
        warnings.append("palette_visual_score_low")
    return {
        "schema_version": REPORT_SCHEMA_VERSION,
        "review_version": REVIEW_VERSION,
        "status": "review" if warnings else "pass",
        "score": total_score,
        "confidence": round(_number(raw.get("confidence"), 0.0, 1.0, "confidence"), 3),
        "summary": _text(raw.get("summary"), "summary", 500),
        "warnings": list(dict.fromkeys(warnings)),
        "errors": [],
        "checks": checks,
        "strict_preview_sha256": strict_sha,
        "reference_sha256": reference_sha,
        "provider": "openai-compatible",
        "model": model,
        "generated_at": time.time(),
        "cached": False,
    }


def _unavailable(message: str, strict_sha: str = "", reference_sha: str = "") -> dict[str, Any]:
    return {
        "schema_version": REPORT_SCHEMA_VERSION,
        "review_version": REVIEW_VERSION,
        "status": "unavailable",
        "score": 0,
        "confidence": 0.0,
        "summary": "四色视觉复核暂不可用；本地门禁结果不受影响。",
        "warnings": [],
        "errors": ["visual_review_unavailable"],
        "checks": {},
        "strict_preview_sha256": strict_sha,
        "reference_sha256": reference_sha,
        "provider": "openai-compatible",
        "model": os.environ.get("OPENAI_TEXT_MODEL", "gpt-5.4"),
        "generated_at": time.time(),
        "cached": False,
        "diagnostic": message[:300],
    }


def _write_report(path: Path, report: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".part")
    try:
        temporary.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
        os.replace(temporary, path)
    except OSError as exc:
        temporary.unlink(missing_ok=True)
        raise PaletteVisualQualityError(f"Cannot write palette visual report: {path}") from exc


def review_printable_palette_visual_quality(
    strict_preview: Path | str,
    output_directory: Path | str,
    *,
    prompt: str,
    style: str,
    recommendation: Mapping[str, Any],
    reference_path: Path | str | None = None,
    force: bool = False,
    completion: Callable[[str, str, tuple[Path, ...]], str] = complete_vision,
) -> dict[str, Any]:
    output = Path(output_directory)
    report_path = output / REPORT_FILENAME
    strict = Path(strict_preview)
    reference = Path(reference_path) if reference_path else None
    strict_sha = ""
    reference_sha = ""
    try:
        if not strict.is_file():
            raise PaletteVisualQualityError("The strict preview does not exist.")
        strict_sha = _sha256(strict)
        if reference is not None:
            if not reference.is_file():
                raise PaletteVisualQualityError("The reference image does not exist.")
            reference_sha = _sha256(reference)
        model = os.environ.get("OPENAI_TEXT_MODEL", "gpt-5.4")
        if not force and report_path.is_file():
            try:
                cached = json.loads(report_path.read_text(encoding="utf-8"))
                if (
                    isinstance(cached, dict)
                    and cached.get("status") in {"pass", "review"}
                    and cached.get("review_version") == REVIEW_VERSION
                    and cached.get("strict_preview_sha256") == strict_sha
                    and cached.get("reference_sha256") == reference_sha
                    and cached.get("model") == model
                ):
                    cached["cached"] = True
                    return cached
            except (OSError, UnicodeError, json.JSONDecodeError):
                pass
        images = (reference, strict) if reference is not None else (strict,)
        response = completion(
            _system_prompt(reference is not None),
            _user_prompt(prompt, style, recommendation),
            images,
        )
        report = _normalize_review(_json_object(response), strict_sha, reference_sha, model)
    except (OpenAIPreprocessorError, PaletteVisualQualityError, OSError) as exc:
        report = _unavailable(str(exc), strict_sha, reference_sha)
    _write_report(report_path, report)
    return report
