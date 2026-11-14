"""HTTP / JSON-RPC adapters for the shared simulator session."""
from __future__ import annotations

import asyncio
import contextlib
import ipaddress
import logging
import re
import time
from contextlib import asynccontextmanager
from pathlib import Path
from urllib.parse import urlsplit

from fastapi import FastAPI, Request, WebSocket, WebSocketDisconnect
from fastapi.responses import FileResponse, JSONResponse, Response, StreamingResponse
from fastapi.staticfiles import StaticFiles

from . import __version__
from .camera import CameraRenderer
from .config import PrinterError
from .gcode import demo_gcode


def local_host(host: str | None) -> bool:
    try:
        return ipaddress.ip_address(host or "").is_loopback
    except ValueError:
        return (host or "").lower() == "localhost"


def parse_authority(value: str, scheme: str) -> tuple[str, int]:
    """Parse a single literal authority without DNS resolution or permissive URL cleanup."""
    if re.fullmatch(r"(?:\[[0-9A-Fa-f:.]+\]|[A-Za-z0-9.-]+)(?::[0-9]+)?", value) is None:
        raise PrinterError("连接地址格式不正确", 403)
    try:
        parsed = urlsplit(f"{scheme}://{value}")
        host, port = parsed.hostname, parsed.port
        if not host or parsed.netloc != value or parsed.path or parsed.query or parsed.fragment:
            raise ValueError()
        if port is None:
            port = 443 if scheme == "https" else 80
        if not 1 <= port <= 65535:
            raise ValueError()
        return host.lower().removesuffix("."), port
    except ValueError as exc:
        raise PrinterError("连接地址格式不正确", 403) from exc


def require_connection(connection, session, *, management=False):
    """One HTTP/WebSocket gate: peer scope, trusted Host, listening port, then Origin."""
    hosts, origins = connection.headers.getlist("host"), connection.headers.getlist("origin")
    scheme = {"http": "http", "https": "https", "ws": "http", "wss": "https"}.get(connection.scope["scheme"])
    if len(hosts) != 1 or len(origins) > 1 or scheme is None:
        raise PrinterError("连接地址或来源不明确", 403)
    host, port = parse_authority(hosts[0], scheme)
    # The launcher records the bound port before accepting traffic. ASGI tests use their listener scope.
    listener = connection.scope.get("server")
    expected_port = urlsplit(session.endpoint).port if session.endpoint else (listener[1] if listener else None)
    if port != expected_port:
        raise PrinterError("连接端口不匹配", 403)
    local_peer = local_host(connection.client.host if connection.client else None)
    local_authority = local_host(host)
    if management and not (local_peer and local_authority):
        raise PrinterError("中控管理仅允许本机访问；局域网可使用打印机协议接口", 403)
    if not management:
        trusted = local_authority
        if session.config["lan_enabled"]:
            try:
                address = ipaddress.ip_address(host)
                trusted = not (address.is_unspecified or address.is_multicast)
            except ValueError:
                trusted = trusted or host == f"hf-virtual-{session.instance_uuid[:12]}.local"
        if not trusted or (not session.config["lan_enabled"] and not local_peer):
            raise PrinterError("当前连接地址未获允许", 403)
    if origins:
        origin = origins[0]
        try:
            parsed = urlsplit(origin)
            if (origin != f"{parsed.scheme}://{parsed.netloc}" or parsed.scheme != scheme or
                    parsed.path or parsed.query or parsed.fragment or
                    parse_authority(parsed.netloc, scheme) != (host, port)):
                raise ValueError()
        except ValueError as exc:
            raise PrinterError("请求来源不匹配", 403) from exc


