"""Single state owner shared by the Moonraker adapter, camera and console."""
from __future__ import annotations

import copy
import json
import math
import re
import threading
import time
import uuid
from collections import deque
from pathlib import Path

from .config import PrinterError, default_config, validate_config, write_json
from .gcode import MAX_FILE, MAX_LINE, TOKEN, Motion, decode, named_parameters, plan_motion, read_metadata

FAULTS = ("offline", "upload_failed", "storage_full", "runout", "overtemperature", "camera_offline")
ACTIVE = {"printing", "paused"}


class VirtualPrinterSession:
    def __init__(self, directory: Path):
        self.directory = directory
        self.directory.mkdir(parents=True, exist_ok=True)
        self.files = directory / "gcodes"
        self.files.mkdir(exist_ok=True)
        self.lock = threading.RLock()
        self.config = validate_config(self._load("config.json", default_config()))
        identity = self._load("identity.json", {"instance_uuid": str(uuid.uuid4())})
        self.instance_uuid = str(uuid.UUID(identity["instance_uuid"]))
        write_json(directory / "identity.json", identity)
        self.history = self._load("history.json", [])[-100:]
        self.events = deque(maxlen=160)
        self.faults = {name: False for name in FAULTS}
        self.endpoint = ""
        self.lan_addresses: list[str] = []
        self.discovery_generation = 0
        self.generation = 0
        self.revision = 0
        self.last_persist = 0.
        self._stream = None
        self._motion: Motion | None = None
        self._wait: tuple[str, int] | None = None
        self._dwell = 0.
        self._unsupported: set[str] = set()
        self.segments = deque(maxlen=18000)
        self.stroke_sequence = 0
        self.state = "standby"
        self.filename = ""
        self.message = "等待打印任务"
        self.progress = 0.
        self.duration = 0.
        self.total_duration = 0.
        self.file_position = 0
        self.layer = 0
        self.metadata: dict = {}
        self.job_id = ""
        self._reset_hardware()
        checkpoint = self._load("current.json", {})
        if checkpoint.get("state") in ACTIVE:
            checkpoint.update(state="interrupted", message="后台重启，任务已中断，请重新开始", finished=time.time())
            self.history.append(checkpoint)
            self.history = self.history[-100:]
            write_json(self.directory / "history.json", self.history)
            self.message = checkpoint["message"]
        write_json(self.directory / "current.json", {})
        self.persist()
        self.log("system", "虚拟打印机已就绪")

    def _load(self, name: str, fallback):
        path = self.directory / name
        return json.loads(path.read_text(encoding="utf-8")) if path.exists() else fallback

    def _reset_hardware(self):
        self.position = [0., 0., 0.]
        self.coordinate_offset = [0., 0., 0.]
        self.e_positions = [0.] * len(self.config["tools"])
        self.absolute = True
        self.e_absolute = True
        self.active_tool = 0
        self.feed = 3000.
        self.speed_factor = 1.
        self.extrusion_factor = 1.
        self.nozzles = [{"temperature": 25., "target": 0.} for _ in self.config["nozzles"]]
        self.bed = {"temperature": 25., "target": 0.}
        self.fan = 0.
        self.light = False
        self.homed = ""
        self.leveling_remaining = 0.
        self.mesh_calibrated = False
        self.filament_used = 0.

    @property
    def active_nozzle(self) -> int:
        return self.config["tools"][self.active_tool]["nozzle"]

    @property
    def active_slot(self) -> int | None:
        return self.config["tools"][self.active_tool]["slot"]

    def log(self, category: str, message: str):
        self.revision += 1
        self.events.append({"time": time.time(), "category": category, "message": message})

    def persist(self):
        with self.lock:
            write_json(self.directory / "config.json", self.config)
            write_json(self.directory / "history.json", self.history[-100:])
            write_json(self.directory / "current.json", self.job_record())
            self.last_persist = time.monotonic()

    def close(self):
        with self.lock:
            self.persist()  # An active task is deliberately recovered as interrupted next time.
            if self._stream:
                self._stream.close()
                self._stream = None

    def configure(self, config: dict):
        with self.lock:
            if self.state in ACTIVE:
                raise PrinterError("打印或暂停时不能修改硬件配置", 409)
            config = validate_config(config)
            if config["storage_mb"] * 1024 ** 2 < self.used_storage():
                raise PrinterError("容量不能小于已保存文件的大小", 409)
            self.config = config
            self._reset_hardware()
            self.segments.clear()
            self.generation += 1
            self.discovery_generation += 1
            self.persist()
            self.log("config", "设备配置已保存，客户端将重新连接")

    def set_speed(self, speed: int):
        if type(speed) is not int or speed not in (1, 10, 50):
            raise PrinterError("仿真速度可选 1、10、50 倍")
        with self.lock:
            self.config["simulation_speed"] = speed
            self.persist()
            self.log("control", f"仿真速度 {speed}×")

    def file_path(self, name: str) -> Path:
        if not isinstance(name, str) or not name or "\\" in name or ":" in name or "\x00" in name:
            raise PrinterError("文件路径不正确")
        rel = Path(name)
        if rel.is_absolute() or any(part in {"..", "."} for part in rel.parts):
            raise PrinterError("文件路径必须在 gcodes 根目录内")
        target = (self.files / rel).resolve()
        if not target.is_relative_to(self.files.resolve()) or target == self.files.resolve():
            raise PrinterError("文件路径必须在 gcodes 根目录内")
        return target

    def used_storage(self) -> int:
        return sum(p.stat().st_size for p in self.files.rglob("*") if p.is_file() and not p.is_symlink())

    def storage(self) -> dict:
        total = self.config["storage_mb"] * 1024 ** 2
        used = self.used_storage()
        return {"total": total, "used": used, "free": 0 if self.faults["storage_full"] else max(0, total - used)}

    def file_list(self) -> list[dict]:
        with self.lock:
            return sorted([{"path": p.relative_to(self.files).as_posix(), "size": p.stat().st_size,
                            "modified": p.stat().st_mtime} for p in self.files.rglob("*")
                           if p.is_file() and not p.is_symlink()], key=lambda item: -item["modified"])

    def metadata_for(self, name: str) -> dict:
        path = self.file_path(name)
        if not path.is_file():
            raise PrinterError("G-code 文件不存在", 404)
        metadata = read_metadata(path)
        metadata["filename"] = name
        return metadata

    def accept_upload(self, stream, name: str) -> dict:
        # Write outside the published gcodes root; aborted uploads never appear as printable files.
        name = name.replace("\\", "/")
        if not name.lower().endswith((".gcode", ".gco", ".g")):
            raise PrinterError("当前虚拟设备接收原始 .gcode / .gco / .g 文件；请关闭发送 3MF")
        path = self.file_path(name)
        import tempfile
        import os
        with self.lock:
            if self.faults["upload_failed"]:
                raise PrinterError("模拟上传失败", 503)
            if self.state in ACTIVE and self.filename == name:
                raise PrinterError("不能覆盖正在打印的文件", 409)
            capacity = self.storage()["free"] + (path.stat().st_size if path.exists() else 0)
        size = 0
        fd, temporary = tempfile.mkstemp(prefix="upload-", suffix=".part", dir=self.directory)
        try:
            with os.fdopen(fd, "wb") as output:
                while chunk := stream.read(1024 * 1024):
                    size += len(chunk)
                    if size > MAX_FILE:
                        raise PrinterError("文件超过 1 GiB 限制", 413)
                    if size > capacity or self.faults["storage_full"]:
                        raise PrinterError("虚拟存储空间不足", 507)
                    if self.faults["upload_failed"] or self.faults["offline"]:
                        raise PrinterError("上传过程中模拟设备故障", 503)
                    if b"\x00" in chunk:
                        raise PrinterError("上传内容不是文本 G-code")
                    output.write(chunk)
            if size == 0:
                raise PrinterError("G-code 不能为空")
            with self.lock:
                if self.state in ACTIVE and self.filename == name:
                    raise PrinterError("不能覆盖正在打印的文件", 409)
                if size > self.storage()["free"] + (path.stat().st_size if path.exists() else 0):
                    raise PrinterError("虚拟存储空间不足", 507)
                path.parent.mkdir(parents=True, exist_ok=True)
                os.replace(temporary, path)
                self.log("upload", f"收到 {name} · {size:,} 字节")
            return {"path": name, "root": "gcodes", "size": size, "modified": path.stat().st_mtime}
        finally:
            Path(temporary).unlink(missing_ok=True)

    def delete_file(self, name: str):
        with self.lock:
            if self.state in ACTIVE and self.filename == name:
                raise PrinterError("不能删除正在打印的文件", 409)
            path = self.file_path(name)
            if not path.is_file():
                raise PrinterError("文件不存在", 404)
            path.unlink()
            self.log("file", f"已删除 {name}")

    def start(self, name: str):
        with self.lock:
            if self.state in ACTIVE:
                raise PrinterError("打印机忙，当前任务尚未结束", 409)
            if self.faults["offline"] or self.faults["overtemperature"] or self.faults["runout"]:
                raise PrinterError("请先清除设备离线、温度或缺料异常", 409)
            metadata = self.metadata_for(name)
            if self._stream:
                self._stream.close()
            self._stream = self.file_path(name).open("rb")
            self._motion = self._wait = None
            self._dwell = 0
            self._unsupported.clear()
            self._explicit_layers = False
            self._extrusion_z = None
            self.absolute = self.e_absolute = True
            self.e_positions = [0.] * len(self.config["tools"])
            self.active_tool = 0
            self.speed_factor = self.extrusion_factor = 1.
            self.metadata = metadata
            self.state, self.filename, self.message = "printing", name, "正在模拟打印"
            self.duration = self.total_duration = self.progress = self.file_position = self.layer = self.filament_used = 0
            self.job_id = str(uuid.uuid4())
            self.segments.clear()
            self.generation += 1
            self.persist()
            self.log("print", f"开始打印 {name}")

    def pause(self):
        with self.lock:
            if self.state != "printing":
                raise PrinterError("仅运行中的任务可以暂停", 409)
            self.state, self.message = "paused", "打印已暂停"
            self.persist()
            self.log("print", self.message)

    def resume(self):
        with self.lock:
            if self.state != "paused":
                raise PrinterError("没有可恢复的暂停任务", 409)
            if self.faults["runout"] or self.faults["overtemperature"]:
                raise PrinterError("请先清除缺料或温度异常", 409)
            if self.active_slot is not None and self.config["slots"][self.active_slot]["remaining_g"] <= 0:
                raise PrinterError("当前料槽已空，请先补充耗材", 409)
            self.state, self.message = "printing", "正在模拟打印"
            self.persist()
            self.log("print", "打印已恢复")

    def finish(self, state: str, message: str):
        self.state, self.message = state, message
        if state == "complete":
            self.progress = 1.
        if self._stream:
            self._stream.close()
            self._stream = None
        self._motion = self._wait = None
        self._dwell = 0
        for heater in [*self.nozzles, self.bed]:
            heater["target"] = 0.
        self.fan = 0.
        record = self.job_record()
        record["finished"] = time.time()
        self.history.append(record)
        self.history = self.history[-100:]
        self.persist()
        self.log("print", message)

    def cancel(self):
        with self.lock:
            if self.state not in ACTIVE:
                raise PrinterError("当前没有可以停止的任务", 409)
            self.finish("cancelled", "打印已停止")

    def set_fault(self, name: str, enabled: bool):
        if name not in FAULTS or type(enabled) is not bool:
            raise PrinterError("异常类型或开关不正确")
        with self.lock:
            self.faults[name] = enabled
            if name == "offline":
                self.discovery_generation += 1
            if enabled and name == "runout" and self.state == "printing":
                self.pause()
                self.message = "模拟缺料，请清除异常后恢复"
            if enabled and name == "overtemperature":
                if self.state in ACTIVE:
                    self.finish("error", "模拟喷头温度异常，任务已停止")
                self.nozzles[self.active_nozzle]["temperature"] = self.config["nozzles"][self.active_nozzle]["max_temperature"] + 15
                self.nozzles[self.active_nozzle]["target"] = 0
            self.log("fault", f"{name} {'已启用' if enabled else '已恢复'}")

    def recover(self):
        with self.lock:
            if self.faults["offline"]:
                self.discovery_generation += 1
            for name in FAULTS:
                self.faults[name] = False
            if self.state == "error":
                self.state, self.message = "standby", "异常已清除，可以开始新任务"
            self.log("fault", "全部模拟异常已清除；暂停任务需手动恢复")

    def refill(self, slot: int, grams: float):
        from .config import number
        if type(slot) is not int or not 0 <= slot < len(self.config["slots"]):
            raise PrinterError("料槽不存在")
        number(grams, 0, 100000, "耗材余量")
        with self.lock:
            self.config["slots"][slot]["remaining_g"] = grams
            self.persist()
            self.log("control", f"料槽 {slot + 1} 余量已更新")

    def heater(self, name: str, target: float):
        if name == "heater_bed":
            heater, maximum = self.bed, self.config["bed_max_temperature"]
        else:
            match = re.fullmatch(r"extruder(\d*)", name)
            if not match:
                raise PrinterError("加热器不存在")
            index = int(match[1] or 0)
            if not 0 <= index < len(self.nozzles):
                raise PrinterError("喷头不存在")
            heater, maximum = self.nozzles[index], self.config["nozzles"][index]["max_temperature"]
        if not math.isfinite(target) or not 0 <= target <= maximum:
            raise PrinterError(f"目标温度范围为 0–{maximum} °C")
        heater["target"] = float(target)

    def script(self, script: str):
        if not isinstance(script, str) or not script.strip() or len(script) > 65536:
            raise PrinterError("控制命令不能为空，且不超过 64 KiB")
        with self.lock:
            for line in script.splitlines():
                command, params, rest = decode(line)
                if command:
                    self._command(command, params, rest, manual=True)
            self.log("control", script[:180])

    def _command(self, command: str, params: dict, rest: str, manual=False):
        named = named_parameters(rest)
        if manual and self.state in ACTIVE and (re.fullmatch(r"T\d+", command) or command in {
                "G0", "G1", "G2", "G3", "G28", "G90", "G91", "G92", "M82", "M83",
                "ACTIVATE_EXTRUDER", "BED_MESH_CALIBRATE"}):
            raise PrinterError("任务运行或暂停时不能手动移动、切换工具或修改坐标模式", 409)
        if manual and command in {"G0", "G1", "G2", "G3", "G92", "G4", "M104", "M109", "M140", "M190", "M106", "M107", "M220", "M221"}:
            if TOKEN.sub("", rest).strip():
                raise PrinterError("控制命令包含无效的数值参数")
        if command in {"G0", "G1", "G2", "G3"}:
            if "F" in params:
                if params["F"] <= 0:
                    raise PrinterError("移动速度必须大于零")
                self.feed = params["F"]
            target = [(params[axis] + self.coordinate_offset[i] if self.absolute else params[axis] + self.position[i])
                      if axis in params else self.position[i]
                      for i, axis in enumerate("XYZ")]
            if any(not 0 <= value <= maximum for value, maximum in zip(target, self.config["bed_size"])):
                raise PrinterError("移动超出虚拟打印范围")
            old_e = self.e_positions[self.active_tool]
            new_e = params.get("E", old_e if self.e_absolute else 0) + (0 if self.e_absolute else old_e)
            extrusion = (new_e - old_e) * self.extrusion_factor
            motion = plan_motion(command, params, self.position, target, self.feed * self.speed_factor, extrusion)
            if any(not 0 <= value <= maximum for point in motion.points for value, maximum in zip(point, self.config["bed_size"])):
                raise PrinterError("圆弧超出虚拟打印范围")
            self.e_positions[self.active_tool] = new_e
            if manual:
                self._deposit(motion, 0, 1, infer_layer=False)
                self.position = target
            else:
                self._motion = motion
        elif command == "G28":
            axes = [axis for axis in "XYZ" if axis in rest.upper()] or list("XYZ")
            for axis in axes:
                self.position["XYZ".index(axis)] = 0
                self.coordinate_offset["XYZ".index(axis)] = 0
            self.homed = "".join(axis for axis in "XYZ" if axis in self.homed + "".join(axes))
        elif command in {"G90", "G91"}:
            self.absolute = command == "G90"
        elif command in {"M82", "M83"}:
            self.e_absolute = command == "M82"
        elif command == "G92":
            for axis in "XYZ":
                if axis in params:
                    index = "XYZ".index(axis)
                    self.coordinate_offset[index] = self.position[index] - params[axis]
            if "E" in params:
                self.e_positions[self.active_tool] = params["E"]
        elif re.fullmatch(r"T\d+", command) or command == "ACTIVATE_EXTRUDER":
            if command == "ACTIVATE_EXTRUDER":
                name = named.get("EXTRUDER", "")
                index = int(name.removeprefix("extruder") or 0) if re.fullmatch(r"extruder\d*", name) else -1
                tool = next((i for i, t in enumerate(self.config["tools"]) if t["nozzle"] == index), -1)
            else:
                tool = int(command[1:])
            if not 0 <= tool < len(self.config["tools"]):
                raise PrinterError(f"工具 {command} 未配置喷头／料槽映射")
            self.active_tool = tool
        elif command in {"M104", "M109", "M140", "M190", "SET_HEATER_TEMPERATURE"}:
            nozzle_index = int(params.get("T", self.active_nozzle))
            name = "heater_bed" if command in {"M140", "M190"} else ("extruder" + (str(nozzle_index) if nozzle_index else ""))
            if command == "SET_HEATER_TEMPERATURE":
                name = named.get("HEATER", "")
                try:
                    target = float(named.get("TARGET", "nan"))
                except ValueError as exc:
                    raise PrinterError("目标温度不正确") from exc
            else:
                target = params.get("S", params.get("R", 0))
            self.heater(name, target)
            if not manual and command in {"M109", "M190"} and target > 0:
                self._wait = (name, 0)
        elif command in {"M106", "M107"}:
            self.fan = max(0, min(1, params.get("S", 255) / 255)) if command == "M106" else 0.
        elif command in {"M220", "M221"}:
            value = params.get("S", 100) / 100
            if not .01 <= value <= 10:
                raise PrinterError("速度或流量倍率范围为 1–1000%")
            if command == "M220":
                self.speed_factor = value
            else:
                self.extrusion_factor = value
        elif command == "G4":
            if not manual:
                self._dwell = max(0, params.get("S", params.get("P", 0) / 1000))
        elif command in {"PAUSE", "M0", "M1", "M25", "M600"}:
            self.pause()
        elif command in {"RESUME", "M24"}:
            self.resume()
        elif command == "CANCEL_PRINT":
            self.cancel()
        elif command == "SDCARD_PRINT_FILE":
            self.start(named.get("FILENAME", ""))
        elif command == "BED_MESH_CALIBRATE":
            self.leveling_remaining = 3.
            self.mesh_calibrated = False
        elif command == "M112":
            self.finish("error", "虚拟紧急停止")
        elif command in {"FIRMWARE_RESTART", "RESTART"}:
            if self.state in ACTIVE:
                self.finish("cancelled", "虚拟固件重启，任务已停止")
            self.recover()
            self._reset_hardware()
            self.state = "standby"
            self.discovery_generation += 1
        elif command in {"START_PRINT", "PRINT_START"}:
            for key, heater in [("BED_TEMP", "heater_bed"), ("EXTRUDER_TEMP", "extruder")]:
                if key in named:
                    try:
                        self.heater(heater, float(named[key]))
                    except ValueError as exc:
                        raise PrinterError("起始宏温度参数不正确") from exc
        elif command in {"END_PRINT", "PRINT_END"}:
            for heater in [*self.nozzles, self.bed]:
                heater["target"] = 0
            self.fan = 0
        elif command in {"M18", "M84"}:
            self.homed = ""
        elif command in {"M117", "M118", "M73", "M400", "EXCLUDE_OBJECT_DEFINE", "EXCLUDE_OBJECT_START", "EXCLUDE_OBJECT_END"}:
            pass  # Bookkeeping / display / synchronization, no physical I/O in a simulator.
        else:
            if manual:
                raise PrinterError(f"未支持的控制命令：{command}")
            if command not in self._unsupported:
                self._unsupported.add(command)
                self.log("unsupported", f"文件命令 {command} 未模拟，已跳过")

    def _deposit(self, motion: Motion, before: float, after: float, *, infer_layer=True):
        if motion.extrusion <= 0:
            return
        extrusion = motion.extrusion * (after - before)
        self.filament_used += extrusion
        slot = self.active_slot
        if slot is not None:
            grams = extrusion * math.pi * .875 ** 2 * 1.24 / 1000
            self.config["slots"][slot]["remaining_g"] = max(0, self.config["slots"][slot]["remaining_g"] - grams)
            if self.config["slots"][slot]["remaining_g"] <= 0:
                self.set_fault("runout", True)
        points = motion.trace(before, after)
        if infer_layer and not self._explicit_layers:
            z = points[-1][2]
            if self._extrusion_z is None or z > self._extrusion_z + .01:
                self.layer += 1
                self._extrusion_z = z
        color = self.config["slots"][slot]["color"] if slot is not None else "#FF7748"
        for start, end in zip(points, points[1:]):
            if math.dist(start, end) > .001:
                self.segments.append((start[:], end[:], color))
                self.stroke_sequence += 1

    def advance(self, seconds: float):
        with self.lock:
            dt = min(1., max(0., seconds)) * self.config["simulation_speed"]
            for heater in [*self.nozzles, self.bed]:
                target = max(25., heater["target"])
                rate = (1.5 if heater is self.bed else 4.) if target > heater["temperature"] else .8
                heater["temperature"] += max(-rate * dt, min(rate * dt, target - heater["temperature"]))
            if self.faults["overtemperature"]:
                self.nozzles[self.active_nozzle]["temperature"] = self.config["nozzles"][self.active_nozzle]["max_temperature"] + 15
            was_leveling = self.leveling_remaining > 0
            self.leveling_remaining = max(0, self.leveling_remaining - dt)
            if was_leveling and self.leveling_remaining == 0:
                self.mesh_calibrated = True
            if self.state != "printing" or not self._stream:
                return
            self.total_duration += dt
            if self.faults["runout"]:
                self.pause()
                return
            self.duration += dt
            budget = dt
            try:
                for _ in range(2500):
                    if self.state != "printing" or budget <= 1e-9:
                        break
                    if self._wait:
                        name = self._wait[0]
                        heater = self.bed if name == "heater_bed" else self.nozzles[int(name.removeprefix("extruder") or 0)]
                        if abs(heater["temperature"] - heater["target"]) > 1.:
                            break
                        self._wait = None
                    if self._dwell:
                        spent = min(budget, self._dwell)
                        budget -= spent
                        self._dwell -= spent
                        continue
                    if self._motion:
                        motion = self._motion
                        spent = min(budget, motion.duration - motion.elapsed)
                        before = motion.elapsed / motion.duration
                        motion.elapsed += spent
                        budget -= spent
                        self.position = motion.position(min(1, motion.elapsed / motion.duration))
                        self._deposit(motion, before, min(1, motion.elapsed / motion.duration))
                        if motion.elapsed >= motion.duration - 1e-8:
                            self.position = motion.points[-1][:]
                            self._motion = None
                        continue
                    line = self._stream.readline(MAX_LINE + 1)
                    if len(line) > MAX_LINE:
                        raise PrinterError("G-code 单行超过 1 MiB")
                    if not line:
                        self.finish("complete", "模拟打印完成")
                        break
                    self.file_position = self._stream.tell()
                    self.progress = min(.999, self.file_position / max(1, self.metadata["size"]))
                    text = line.decode("utf-8", errors="replace").strip()
                    if re.match(r";\s*(LAYER_CHANGE|layer\s+num/total_layer_count)", text, re.I):
                        if not self._explicit_layers:
                            self.layer = 0
                        self._explicit_layers = True
                        self.layer += 1
                    layer_match = re.match(r";\s*LAYER:\s*(\d+)", text, re.I)
                    if layer_match:
                        self._explicit_layers = True
                        self.layer = int(layer_match[1]) + 1
                    command, params, rest = decode(text)
                    if command:
                        self._command(command, params, rest)
            except (PrinterError, ValueError, OverflowError, OSError) as exc:
                self.finish("error", str(exc))
            if time.monotonic() - self.last_persist > 3:
                self.persist()

    def job_record(self) -> dict:
        return {"job_id": self.job_id, "filename": self.filename, "state": self.state,
                "message": self.message, "progress": self.progress, "duration": self.duration,
                "layer": self.layer, "filament_used": self.filament_used}

    def objects(self) -> dict:
        with self.lock:
            extruder = "extruder" + (str(self.active_nozzle) if self.active_nozzle else "")
            result = {
                "print_stats": {"state": self.state, "filename": self.filename, "message": self.message,
                                "print_duration": self.duration, "total_duration": self.total_duration,
                                "filament_used": self.filament_used,
                                "info": {"current_layer": self.layer, "total_layer": self.metadata.get("layer_count") or 0}},
                "virtual_sdcard": {"progress": self.progress, "file_position": self.file_position,
                                   "is_active": self.state == "printing", "file_size": self.metadata.get("size", 0)},
                "toolhead": {"position": self.position[:], "extruder": extruder, "homed_axes": self.homed,
                             "axis_minimum": [0, 0, 0], "axis_maximum": self.config["bed_size"][:]},
                "heater_bed": self.bed.copy(), "fan": {"speed": self.fan},
                "display_status": {"progress": self.progress, "message": self.message},
                "gcode_move": {"gcode_position": [p - o for p, o in zip(self.position, self.coordinate_offset)] + [self.e_positions[self.active_tool]],
                               "speed_factor": self.speed_factor, "extrude_factor": self.extrusion_factor},
                "bed_mesh": {"profile_name": "virtual" if self.mesh_calibrated else "",
                             "mesh_matrix": [[0., .02, 0.], [.01, 0., -.01], [0., -.02, 0.]] if self.mesh_calibrated else []},
                "pause_resume": {"is_paused": self.state == "paused"},
            }
            for i, heater in enumerate(self.nozzles):
                result["extruder" + (str(i) if i else "")] = {**heater, "power": .6 if heater["target"] > heater["temperature"] + 1 else 0,
                                                              "can_extrude": heater["temperature"] >= 170}
            return result

    def lane_data(self) -> dict:
        with self.lock:
            return {f"lane{i}": {"lane": str(i), "material": slot["material"] if slot["remaining_g"] > 0 else "",
                                 "color": slot["color"][1:] + "FF", "nozzle_temp": slot["nozzle_temperature"],
                                 "bed_temp": slot["bed_temperature"]} for i, slot in enumerate(self.config["slots"])}

    def snapshot(self) -> dict:
        with self.lock:
            estimated = self.metadata.get("estimated_time")
            remaining = max(0, estimated - self.duration) if estimated else (self.duration * (1 - self.progress) / self.progress if self.progress > .001 else None)
            if remaining == 0 and self.state in ACTIVE and self.progress > .001:
                remaining = self.duration * (1 - self.progress) / self.progress
            if self.state == "complete":
                remaining = 0
            return {"instance_uuid": self.instance_uuid, "config": copy.deepcopy(self.config),
                    "job": {**self.job_record(), "remaining_seconds": remaining,
                            "total_layers": self.metadata.get("layer_count")},
                    "nozzles": copy.deepcopy(self.nozzles), "bed": self.bed.copy(),
                    "position": self.position[:], "active_tool": self.active_tool,
                    "active_nozzle": self.active_nozzle, "active_slot": self.active_slot,
                    "homed_axes": self.homed, "fan": self.fan, "mesh_calibrated": self.mesh_calibrated,
                    "leveling": self.leveling_remaining > 0, "storage": self.storage(),
                    "faults": self.faults.copy(), "events": list(self.events), "history": self.history[-30:][::-1],
                    "endpoint": self.endpoint, "lan_addresses": self.lan_addresses[:], "revision": self.revision,
                    "generation": self.generation, "discovery_generation": self.discovery_generation}
