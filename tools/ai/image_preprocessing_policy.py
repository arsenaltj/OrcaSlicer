"""Local subject routing and bounded image-to-3D preprocessing policy.

Keyword evidence is not visual recognition. Unknown subjects retain the
existing prompt; material risks never replace the subject category.
"""
from __future__ import annotations

try:
    from . import image_preprocessing_prompts as prompt_rules
except ImportError:
    import image_preprocessing_prompts as prompt_rules

import math
from pathlib import Path
import re
from typing import Any, Mapping

POLICY_VERSION = "image-preprocessing-v2"
CATEGORIES = ("portrait", "animal", "architecture", "hard_surface", "organic", "flat_graphic", "scene", "effects")
SUBJECT_KEYWORDS = (
    ("portrait", ("人像", "人物", "头像", "肖像", "自拍", "男士", "女士", "男孩", "女孩", "portrait", "person", "people", "face", "selfie", "man", "woman", "boy", "girl")),
    ("scene", ("场景", "群像", "多人", "多物体", "街景", "风景", "scene", "group", "landscape", "diorama")),
    ("animal", ("宠物", "动物", "猫", "狗", "犬", "兔", "鸟", "鱼", "马", "熊", "龙", "龟", "cat", "dog", "pet", "animal", "rabbit", "bird", "horse", "bear", "dragon", "turtle", "golden retriever", "poodle")),
    ("architecture", ("建筑", "房屋", "大楼", "塔", "桥", "寺庙", "城堡", "亭", "architecture", "building", "house", "tower", "bridge", "temple", "castle")),
    ("hard_surface", ("汽车", "车辆", "机器人", "机甲", "机器", "机械", "挖掘机", "显微镜", "产品", "家具", "椅子", "桌子", "相机", "手机", "工具", "雨伞", "扇子", "花瓶", "首饰", "戒指", "项链", "包袋", "背包", "螺丝", "齿轮", "卡扣", "vehicle", "car", "robot", "machine", "excavator", "microscope", "product", "furniture", "chair", "table", "camera", "phone", "tool", "umbrella", "fan", "vase", "jewelry", "ring", "necklace", "bag", "backpack", "screw", "gear", "clip")),
    ("flat_graphic", ("logo", "标志", "图标", "文字", "字体", "徽章", "标牌", "海报", "icon", "badge", "sign", "lettering", "typography")),
    ("organic", ("花", "植物", "树", "盆景", "珊瑚", "鹿角", "叶片", "食物", "蛋糕", "水果", "蔬菜", "plant", "flower", "tree", "bonsai", "coral", "antler", "leaf", "food", "cake", "fruit", "vegetable")),
)
FEATURE_KEYWORDS = {
    "mechanical": ("挖掘机", "机械", "机器", "车辆", "汽车", "显微镜", "相机", "工具", "excavator", "machine", "vehicle", "car", "microscope", "camera", "tool", "robot"),
    "branching": ("植物", "盆景", "树", "枝", "珊瑚", "鹿角", "plant", "bonsai", "tree", "branch", "coral", "antler"),
    "thin_surface": ("雨伞", "扇子", "羽扇", "翅膀", "船帆", "叶片", "羽毛", "umbrella", "fan", "wing", "sail", "leaf", "feather", "canopy"),
    "hollow": ("首饰", "戒指", "项链", "链条", "镂空", "网格", "花瓶", "jewelry", "ring", "necklace", "chain", "lattice", "hollow", "vase"),
    "soft_surface": ("布艺", "衣服", "包袋", "背包", "食物", "蛋糕", "水果", "fabric", "cloth", "bag", "backpack", "food", "cake", "fruit"),
    "precision": ("齿轮", "卡扣", "螺纹", "螺丝", "装配", "公差", "gear", "thread", "screw", "snap fit", "tolerance", "assembly"),
}
MATERIAL_KEYWORDS = {
    "transparent": ("透明", "玻璃", "glass", "transparent", "translucent"),
    "reflective": ("镜面", "反光", "镀铬", "chrome", "mirror", "reflective", "polished metal"),
    "transient": ("烟雾", "火焰", "液体", "水花", "smoke", "fire", "flame", "liquid", "splash"),
}


def _matching_positions(text: str, words: tuple[str, ...]):
    text = re.sub(r"[_-]+", " ", text.casefold())
    for word in words:
        pattern = re.escape(word)
        if word.isascii():
            pattern = r"(?<![a-z])" + pattern + r"(?![a-z])"
        for match in re.finditer(pattern, text):
            if word == "花" and text[max(0, match.start()-1):match.start()] == "水":
                continue
            before = text[max(0, match.start()-32):match.start()]
            # A request to remove background scenery/props is not a subject hint.
            removal = re.search(r"(?:remove|erase|delete|without|不要|去除|去掉|删除|移除|清除)\s*(?:the\s*|extra\s*|background\s*|背景[中里的]*\s*)*$", before)
            if removal and not re.search(r"(?:do not|don't|不要|别)\s*$", before[:removal.start()]):
                continue
            yield match.start()


def _matches(text: str, words: tuple[str, ...]) -> bool:
    return next(_matching_positions(text, words), None) is not None


