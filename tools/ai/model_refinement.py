from __future__ import annotations

from collections.abc import Mapping, Sequence
from dataclasses import dataclass
from typing import Any


REPORT_SCHEMA_VERSION = 1
MAX_ISSUES = 6
MAX_SUMMARY_BYTES = 240
MAX_PROMPT_SUFFIX_BYTES = 900
MAX_TITLE_BYTES = 120
MAX_INSTRUCTION_BYTES = 360


@dataclass(frozen=True)
class _AdviceRule:
    category: str
    codes: tuple[str, ...]
    title: str
    instruction: str


_RULES = (
    _AdviceRule(
        "topology",
        (
            "degenerate_faces",
            "boundary_edges",
            "non_manifold_edges",
            "inconsistent_winding_edges",
            "flat_or_empty_axis",
            "repairable_boundary_edges",
            "repairable_non_manifold_edges",
            "repairable_inconsistent_winding_edges",
        ),
        "网格拓扑需要更稳定",
        "生成闭合、流形且无自交的连续实体，避免孔洞、重叠壳体和退化薄片。",
    ),
    _AdviceRule(
        "attachments",
        (
            "floating_disconnected_components",
            "tiny_detached_components",
            "visual_detached_artifacts",
        ),
        "存在分离或漂浮部件",
        "删除无意义碎片，并让所有必要部件与主体或底座形成清晰、牢固的连接。",
    ),
    _AdviceRule(
        "thickness",
        ("thin_structural_components", "thin_local_wall_regions", "extreme_aspect_ratio"),
        "薄壁或细连接风险",
        "加粗薄壁、把手和连接颈，使用圆滑过渡并与主体可靠连接。",
    ),
    _AdviceRule(
        "base",
        ("weak_bed_contact", "visual_base_relationship"),
        "底部支撑关系不足",
        "扩大平整接地面；不稳定主体使用一体式底座，并避免仅靠细小触点站立。",
    ),
    _AdviceRule(
        "overhang",
        ("high_downward_surface_ratio", "localized_overhang_regions"),
        "悬垂区域偏多",
        "将大悬垂改成自支撑斜面、拱形或渐变过渡，减少水平朝下的平台。",
    ),
    _AdviceRule(
        "detail",
        ("dense_micro_triangles",),
        "表面微细节过密",
        "简化不可打印的微细节，保留清楚的大轮廓和尺寸足够的实体特征。",
    ),
    _AdviceRule(
        "identity",
        ("visual_identity_mismatch",),
        "主体身份或人脸相似度不足",
        "严格对照原图保留身份特征；人像保持脸宽、眼距、眉形、鼻形、嘴形、笑容、下颌、年龄与自然不对称，禁止泛化成玩偶脸。",
    ),
    _AdviceRule(
        "semantics",
        (
            "visual_subject_incomplete",
            "visual_semantic_incoherence",
            "visual_silhouette_unclear",
        ),
        "主体完整性或轮廓不清",
        "保持主体及标志性部件完整，从前后左右都形成清晰、可辨认的一致轮廓。",
    ),
    _AdviceRule(
        "color",
        (
            "colors_outside_target_palette",
            "too_few_meaningful_target_palette_colors",
            "tiny_printable_color_regions",
            "visual_color_regions_unclear",
            "visual_material_color_mixing",
        ),
        "材料色区需要更清楚",
        "只使用确认的目标色，把肤色、衣物、头发和底座限制在各自部位，并组织成更大、更连续的实体材料区域，减少串色、斑点和细碎色带。",
    ),
)


def _truncate_utf8(value: str, byte_limit: int) -> str:
    encoded = value.encode("utf-8")
    if len(encoded) <= byte_limit:
        return value
    return encoded[:byte_limit].decode("utf-8", errors="ignore").rstrip()


def _active_codes(report: Mapping[str, Any] | None) -> set[str]:
    if not isinstance(report, Mapping) or report.get("status") not in {"review", "reject"}:
        return set()
    result: set[str] = set()
    for field in ("errors", "warnings"):
        values = report.get(field, ())
        if not isinstance(values, Sequence) or isinstance(values, (str, bytes, bytearray)):
            continue
        result.update(value for value in values if isinstance(value, str) and value.isascii())
    return result


def _empty_advice() -> dict[str, Any]:
    return {
        "schema": REPORT_SCHEMA_VERSION,
        "available": False,
        "summary": "",
        "prompt_suffix": "",
        "issues": [],
    }


def build_model_refinement_advice(
    model_quality: Mapping[str, Any] | None,
    visual_quality: Mapping[str, Any] | None,
) -> dict[str, Any]:
    active = _active_codes(model_quality) | _active_codes(visual_quality)
    issues: list[dict[str, str]] = []
    for rule in _RULES:
        matched = next((code for code in rule.codes if code in active), None)
        if matched is None:
            continue
        issues.append({
            "code": matched,
            "category": rule.category,
            "title": _truncate_utf8(rule.title, MAX_TITLE_BYTES),
            "instruction": _truncate_utf8(rule.instruction, MAX_INSTRUCTION_BYTES),
        })
        if len(issues) >= MAX_ISSUES:
            break
    if not issues:
        return _empty_advice()

    summary = _truncate_utf8(
        f"检测到 {len(issues)} 类可在下一次生成中改善的问题。应用建议不会自动调用付费服务。",
        MAX_SUMMARY_BYTES,
    )
    prompt_suffix = _truncate_utf8(
        "打印优化要求：" + "；".join(issue["instruction"].rstrip("。") for issue in issues) + "。",
        MAX_PROMPT_SUFFIX_BYTES,
    )
    return {
        "schema": REPORT_SCHEMA_VERSION,
        "available": True,
        "summary": summary,
        "prompt_suffix": prompt_suffix,
        "issues": issues,
    }
