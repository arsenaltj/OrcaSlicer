"""One user-level background instance; no installed Python or admin service needed."""
from __future__ import annotations

import argparse
import contextlib
import ctypes
import hashlib
import json
import logging
from logging.handlers import RotatingFileHandler
import os
from pathlib import Path
import socket
import sys
import threading
import time
import urllib.request
import webbrowser

import uvicorn

from . import __version__
from .config import data_directory, write_json
from .desktop import Advertisement, set_autorun, tray_icon
from .server import create_app
from .session import VirtualPrinterSession


class InstanceLock:
    def __init__(self, directory):
        self.handle = None
        if os.name == "nt":
            name = "Local\\HonorForgeVirtualPrinter-" + hashlib.sha256(str(directory.resolve()).casefold().encode()).hexdigest()[:20]
            kernel = ctypes.WinDLL("kernel32", use_last_error=True)
            kernel.CreateMutexW.argtypes = [ctypes.c_void_p, ctypes.c_bool, ctypes.c_wchar_p]
            kernel.CreateMutexW.restype = ctypes.c_void_p
            self.handle = kernel.CreateMutexW(None, False, name)
            self.existing = ctypes.get_last_error() == 183
            if not self.handle:
                raise OSError("无法创建本机实例锁")
        else:
            self.existing = False

    def close(self):
        if self.handle:
            kernel = ctypes.WinDLL("kernel32", use_last_error=True)
            kernel.CloseHandle.argtypes = [ctypes.c_void_p]
            kernel.CloseHandle(self.handle)
            self.handle = None


