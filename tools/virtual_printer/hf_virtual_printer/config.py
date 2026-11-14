"""Portable hardware profiles; installation identity is deliberately separate."""
from __future__ import annotations

import copy
import json
import math
import os
from pathlib import Path
import re


class PrinterError(Exception):
    def __init__(self, message: str, status: int = 400):
        super().__init__(message)
        self.status = status


def data_directory() -> Path:
    if os.name == "nt":
        return Path(os.environ.get("LOCALAPPDATA", Path.home() / "AppData/Local")) / "HonorForge/VirtualPrinter"
    return Path(os.environ.get("XDG_STATE_HOME", Path.home() / ".local/state")) / "HonorForge/VirtualPrinter"


def write_json(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temp = path.with_suffix(path.suffix + ".tmp")
    temp.write_text(json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False), encoding="utf-8")
    os.replace(temp, path)


def default_config(template: str = "multicolor") -> dict:
    if template not in {"single", "multicolor", "multihead"}:
        raise PrinterError("未知硬件模板")
    heads = 4 if template == "multihead" else 1
    slots = 1 if template == "single" else 4
    colors = ["#FF7748", "#49B9F2", "#F2D469", "#A884EA"]
    return {
        "schema": 1, "name": "Honor Forge 虚拟打印机", "bed_size": [256, 256, 256],
        "nozzles": [{"diameter": 0.4, "max_temperature": 300} for _ in range(heads)],
        "slots": [{"material": "PLA", "color": colors[i], "remaining_g": 1000,
                   "nozzle_temperature": 220, "bed_temperature": 60} for i in range(slots)],
        "tools": [{"nozzle": i if heads > 1 else 0, "slot": i} for i in range(slots)],
        "bed_max_temperature": 120, "storage_mb": 2048, "simulation_speed": 10,
        "lan_enabled": False, "autorun": False,
    }


def number(value, minimum: float, maximum: float, label: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value):
        raise PrinterError(f"{label}必须为有限数值")
    if not minimum <= value <= maximum:
        raise PrinterError(f"{label}范围为 {minimum}–{maximum}")
    return value


def validate_config(value: dict) -> dict:
    if not isinstance(value, dict):
        raise PrinterError("配置必须为对象")
    result = copy.deepcopy(value)
    allowed = set(default_config())
    if set(result) != allowed or type(result.get("schema")) is not int or result["schema"] != 1:
        raise PrinterError("配置字段或版本不正确，请先导出当前配置作为模板")
    name = result["name"]
    if not isinstance(name, str) or not name.strip() or len(name) > 80 or any(ord(c) < 32 for c in name):
        raise PrinterError("名称需为 1–80 个可显示字符")
    result["name"] = name.strip()
    if not isinstance(result["bed_size"], list) or len(result["bed_size"]) != 3:
        raise PrinterError("打印尺寸需要 X、Y、Z 三项")
    for dimension in result["bed_size"]:
        number(dimension, 10, 2000, "打印尺寸")
    nozzles, slots = result["nozzles"], result["slots"]
    if not isinstance(nozzles, list) or not 1 <= len(nozzles) <= 8:
        raise PrinterError("物理喷头数量范围为 1–8")
    if not isinstance(slots, list) or not 0 <= len(slots) <= 16:
        raise PrinterError("料槽数量范围为 0–16")
    for nozzle in nozzles:
        if not isinstance(nozzle, dict) or set(nozzle) != {"diameter", "max_temperature"}:
            raise PrinterError("喷头配置字段不正确")
        number(nozzle["diameter"], 0.1, 2, "喷嘴直径")
        number(nozzle["max_temperature"], 100, 370, "喷头最高温度")
    for slot in slots:
        if not isinstance(slot, dict) or set(slot) != {"material", "color", "remaining_g", "nozzle_temperature", "bed_temperature"}:
            raise PrinterError("料槽配置字段不正确")
        if not isinstance(slot["material"], str) or len(slot["material"]) > 40:
            raise PrinterError("材料名称过长")
        color = slot["color"]
        if not isinstance(color, str) or re.fullmatch(r"#[0-9A-Fa-f]{6}", color) is None:
            raise PrinterError("颜色格式应为 #RRGGBB")
        number(slot["remaining_g"], 0, 100000, "耗材余量")
        number(slot["nozzle_temperature"], 0, 370, "耗材喷头温度")
        number(slot["bed_temperature"], 0, 120, "耗材热床温度")
    if not isinstance(result["tools"], list) or not 1 <= len(result["tools"]) <= 32:
        raise PrinterError("需要 1–32 个工具映射")
    for tool in result["tools"]:
        if not isinstance(tool, dict) or set(tool) != {"nozzle", "slot"}:
            raise PrinterError("工具映射字段不正确")
        for key, count in [("nozzle", len(nozzles)), ("slot", len(slots))]:
            index = tool[key]
            if key == "slot" and count == 0 and index is None:
                continue
            if isinstance(index, bool) or not isinstance(index, int) or not 0 <= index < count:
                raise PrinterError(f"工具映射的 {key} 超出设备配置")
    number(result["bed_max_temperature"], 0, 120, "热床最高温度")
    number(result["storage_mb"], 1, 1048576, "存储容量")
    if type(result["simulation_speed"]) is not int or result["simulation_speed"] not in (1, 10, 50):
        raise PrinterError("仿真速度可选 1、10、50 倍")
    if type(result["lan_enabled"]) is not bool or type(result["autorun"]) is not bool:
        raise PrinterError("网络和启动设置必须为布尔值")
    return result