def create_app(session, *, engine=True, network_changed=None, shutdown=None) -> FastAPI:
    camera = CameraRenderer(session)
    clients: dict[WebSocket, dict] = {}
    generation = session.discovery_generation
    frame_tick = 0.

    async def engine_loop():
        nonlocal generation, frame_tick
        previous = time.monotonic()
        while True:
            now = time.monotonic()
            try:
                await asyncio.to_thread(session.advance, now - previous)
            except Exception:
                logging.exception("Simulation engine failure")
                with session.lock:
                    if session.state in {"printing", "paused"}:
                        session.finish("error", "仿真引擎异常，请查看后台日志")
            previous = now
            if now - frame_tick >= .2:
                try:
                    await asyncio.to_thread(camera.render)
                except Exception:
                    logging.exception("Camera renderer failure")
                frame_tick = now
            if generation != session.discovery_generation:
                generation = session.discovery_generation
                for ws in list(clients):
                    with contextlib.suppress(Exception):
                        await ws.close(code=1012, reason="Printer configuration changed")
                    clients.pop(ws, None)
                if network_changed:
                    await asyncio.to_thread(network_changed)
            if session.faults["offline"]:
                for ws in list(clients):
                    with contextlib.suppress(Exception):
                        await ws.close(code=1013, reason="Simulated printer offline")
                    clients.pop(ws, None)
            else:
                status = session.objects()
                for ws, objects in list(clients.items()):
                    payload = {"jsonrpc": "2.0", "method": "notify_status_update",
                               "params": [filter_objects(status, objects), now]}
                    try:
                        await asyncio.wait_for(ws.send_json(payload), timeout=.25)
                    except Exception:
                        clients.pop(ws, None)
            await asyncio.sleep(.2)

    @asynccontextmanager
    async def lifespan(app):
        task = asyncio.create_task(engine_loop()) if engine else None
        yield
        if task:
            task.cancel()
            with contextlib.suppress(asyncio.CancelledError):
                await task
        for ws in list(clients):
            with contextlib.suppress(Exception):
                await ws.close(code=1001)
        session.close()

    app = FastAPI(title="Honor Forge Virtual Printer", version=__version__, lifespan=lifespan,
                  docs_url=None, redoc_url=None, openapi_url=None)
    app.state.session, app.state.camera = session, camera

    @app.exception_handler(PrinterError)
    async def printer_error(request, error):
        return JSONResponse({"error": {"code": error.status, "message": str(error)}}, status_code=error.status)

    @app.middleware("http")
    async def gate(request: Request, call_next):
        path = request.scope["path"]
        management = path == "/" or path.startswith(("/sim/", "/assets/"))
        try:
            require_connection(request, session, management=management)
            if not management and session.faults["offline"]:
                raise PrinterError("模拟设备离线", 503)
            if path not in {"/webcam/", "/sim/api/v1/state"} and not path.startswith("/assets/"):
                with session.lock:
                    session.log("api", f"{request.method} {path}")
            response = await call_next(request)
            response.headers["X-Content-Type-Options"] = "nosniff"
            response.headers["Referrer-Policy"] = "same-origin"
            return response
        except PrinterError as exc:
            return await printer_error(request, exc)

    def result(value):
        return {"result": value}

    def filter_objects(status: dict, requested: dict) -> dict:
        return {name: ({key: value for key, value in status[name].items() if key in fields} if isinstance(fields, list) else status[name])
                for name, fields in requested.items() if name in status}

    @app.get("/server/info")
    def server_info():
        return result({"moonraker_version": f"0.9.3-hf-virtual-{__version__}", "klippy_connected": True,
                       "klippy_state": "shutdown" if session.faults["overtemperature"] else "ready",
                       "components": ["file_manager", "webcam", "database"], "failed_components": [],
                       "machine_name": session.config["name"], "hostname": "hf-virtual-printer",
                       "instance_uuid": session.instance_uuid, "hf_virtual_printer": True})

    @app.get("/printer/info")
    def printer_info():
        return result({"state": "ready", "state_message": session.message, "hostname": "hf-virtual-printer",
                       "machine_name": session.config["name"], "software_version": f"virtual-{__version__}"})

    @app.get("/printer/objects/list")
    def object_list():
        return result({"objects": list(session.objects())})

    @app.get("/printer/objects/query")
    def query(request: Request):
        objects = {key: value.split(",") if value else None for key, value in request.query_params.items()}
        return result({"status": filter_objects(session.objects(), objects or {name: None for name in session.objects()}),
                       "eventtime": time.monotonic()})

    @app.get("/server/files/roots")
    def roots():
        return result([{"name": "gcodes", "path": "gcodes", "permissions": "rw"}])

    @app.get("/server/files/list")
    def file_list(root: str = "gcodes"):
        if root != "gcodes":
            raise PrinterError("不存在此文件根目录", 404)
        return result(session.file_list())

    @app.get("/server/files/directory")
    def directory(path: str = "gcodes"):
        if path != "gcodes":
            raise PrinterError("当前虚拟根目录为 gcodes", 404)
        return result({"dirs": [], "files": [{**item, "filename": item["path"]} for item in session.file_list()],
                       "disk_usage": session.storage(), "root_info": {"name": "gcodes", "permissions": "rw"}})

    @app.get("/server/files/metadata")
    def metadata(filename: str):
        return result(session.metadata_for(filename))

    @app.get("/server/files/gcodes/{filename:path}")
    def download(filename: str):
        path = session.file_path(filename)
        if not path.is_file():
            raise PrinterError("文件不存在", 404)
        return FileResponse(path, filename=path.name, media_type="text/plain")

    @app.delete("/server/files/gcodes/{filename:path}")
    def delete(filename: str):
        session.delete_file(filename)
        return result({"item": {"path": filename, "root": "gcodes"}, "action": "delete_file"})

    @app.post("/server/files/upload")
    async def upload(request: Request):
        async with request.form(max_files=1, max_fields=8) as form:
            file = form.get("file")
            if form.get("root", "gcodes") != "gcodes" or not hasattr(file, "file"):
                raise PrinterError("需要 file 文件和 gcodes 根目录")
            name = str(file.filename or "print.gcode")
            folder = str(form.get("path", "")).strip("/")
            if folder:
                name = folder + "/" + name
            item = await asyncio.to_thread(session.accept_upload, file.file, name)
            started = False
            if str(form.get("print", "false")).lower() == "true":
                await asyncio.to_thread(session.start, item["path"])
                started = True
            # Moonraker upload has a bare response, unlike the other HTTP endpoints.
            return {"item": item, "print_started": started, "print_queued": False, "action": "create_file"}

    async def body(request):
        try:
            value = await request.json()
            if not isinstance(value, dict):
                raise ValueError()
            return value
        except (ValueError, UnicodeError) as exc:
            raise PrinterError("请求正文必须是 JSON 对象") from exc

    @app.post("/printer/print/start")
    async def start(request: Request):
        value = await body(request)
        params = value.get("params", {})
        if not isinstance(params, dict):
            raise PrinterError("params 必须为对象")
        filename = value.get("filename", params.get("filename", ""))
        await asyncio.to_thread(session.start, filename)
        return result("ok")

    @app.post("/printer/print/{action}")
    def print_action(action: str):
        actions = {"pause": session.pause, "resume": session.resume, "cancel": session.cancel}
        if action not in actions:
            raise PrinterError("未知打印操作", 404)
        actions[action]()
        return result("ok")

    @app.post("/printer/gcode/script")
    async def script(request: Request):
        value = await body(request)
        await asyncio.to_thread(session.script, value.get("script", ""))
        return result("ok")

    @app.get("/server/database/item")
    def database(namespace: str, key: str | None = None):
        if namespace != "lane_data":
            raise PrinterError("此命名空间不存在", 404)
        value = session.lane_data()
        if key:
            if key not in value:
                raise PrinterError("此料槽不存在", 404)
            value = value[key]
        return result({"namespace": namespace, "key": key, "value": value})

    @app.get("/server/webcams/list")
    def webcams(request: Request):
        origin = str(request.base_url).rstrip("/")
        return result({"webcams": [{"name": "Virtual Print Camera", "enabled": True, "service": "mjpegstreamer",
                                    "stream_url": origin + "/webcam/?action=stream",
                                    "snapshot_url": origin + "/webcam/?action=snapshot", "target_fps": 5}]})

    @app.get("/webcam/")
    async def webcam(request: Request, action: str = "snapshot"):
        if session.faults["camera_offline"]:
            raise PrinterError("模拟摄像头中断", 503)
        headers = {"Cache-Control": "no-store, no-cache, must-revalidate"}
        if action == "snapshot":
            return Response(camera.jpeg(), media_type="image/jpeg", headers=headers)
        if action != "stream":
            raise PrinterError("未知摄像头动作")

        async def frames():
            while not await request.is_disconnected():
                if session.faults["camera_offline"] or session.faults["offline"]:
                    break
                frame = camera.jpeg()
                yield b"--frame\r\nContent-Type: image/jpeg\r\nContent-Length: " + str(len(frame)).encode() + b"\r\n\r\n" + frame + b"\r\n"
                await asyncio.sleep(.2)
        return StreamingResponse(frames(), media_type="multipart/x-mixed-replace; boundary=frame", headers=headers)

    @app.websocket("/websocket")
    async def moonraker_websocket(ws: WebSocket):
        try:
            require_connection(ws, session)
        except PrinterError:
            await ws.close(code=1008)
            return
        if session.faults["offline"]:
            await ws.close(code=1013)
            return
        await ws.accept()
        try:
            while True:
                message = await ws.receive_json()
                request_id = message.get("id") if isinstance(message, dict) else None
                try:
                    if not isinstance(message, dict) or message.get("jsonrpc") != "2.0":
                        raise PrinterError("Invalid JSON-RPC request", -32600)
                    method = message.get("method")
                    params = message.get("params", {})
                    if not isinstance(params, dict):
                        raise PrinterError("Invalid params", -32602)
                    if method == "server.connection.identify":
                        value = {"connection_id": id(ws)}
                    elif method in {"printer.objects.subscribe", "printer.objects.query"}:
                        objects = params.get("objects", {})
                        if not isinstance(objects, dict):
                            raise PrinterError("objects must be an object", -32602)
                        value = {"status": filter_objects(session.objects(), objects), "eventtime": time.monotonic()}
                        # Send the complete initial state before registering for delta notifications.
                        await ws.send_json({"jsonrpc": "2.0", "id": request_id, "result": value})
                        if method == "printer.objects.subscribe":
                            clients[ws] = objects
                        continue
                    elif method == "printer.objects.list":
                        value = {"objects": list(session.objects())}
                    elif method == "server.info":
                        value = server_info()["result"]
                    elif method == "printer.gcode.script":
                        await asyncio.to_thread(session.script, params.get("script", ""))
                        value = "ok"
                    elif method in {"printer.print.start", "printer.print.pause", "printer.print.resume", "printer.print.cancel"}:
                        action = method.rsplit(".", 1)[1]
                        if action == "start":
                            await asyncio.to_thread(session.start, params.get("filename", ""))
                        else:
                            getattr(session, action)()
                        value = "ok"
                    else:
                        raise PrinterError("Method not found", -32601)
                    if request_id is not None:
                        await ws.send_json({"jsonrpc": "2.0", "id": request_id, "result": value})
                except PrinterError as exc:
                    await ws.send_json({"jsonrpc": "2.0", "id": request_id, "error": {"code": exc.status, "message": str(exc)}})
        except (WebSocketDisconnect, RuntimeError, ValueError):
            pass
        finally:
            clients.pop(ws, None)

    @app.get("/sim/api/v1/state")
    def state():
        return session.snapshot()

    @app.put("/sim/api/v1/config")
    async def config(request: Request):
        value = await body(request)
        await asyncio.to_thread(session.configure, value)
        return session.snapshot()

    @app.get("/sim/api/v1/config")
    def export_config():
        return JSONResponse(session.snapshot()["config"], headers={"Content-Disposition": 'attachment; filename="virtual-printer-profile.json"'})

    @app.post("/sim/api/v1/config/validate")
    async def validate_profile(request: Request):
        from .config import validate_config
        return validate_config(await body(request))

    @app.get("/sim/api/v1/templates")
    def templates():
        from .config import default_config
        return {name: default_config(name) for name in ("single", "multicolor", "multihead")}

    @app.post("/sim/api/v1/action")
    async def action(request: Request):
        value = await body(request)
        action = value.get("action")
        if action == "speed":
            session.set_speed(value.get("value"))
        elif action == "fault":
            session.set_fault(value.get("name"), value.get("enabled"))
        elif action == "recover":
            session.recover()
        elif action == "refill":
            session.refill(value.get("slot"), value.get("grams"))
        elif action == "demo":
            import io
            await asyncio.to_thread(session.accept_upload, io.BytesIO(demo_gcode()), "HF-Demo.gcode")
        elif action == "shutdown" and shutdown:
            asyncio.get_running_loop().call_later(.5, shutdown)
        else:
            raise PrinterError("未知中控操作")
        return session.snapshot()

    @app.websocket("/sim/events")
    async def console_events(ws: WebSocket):
        try:
            require_connection(ws, session, management=True)
        except PrinterError:
            await ws.close(code=1008)
            return
        await ws.accept()
        try:
            while True:
                await asyncio.wait_for(ws.send_json(session.snapshot()), timeout=2)
                await asyncio.sleep(.5)
        except (WebSocketDisconnect, RuntimeError, asyncio.TimeoutError):
            pass

    static = Path(__file__).parent / "web"
    app.mount("/assets", StaticFiles(directory=static), name="assets")

    @app.get("/")
    def home():
        return FileResponse(static / "index.html", headers={"Cache-Control": "no-cache"})

    return app
