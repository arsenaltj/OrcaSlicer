"""Exercise the real listener and frozen distribution; no in-process ASGI adapter."""
from __future__ import annotations

import io
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest

import httpx
from PIL import Image
from websockets.sync.client import connect
from websockets.exceptions import InvalidStatus


class RejectProxyHandler(BaseHTTPRequestHandler):
    def do_GET(self):
        self.server.requests.append(self.path)
        self.send_response(502)
        self.end_headers()

    def log_message(self, *_):
        pass


class NetworkTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="Honor Forge 网络验证 中文 ")
        cls.root = Path(cls.temp.name)
        cls.runtime = cls.root / "runtime.json"
        cls.command = ([os.environ["HF_VP_EXECUTABLE"]] if os.environ.get("HF_VP_EXECUTABLE") else
                       [sys.executable, str(Path(__file__).resolve().parents[1] / "run.py")])
        cls.command += ["--headless", "--no-browser", "--data-dir", str(cls.root), "--runtime-file", str(cls.runtime)]
        cls.environment = os.environ.copy()
        # The frozen application cannot rely on any developer executables or Python installation on PATH.
        if os.environ.get("HF_VP_EXECUTABLE") and os.name == "nt":
            cls.environment["PATH"] = str(Path(os.environ["SystemRoot"]) / "System32")
            cls.environment.pop("PYTHONPATH", None)
            cls.environment.pop("PYTHONHOME", None)
        cls.proxy = ThreadingHTTPServer(("127.0.0.1", 0), RejectProxyHandler)
        cls.proxy.requests = []
        cls.proxy_thread = threading.Thread(target=cls.proxy.serve_forever, kwargs={"poll_interval": .01})
        cls.proxy_thread.start()
        proxy_url = f"http://127.0.0.1:{cls.proxy.server_port}"
        cls.environment.update({"HTTP_PROXY": proxy_url, "http_proxy": proxy_url,
                                "ALL_PROXY": proxy_url, "all_proxy": proxy_url, "NO_PROXY": "", "no_proxy": ""})
        cls.blocker = socket.socket()
        try:
            cls.blocker.bind(("127.0.0.1", 7125))
            cls.blocker.listen()
        except OSError:
            cls.blocker.close()
        cls.process = subprocess.Popen(cls.command, env=cls.environment, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        deadline = time.monotonic() + 25
        while time.monotonic() < deadline and cls.process.poll() is None:
            try:
                cls.declaration = json.loads(cls.runtime.read_text(encoding="utf-8"))
                cls.base = f"http://127.0.0.1:{cls.declaration['port']}"
                cls.http = httpx.Client(base_url=cls.base, timeout=10, trust_env=False)
                if cls.http.get("/server/info").status_code == 200:
                    return
                cls.http.close()
            except (OSError, ValueError, httpx.HTTPError):
                pass
            time.sleep(.1)
        cls.process.terminate()
        cls.process.wait(timeout=10)
        cls.proxy.shutdown()
        cls.proxy.server_close()
        cls.proxy_thread.join(5)
        raise RuntimeError((cls.root / "virtual-printer.log").read_text(encoding="utf-8"))

    @classmethod
    def tearDownClass(cls):
        try:
            cls.http.post("/sim/api/v1/action", json={"action": "shutdown"})
            cls.process.wait(timeout=20)
        finally:
            cls.http.close()
            if cls.process.poll() is None:
                cls.process.terminate()
                cls.process.wait(timeout=10)
            cls.blocker.close()
            cls.proxy.shutdown()
            cls.proxy.server_close()
            cls.proxy_thread.join(5)
            cls.temp.cleanup()

    def tearDown(self):
        self.http.post("/sim/api/v1/action", json={"action": "recover"})
        self.http.post("/printer/print/cancel")

    def upload(self, name, content, printing=False):
        return self.http.post("/server/files/upload", data={"root": "gcodes", "print": str(printing).lower()},
                              files={"file": (name, content, "text/plain")})

    def test_port_conflict_identity_duplicate_start_and_offline_assets(self):
        self.assertGreater(self.declaration["port"], 7125)
        identity = self.http.get("/server/info").json()["result"]
        self.assertEqual(identity["instance_uuid"], self.declaration["instance_uuid"])
        duplicate = subprocess.run(self.command, env=self.environment, timeout=15, capture_output=True)
        self.assertEqual(duplicate.returncode, 0)
        # A Windows venv redirector may launch the actual interpreter as a child process.
        self.assertEqual(json.loads(self.runtime.read_text(encoding="utf-8"))["pid"], self.declaration["pid"])
        self.assertEqual(self.proxy.requests, [])
        for path in ["/", "/assets/app.js", "/assets/style.css"]:
            self.assertEqual(self.http.get(path).status_code, 200)

    def test_untrusted_host_and_origin_are_rejected_by_the_real_listener(self):
        saved = (self.root / "config.json").read_bytes()
        host = f"rebind.example:{self.declaration['port']}"
        headers = {"Host": host, "Origin": f"http://{host}"}
        for path in ["/", "/sim/api/v1/config", "/server/info", "/webcam/?action=snapshot"]:
            self.assertEqual(self.http.get(path, headers=headers).status_code, 403)
        config = self.http.get("/sim/api/v1/config").json()
        config["name"] = "must-not-be-written"
        self.assertEqual(self.http.put("/sim/api/v1/config", json=config, headers=headers).status_code, 403)
        self.assertEqual((self.root / "config.json").read_bytes(), saved)
        for path in ["/websocket", "/sim/events"]:
            with self.subTest(path=path):
                with socket.create_connection(("127.0.0.1", self.declaration["port"])) as transport:
                    with self.assertRaises(InvalidStatus) as rejected:
                        with connect(f"ws://{host}{path}", sock=transport, origin=f"http://{host}", proxy=None):
                            pass
                    self.assertEqual(rejected.exception.response.status_code, 403)
        self.assertEqual(self.http.get("/server/info", headers={"Origin": "null"}).status_code, 403)
        with connect(self.base.replace("http", "ws", 1) + "/sim/events", origin=self.base, proxy=None) as ws:
            self.assertEqual(json.loads(ws.recv(timeout=5))["instance_uuid"], self.declaration["instance_uuid"])

    def test_invalid_colors_never_persist_over_real_http(self):
        saved = (self.root / "config.json").read_bytes()
        config = self.http.get("/sim/api/v1/config").json()
        for color in ["#+00000", "#-00001", "#00_000", "# 00000"]:
            with self.subTest(color=color):
                config["slots"][0]["color"] = color
                self.assertEqual(self.http.post("/sim/api/v1/config/validate", json=config).status_code, 400)
                self.assertEqual(self.http.put("/sim/api/v1/config", json=config).status_code, 400)
                self.assertEqual((self.root / "config.json").read_bytes(), saved)
        self.assertEqual(self.http.get("/webcam/?action=snapshot").status_code, 200)

    def test_real_websocket_notifications_camera_and_print_controls(self):
        self.assertEqual(self.upload("网络联调.gcode", b"G28\nM104 S180\nG1 X100 Y100 Z.2 E3 F600\nG4 S600\n").status_code, 200)
        with connect(self.base.replace("http", "ws", 1) + "/websocket", proxy=None) as ws:
            ws.send(json.dumps({"jsonrpc": "2.0", "id": 1, "method": "printer.objects.subscribe",
                                "params": {"objects": {"print_stats": None, "toolhead": None}}}))
            self.assertIn("status", json.loads(ws.recv(timeout=5))["result"])
            before = self.http.get("/webcam/?action=snapshot").content
            self.assertEqual(self.http.post("/printer/print/start", json={"filename": "网络联调.gcode"}).status_code, 200)
            for _ in range(15):
                message = json.loads(ws.recv(timeout=5))
                if message.get("method") == "notify_status_update" and message["params"][0]["toolhead"]["position"][0] > 0:
                    break
            else:
                self.fail("No moving status notification received")
            self.assertEqual(message["params"][0]["print_stats"]["state"], "printing")
            after = self.http.get("/webcam/?action=snapshot").content
            self.assertNotEqual(before, after)
            self.assertEqual(Image.open(io.BytesIO(after)).size, (640, 360))
            self.assertEqual(self.http.post("/printer/print/pause").status_code, 200)
            paused = self.http.get("/sim/api/v1/state").json()["position"]
            # Wait for a real event, with the page and snapshots completely closed.
            ws.recv(timeout=5)
            self.assertEqual(self.http.get("/sim/api/v1/state").json()["position"], paused)
            self.assertEqual(self.http.post("/printer/print/resume").status_code, 200)
            self.assertEqual(self.upload("忙碌时仅上传.gcode", b"G28\n").status_code, 200)
            self.assertEqual(self.http.post("/printer/print/start", json={"filename": "网络联调.gcode"}).status_code, 409)
            self.assertEqual(self.http.post("/printer/print/cancel").status_code, 200)
        self.assertTrue(self.http.get("/sim/api/v1/state").json()["history"])

    def test_fault_recovery_and_camera_stream(self):
        for fault, expected in [("upload_failed", 503), ("storage_full", 507)]:
            self.http.post("/sim/api/v1/action", json={"action": "fault", "name": fault, "enabled": True})
            self.assertEqual(self.upload("失败.gcode", b"G28\n").status_code, expected)
            self.http.post("/sim/api/v1/action", json={"action": "recover"})
        self.http.post("/sim/api/v1/action", json={"action": "fault", "name": "offline", "enabled": True})
        self.assertEqual(self.http.get("/server/info").status_code, 503)
        self.assertEqual(self.http.get("/sim/api/v1/state").status_code, 200)
        self.http.post("/sim/api/v1/action", json={"action": "recover"})
        with self.http.stream("GET", "/webcam/?action=stream") as response:
            self.assertIn("multipart/x-mixed-replace", response.headers["content-type"])
            self.assertIn(b"--frame", next(response.iter_bytes()))
        self.assertEqual(self.http.get("/server/info").status_code, 200)

    def test_large_streaming_file_and_aborted_http_upload(self):
        with tempfile.TemporaryFile() as file:
            for _ in range(32):
                file.write(b"; large bounded-memory file\n" * 32000)
            size = file.tell()
            file.seek(0)
            response = self.upload("大文件.gcode", file)
        self.assertEqual(response.status_code, 200)
        self.assertEqual(response.json()["item"]["size"], size)
        self.assertEqual(self.http.get("/server/files/metadata?filename=大文件.gcode").json()["result"]["size"], size)
        with socket.create_connection(("127.0.0.1", self.declaration["port"])) as connection:
            body = b'--interrupted\r\nContent-Disposition: form-data; name="file"; filename="aborted.gcode"\r\n\r\nG28\n'
            headers = (f"POST /server/files/upload HTTP/1.1\r\nHost: 127.0.0.1:{self.declaration['port']}\r\n"
                       "Content-Type: multipart/form-data; boundary=interrupted\r\nContent-Length: 999999\r\n\r\n").encode()
            connection.sendall(headers + body)
        names = [item["path"] for item in self.http.get("/server/files/list").json()["result"]]
        self.assertNotIn("aborted.gcode", names)
        self.assertEqual(self.http.get("/sim/api/v1/state").status_code, 200)


if __name__ == "__main__":
    unittest.main(verbosity=2)
