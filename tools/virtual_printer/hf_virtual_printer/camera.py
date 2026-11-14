"""Camera frames are produced by the service, independently of open browsers."""
from __future__ import annotations

import io
import threading
import time

from PIL import Image, ImageDraw


class CameraRenderer:
    def __init__(self, session):
        self.session = session
        self.lock = threading.Lock()
        self.frame = b""
        self.sequence = 0
        self.path_generation = -1
        self.drawn_strokes = 0
        self.paths = Image.new("RGBA", (640, 360))
        self.render()

    def render(self):
        with self.session.lock:
            position = self.session.position[:]
            size = self.session.config["bed_size"][:]
            segments = list(self.session.segments)
            strokes = self.session.stroke_sequence
            generation = self.session.generation
            job = self.session.job_record()
            nozzle = self.session.active_nozzle
            tool = self.session.active_tool
            active_slot = self.session.active_slot
            color = self.session.config["slots"][active_slot]["color"] if active_slot is not None else "#FF7748"
        image = Image.new("RGB", (640, 360), "#111922")
        draw = ImageDraw.Draw(image)
        scale = min(440 / max(size[0] + size[1], 1), 190 / max((size[0] + size[1]) * .42, 1))

        def project(p):
            return (320 + (p[0] - p[1]) * scale, 142 + (p[0] + p[1]) * .42 * scale - p[2] * scale)

        corners = [project(p) for p in [[0, 0, 0], [size[0], 0, 0], [size[0], size[1], 0], [0, size[1], 0]]]
        draw.polygon(corners, fill="#263343", outline="#506176")
        for t in range(1, 10):
            draw.line([project([size[0] * t / 10, 0, 0]), project([size[0] * t / 10, size[1], 0])], fill="#344355")
            draw.line([project([0, size[1] * t / 10, 0]), project([size[0], size[1] * t / 10, 0])], fill="#344355")
        for p in [[0, 0, 0], [size[0], 0, 0], [size[0], size[1], 0], [0, size[1], 0]]:
            draw.line([project(p), project([p[0], p[1], min(size[2], 180)])], fill="#2C3D50", width=2)
        # A fixed-size raster retains the entire print, without retaining all G-code paths.
        if generation != self.path_generation:
            self.paths = Image.new("RGBA", (640, 360))
            self.path_generation = generation
            self.drawn_strokes = max(0, strokes - len(segments))
        pending = min(len(segments), max(0, strokes - self.drawn_strokes))
        path_draw = ImageDraw.Draw(self.paths)
        for start, end, segment_color in segments[-pending:] if pending else []:
            path_draw.line([project(start), project(end)], fill=segment_color, width=2)
        self.drawn_strokes = strokes
        image.paste(self.paths, (0, 0), self.paths)
        x, y = project(position)
        draw.line([(x, 45), (x, y - 15)], fill="#77889A", width=3)
        draw.rounded_rectangle((x - 12, y - 30, x + 12, y - 8), 5, fill="#CDD7DF", outline="#FFFFFF")
        draw.polygon([(x - 6, y - 8), (x + 6, y - 8), (x, y + 2)], fill=color)
        draw.ellipse((x - 2, y - 1, x + 2, y + 3), fill="#FFE7BE")
        draw.rounded_rectangle((16, 14, 194, 39), 6, fill="#1C2E33")
        draw.ellipse((25, 23, 31, 29), fill="#6EE7B7")
        draw.text((38, 21), "VIRTUAL CAMERA  /  5 FPS", fill="#91E4C7")
        draw.text((16, 320), f"T{tool}  /  NOZZLE {nozzle + 1}    Z {position[2]:.2f} mm", fill="#D1DFEC")
        draw.text((16, 338), f"LAYER {job['layer']:03d}    {job['state'].upper()}    {job['progress'] * 100:.1f}%", fill="#7C92AA")
        stamp = time.strftime("%H:%M:%S") + f".{int(time.time() * 10) % 10}"
        draw.text((540, 21), stamp, fill="#A7B7C7")
        buffer = io.BytesIO()
        image.save(buffer, format="JPEG", quality=82)
        with self.lock:
            self.frame = buffer.getvalue()
            self.sequence += 1

    def jpeg(self) -> bytes:
        with self.lock:
            return self.frame


def application_icon(size=64):
    image = Image.new("RGBA", (size, size), "#101820")
    draw = ImageDraw.Draw(image)
    s = size / 64
    draw.rounded_rectangle([int(v * s) for v in (7, 6, 57, 58)], radius=int(11 * s), fill="#263444", outline="#4D91FF", width=max(1, int(2 * s)))
    draw.line([(int(x * s), int(y * s)) for x, y in [(16, 45), (32, 53), (49, 44)]], fill="#8FA8C3", width=max(1, int(2 * s)))
    draw.polygon([(int(x * s), int(y * s)) for x, y in [(21, 32), (32, 26), (43, 32), (43, 44), (32, 50), (21, 44)]], fill="#FF8856")
    draw.line([(int(32 * s), int(12 * s)), (int(32 * s), int(24 * s))], fill="#E5ECF4", width=max(2, int(5 * s)))
    return image