class LocalProbeRedirectHandler(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        # A response from a different endpoint cannot establish this listener's readiness.
        return None


def existing_endpoint(runtime_file):
    try:
        declaration = json.loads(runtime_file.read_text(encoding="utf-8"))
        if not isinstance(declaration, dict):
            return None
        port = declaration["port"]
        identity_id = declaration.get("instance_uuid")
        if (type(declaration.get("schema")) is not int or declaration["schema"] != 1 or
                declaration.get("host") != "127.0.0.1" or type(port) is not int or not 7125 <= port <= 7135 or
                not isinstance(identity_id, str) or not identity_id):
            return None
        endpoint = f"http://127.0.0.1:{port}"
        # Keep the proxy policy local to this request; do not change the global opener or user settings.
        opener = urllib.request.build_opener(urllib.request.ProxyHandler({}), LocalProbeRedirectHandler())
        with opener.open(endpoint + "/server/info", timeout=1) as response:
            payload = json.load(response)
        identity = payload.get("result") if isinstance(payload, dict) else None
        if isinstance(identity, dict) and identity.get("hf_virtual_printer") is True and identity.get("instance_uuid") == identity_id:
            return endpoint
    except (OSError, ValueError, KeyError):
        pass
    return None


class Application:
    def __init__(self, args):
        self.args = args
        self.session = VirtualPrinterSession(args.data_dir)
        self.runtime_file = args.runtime_file or args.data_dir / "runtime.json"
        self.stopping = threading.Event()
        self.ready = threading.Event()
        self.server = None
        self.port = args.port
        self.binding_lan = self.session.config["lan_enabled"]
        self.advertisement = Advertisement()
        self.icon = None
        self.error = None

    def declaration(self, state="ready"):
        return {"schema": 1, "host": "127.0.0.1", "port": self.port, "instance_uuid": self.session.instance_uuid,
                "pid": os.getpid(), "state": state, "version": __version__, "executable": sys.executable}

    def open_console(self):
        webbrowser.open(f"http://127.0.0.1:{self.port}/")

    def stop(self):
        self.stopping.set()
        if self.server:
            self.server.should_exit = True
        if self.icon:
            self.icon.stop()

    def preferences_changed(self):
        try:
            # Isolated QA instances must not change the installed user's startup entry.
            if self.args.data_dir.resolve() == data_directory().resolve():
                set_autorun(self.session.config["autorun"])
            elif self.session.config["autorun"]:
                raise RuntimeError("隔离数据目录不支持开机启动，请使用默认用户数据目录")
        except Exception as exc:
            self.session.config["autorun"] = False
            self.session.persist()
            self.session.log("system", str(exc))
        if self.binding_lan != self.session.config["lan_enabled"]:
            if self.server:
                self.server.should_exit = True
        else:
            try:
                self.advertisement.update(self.session, self.port)
            except Exception as exc:
                self.session.log("system", "局域网广播失败，可使用显示的 IP 手动连接：" + str(exc))

    def run_server(self):
        try:
            while not self.stopping.is_set():
                self.binding_lan = self.session.config["lan_enabled"]
                host = "0.0.0.0" if self.binding_lan else "127.0.0.1"
                listener = None
                for port in [self.port] + [p for p in range(7125, 7136) if p != self.port]:
                    candidate = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                    if os.name == "nt":
                        candidate.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
                    try:
                        candidate.bind((host, port))
                        candidate.listen(128)
                        listener, self.port = candidate, port
                        break
                    except OSError:
                        candidate.close()
                if listener is None:
                    raise RuntimeError("7125–7135 端口均被占用，请退出多余的虚拟打印机实例")
                self.session.endpoint = f"http://127.0.0.1:{self.port}"
                self.preferences_changed()
                write_json(self.runtime_file, self.declaration())
                app = create_app(self.session, network_changed=self.preferences_changed, shutdown=self.stop)
                self.server = uvicorn.Server(uvicorn.Config(app, host=host, port=self.port, log_config=None,
                                                           access_log=False, loop="asyncio", ws="websockets", lifespan="on"))
                self.ready.set()
                self.server.run(sockets=[listener])
                listener.close()
                self.advertisement.close()
        except Exception as exc:
            self.error = exc
            logging.exception("Virtual printer stopped")
            self.ready.set()
            self.stop()
            if self.icon:
                self.icon.stop()
        finally:
            self.session.close()
            with contextlib.suppress(OSError, ValueError):
                if json.loads(self.runtime_file.read_text(encoding="utf-8")).get("pid") == os.getpid():
                    self.runtime_file.unlink()

    def run(self):
        thread = threading.Thread(target=self.run_server, name="virtual-printer-http", daemon=True)
        thread.start()
        if not self.ready.wait(20) or self.error:
            raise RuntimeError(str(self.error or "后台启动超时，请查看日志"))
        # Readiness is verified over HTTP, not inferred from the worker thread starting.
        for _ in range(50):
            if existing_endpoint(self.runtime_file):
                break
            if self.error:
                raise self.error
            time.sleep(.1)
        else:
            self.stop()
            raise RuntimeError("后台未通过 HTTP 就绪检查")
        if not self.args.no_browser:
            self.open_console()
        try:
            if self.args.headless:
                while thread.is_alive():
                    thread.join(.5)
            else:
                self.icon = tray_icon(self.open_console, self.stop)
                self.icon.run()
        except KeyboardInterrupt:
            self.stop()
        finally:
            self.stop()
            thread.join(15)
        return 1 if self.error else 0


def main(argv=None):
    parser = argparse.ArgumentParser(description="Honor Forge Virtual Printer")
    parser.add_argument("--version", action="version", version=__version__)
    parser.add_argument("--headless", action="store_true", help="仅运行后台，不创建托盘")
    parser.add_argument("--no-browser", action="store_true", help="不自动打开浏览器")
    parser.add_argument("--check-running", action="store_true", help="安装器检查：运行中返回 2，不启动服务")
    parser.add_argument("--data-dir", type=Path, default=data_directory())
    parser.add_argument("--runtime-file", type=Path, help="隔离联调时覆盖本机发现声明位置")
    parser.add_argument("--port", type=int, default=7125, choices=range(7125, 7136))
    args = parser.parse_args(argv)
    if args.check_running:
        lock = InstanceLock(args.data_dir)
        try:
            return 2 if lock.existing else 0
        finally:
            lock.close()
    args.data_dir.mkdir(parents=True, exist_ok=True)
    handler = RotatingFileHandler(args.data_dir / "virtual-printer.log", maxBytes=2 * 1024 * 1024, backupCount=2, encoding="utf-8")
    logging.basicConfig(level=logging.INFO, handlers=[handler], format="%(asctime)s %(levelname)s %(message)s")
    lock = InstanceLock(args.data_dir)
    try:
        if lock.existing:
            if not args.no_browser:
                endpoint = existing_endpoint(args.runtime_file or args.data_dir / "runtime.json")
                if endpoint:
                    webbrowser.open(endpoint)
            return 0
        return Application(args).run()
    except Exception as exc:
        logging.exception("Startup failed")
        if os.name == "nt" and not args.headless:
            ctypes.windll.user32.MessageBoxW(None, f"{exc}\n\n日志：{args.data_dir / 'virtual-printer.log'}", "Honor Forge 虚拟打印机", 0x10)
        return 1
    finally:
        lock.close()
