"""Bounded, streaming G-code decoding and motion planning, without firmware I/O."""
from __future__ import annotations

import math
import re
from dataclasses import dataclass
from pathlib import Path

from .config import PrinterError

# G-code's E is an extrusion axis, including in compact commands such as X10E1.
TOKEN = re.compile(r"([A-Za-z])\s*([-+]?(?:\d*\.\d+|\d+\.?))")
COMMAND = re.compile(r"^([GMT]\d+(?:\.\d+)?|[A-Z_]+)(?:\s|(?=[XYZEFSTIJKRPN]))?(.*)$", re.I)
MAX_LINE = 1024 * 1024
MAX_FILE = 1024 * 1024 * 1024


def decode(line: str) -> tuple[str, dict, str]:
    code = re.sub(r"\([^)]*\)", "", line.split(";", 1)[0]).strip()
    code = re.sub(r"^N\d+\s*", "", code, flags=re.I).split("*", 1)[0].strip()
    if not code:
        return "", {}, ""
    match = COMMAND.match(code)
    if not match:
        return code.upper(), {}, ""
    command, rest = match[1].upper(), match[2].strip()
    params = {key.upper(): float(value) for key, value in TOKEN.findall(rest)}
    if not all(math.isfinite(value) for value in params.values()):
        raise PrinterError("G-code 参数必须是有限数值")
    return command, params, rest


def named_parameters(text: str) -> dict[str, str]:
    return {m[1].upper(): m[2] if m[2] is not None else m[3]
            for m in re.finditer(r'([A-Za-z_]+)=(?:"([^"]*)"|([^\s]+))', text)}


@dataclass
class Motion:
    points: list[list[float]]
    duration: float
    extrusion: float
    elapsed: float = 0

    def position(self, fraction: float) -> list[float]:
        if fraction >= 1:
            return self.points[-1][:]
        at = min(len(self.points) - 1.000001, max(0, fraction) * (len(self.points) - 1))
        index = int(at)
        blend = at - index
        return [a + (b - a) * blend for a, b in zip(self.points[index], self.points[index + 1])]

    def trace(self, before: float, after: float) -> list[list[float]]:
        """Preserve arc curvature even when one engine tick crosses the whole arc."""
        count = len(self.points) - 1
        interior = self.points[math.floor(before * count) + 1:math.ceil(after * count)]
        return [self.position(before), *interior, self.position(after)]


def plan_motion(command: str, params: dict, start: list[float], end: list[float], feed: float, extrusion: float) -> Motion:
    points = [start[:], end[:]]
    if command in {"G2", "G3"}:
        if "I" in params or "J" in params:
            center = [start[0] + params.get("I", 0), start[1] + params.get("J", 0)]
        elif "R" in params:
            radius = abs(params["R"])
            dx, dy = end[0] - start[0], end[1] - start[1]
            chord = math.hypot(dx, dy)
            if not chord or chord > 2 * radius + 1e-6:
                raise PrinterError("圆弧半径与端点不匹配")
            h = math.sqrt(max(0, radius * radius - chord * chord / 4))
            sign = (1 if command == "G3" else -1) * (1 if params["R"] > 0 else -1)
            center = [(start[0] + end[0]) / 2 - sign * dy * h / chord,
                      (start[1] + end[1]) / 2 + sign * dx * h / chord]
        else:
            raise PrinterError("圆弧缺少 I/J 或 R 参数")
        radius = math.hypot(start[0] - center[0], start[1] - center[1])
        if radius < 1e-9 or abs(math.hypot(end[0] - center[0], end[1] - center[1]) - radius) > max(0.1, radius * .01):
            raise PrinterError("圆弧中心与端点不匹配")
        angle = math.atan2(start[1] - center[1], start[0] - center[0])
        sweep = (math.atan2(end[1] - center[1], end[0] - center[0]) - angle) % (2 * math.pi)
        if command == "G2":
            sweep = sweep - 2 * math.pi if sweep > 1e-9 else -2 * math.pi
        elif sweep < 1e-9:
            sweep = 2 * math.pi
        steps = max(4, min(512, math.ceil(abs(sweep) * radius / .8)))
        points = [[center[0] + radius * math.cos(angle + sweep * i / steps),
                   center[1] + radius * math.sin(angle + sweep * i / steps),
                   start[2] + (end[2] - start[2]) * i / steps] for i in range(steps + 1)]
        points[0], points[-1] = start[:], end[:]
    distance = sum(math.dist(a, b) for a, b in zip(points, points[1:]))
    duration = max(distance, abs(extrusion) if distance < 1e-8 else 0) / max(.01, feed / 60)
    return Motion(points, max(.001, duration), extrusion)


def read_metadata(path: Path) -> dict:
    metadata = {"filename": path.name, "size": path.stat().st_size,
                "modified": path.stat().st_mtime, "estimated_time": None, "layer_count": None}
    # Orca headers contain the needed metadata. No full-file allocation or scan on HTTP requests.
    with path.open("rb") as stream:
        header = stream.read(min(256 * 1024, metadata["size"])).decode("utf-8", errors="replace")
    for line in header.splitlines():
        if "estimated printing time" in line.lower():
            match = re.search(r"=\s*(?:(\d+)d\s*)?(?:(\d+)h\s*)?(?:(\d+)m\s*)?([\d.]+)s", line)
            if match:
                metadata["estimated_time"] = sum(float(n or 0) * scale for n, scale in zip(match.groups(), [86400, 3600, 60, 1]))
        match = re.search(r"(?:total layer number|total_layer_count|LAYER_COUNT)\s*[:=]\s*(\d+)", line, re.I)
        if match:
            metadata["layer_count"] = int(match[1])
        match = re.search(r";\s*TIME:\s*([\d.]+)", line, re.I)
        if match:
            metadata["estimated_time"] = float(match[1])
    return metadata


def demo_gcode() -> bytes:
    lines = ["; Honor Forge virtual printer demo", "; estimated printing time (normal mode) = 32m 0s",
             "; total layer number: 40", "G90", "M82", "G28", "M104 S200", "M140 S60", "M109 S200", "G92 E0", "M106 S160"]
    extrusion = 0.
    for layer in range(40):
        lines += [";LAYER_CHANGE", f"G1 Z{(layer + 1) * .24:.2f} F1200", "G0 X90 Y90 F6000"]
        for inset in range(4):
            a, b = 90 + inset * .5, 166 - inset * .5
            for x, y in [(b, a), (b, b), (a, b), (a, a)]:
                extrusion += 2.5
                lines.append(f"G1 X{x:.2f} Y{y:.2f} E{extrusion:.3f} F3000")
        for x in range(94, 163, 6):
            extrusion += 3
            lines.append(f"G1 X{x} Y94 E{extrusion:.3f} F4500")
            extrusion += 3
            lines.append(f"G1 X{x} Y162 E{extrusion:.3f} F4500")
    lines += ["M104 S0", "M140 S0", "M107", "G0 X20 Y230 F6000", "M84"]
    return ("\n".join(lines) + "\n").encode()