def classify_reference_subject(text: str, *, filename: str = "", category_hint: str = "") -> dict[str, Any]:
    if not all(isinstance(value, str) for value in (text, filename, category_hint)):
        raise ValueError("Subject evidence must be text.")
    if category_hint and category_hint not in CATEGORIES:
        raise ValueError("Unsupported image preprocessing subject category.")
    def category(value: str) -> str:
        hits = []
        for order, (key, words) in enumerate(SUBJECT_KEYWORDS):
            positions = list(_matching_positions(value, words))
            if positions:
                if key in {"portrait", "scene"}:
                    return key
                hits.append((min(positions), order, key))
        return min(hits)[2] if hits else ""
    subject = category_hint or category(text)
    evidence = "explicit_hint" if category_hint else "description" if subject else "unknown"
    if not subject and filename:
        subject = category(Path(filename).stem)
        if subject:
            evidence = "filename"
    risk_evidence = text + (" " + Path(filename).stem if evidence == "filename" else "")
    risks = [key for key, words in MATERIAL_KEYWORDS.items() if _matches(risk_evidence, words)]
    if not subject and risks:
        subject, evidence = "effects", "material_only"
    features = [key for key, words in FEATURE_KEYWORDS.items() if _matches(text, words)]
    if evidence == "filename":
        features = [key for key, words in FEATURE_KEYWORDS.items() if _matches(Path(filename).stem, words)]
    return {"subject": subject or "unknown", "features": features, "material_risks": risks,
            "evidence": evidence, "confidence": "high" if category_hint else "medium" if subject else "low"}


def _text_policy(instruction: str, subject: str, requested: str) -> str:
    if requested not in {"auto", "preserve", "remove"}:
        raise ValueError("Subject text policy must be auto, preserve or remove.")
    if requested != "auto":
        return requested
    removal = re.search(r"(?:remove|erase|delete|去除|清除|删除|去掉)\s*(?:the\s*|subject[ 's]*\s*|主体[上的]*\s*)?(?:logo|text|lettering|文字|字标|标志)", instruction, re.I)
    if removal and not re.search(r"(?:do not|don't|不要|别)\s*$", instruction[:removal.start()], re.I):
        return "remove"
    if subject == "flat_graphic" or re.search(r"(?:preserve|retain|keep|保留|保持).{0,15}(?:logo|text|lettering|文字|字标|标志)", instruction, re.I):
        return "preserve"
    return "auto"


def build_image_preprocessing_policy(instruction: str, style: str, *, filename: str = "",
                                     print_settings: Mapping[str, Any] | None = None,
                                     category_hint: str = "", subject_text: str = "auto",
                                     material_proxy: bool = False) -> dict[str, Any]:
    if not isinstance(material_proxy, bool):
        raise ValueError("Material proxy must be a boolean.")
    profile = classify_reference_subject(instruction, filename=filename, category_hint=category_hint)
    settings = {}
    if print_settings is not None:
        if not isinstance(print_settings, Mapping):
            raise ValueError("Print settings must be a mapping.")
        try:
            from .printable_image_pipeline import PrintSettings, PrintableImageError
        except ImportError:
            from printable_image_pipeline import PrintSettings, PrintableImageError
        try:
            parsed = PrintSettings.from_mapping(dict(print_settings))
        except PrintableImageError as exc:
            raise ValueError(str(exc)) from None
        settings = {key: getattr(parsed, key) for key in ("width_mm", "nozzle_mm", "line_width_mm", "minimum_feature_mm")}
        if any(not math.isfinite(value) for value in settings.values()):
            raise ValueError("Preprocessing print dimensions must be finite.")
    return {"version": POLICY_VERSION, **profile, "style": style, "user_direction": instruction.strip(),
            "subject_text": _text_policy(instruction, profile["subject"], subject_text),
            "material_proxy": material_proxy, "print_constraints": settings,
            "specialized": profile["subject"] not in {"portrait", "unknown"},
            "view_policy": "preserve_source_view; real additional views preferred; generated views are unverified",
            "physical_print_qualified": False}


def print_scale_direction(policy: Mapping[str, Any]) -> str:
    settings = policy["print_constraints"]
    if not settings:
        return ""
    return prompt_rules.PRINT_SCALE_TEMPLATE.format(**settings)


def subject_structure_direction(policy: Mapping[str, Any]) -> str:
    return "".join(layer["text"] for layer in subject_structure_layers(policy))


def subject_structure_layers(policy: Mapping[str, Any]) -> list[dict[str, str]]:
    """Return the selected editable rules, including exact join whitespace."""
    subject, features = policy["subject"], policy["features"]
    selected = [(f"subject.{subject}", f"SUBJECT_RULES.{subject}", prompt_rules.SUBJECT_RULES.get(subject, ""))]
    selected.extend((f"feature.{key}", f"FEATURE_RULES.{key}", prompt_rules.FEATURE_RULES[key])
                    for key in features if subject in prompt_rules.FEATURE_SUBJECTS.get(key, set()))
    if policy["material_risks"]:
        selected.append(("material.risk", "COMMON_RULES.material_risk", prompt_rules.COMMON_RULES["material_risk"]))
    selected.append(("common.checklist", "COMMON_RULES.source_checklist", prompt_rules.COMMON_RULES["source_checklist"]))
    layers = []
    for name, rule, text in selected:
        if text:
            layers.append({"id": name, "rule": rule, "text": (" " if layers else "") + text})
    scale = print_scale_direction(policy)
    if scale:
        layers.append({"id": "print.scale", "rule": "PRINT_SCALE_TEMPLATE", "text": scale})
    return layers
