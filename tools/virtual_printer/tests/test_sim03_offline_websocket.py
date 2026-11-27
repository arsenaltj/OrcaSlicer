"""Real TCP/uvicorn offline handshakes; never the user's launcher or data directory."""
from __future__ import annotations

import json
import os
from pathlib import Path
import socket
import sys
import tempfile
import threading
import time
import unittest

import httpx
import uvicorn
from websockets.exceptions import ConnectionClosed, InvalidStatus
from websockets.sync.client import connect

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from hf_virtual_printer.server import create_app
from hf_virtual_printer.session import VirtualPrinterSession


class OfflineWebSocketNetworkTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="HF Sim03 private ")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.session = VirtualPrinterSession(self.root)
        self.changed = threading.Event()
        self.listener = socket.socket()
        if os.name == "nt":
            self.listener.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen(128)
        self.port = self.listener.getsockname()[1]
        self.base = f"http://127.0.0.1:{self.port}"
        self.session.endpoint = self.base
        self.server = uvicorn.Server(uvicorn.Config(
            create_app(self.session, network_changed=self.changed.set),
            host="127.0.0.1", port=self.port, log_config=None,
            access_log=False, loop="asyncio", ws="websockets", lifespan="on"))
        self.thread = threading.Thread(target=self.server.run, kwargs={"sockets": [self.listener]})
        self.addCleanup(self.stop_listener)
        self.thread.start()
        self.http = httpx.Client(base_url=self.base, timeout=3, trust_env=False)
        self.addCleanup(self.http.close)
        deadline = time.monotonic() + 5
        while not self.server.started:
            self.assertTrue(self.thread.is_alive(), "Private listener stopped before readiness")
            if time.monotonic() >= deadline:
                self.fail("Private listener readiness timed out")
            time.sleep(.005)
        self.assertEqual(self.http.get("/server/info").status_code, 200)
        self.wire("listener_ready", port=self.port, pid=os.getpid())

    def stop_listener(self):
        self.server.should_exit = True
        self.thread.join(8)
        self.listener.close()
        self.assertFalse(self.thread.is_alive(), "Owned private listener did not exit normally")
        with socket.socket() as probe:
            self.assertNotEqual(probe.connect_ex(("127.0.0.1", self.port)), 0,
                                "Owned private port is still listening")
        self.wire("listener_stopped", port=self.port, pid=os.getpid())

    def wire(self, event, **fields):
        print("sim03-wire " + json.dumps({"test": self.id(), "event": event, **fields}), flush=True)

    def offline(self):
        self.changed.clear()
        response = self.http.post("/sim/api/v1/action", json={"action": "fault", "name": "offline", "enabled": True})
        self.assertEqual(response.status_code, 200)
        self.assertTrue(self.changed.wait(3), "Engine didn't apply the offline generation")
        self.assertEqual(self.http.get("/server/info").status_code, 503)
        self.assertEqual(self.http.get("/sim/api/v1/state").status_code, 200)

    def subscribed(self, ws):
        request = {"jsonrpc": "2.0", "id": 1, "method": "printer.objects.subscribe",
                   "params": {"objects": {"print_stats": None, "extruder": None}}}
        ws.send(json.dumps(request))
        initial = json.loads(ws.recv(timeout=3))
        self.assertEqual(initial.get("jsonrpc"), "2.0")
        self.assertEqual(initial.get("id"), 1)
        self.assertIn("state", initial["result"]["status"]["print_stats"])
        self.wire("subscription_snapshot", handshakeStatus=ws.response.status_code, response=initial)
        return initial

    def test_authorized_offline_new_connections_upgrade_then_close_1013(self):
        self.offline()
        for attempt in range(2):
            try:
                with connect(self.base.replace("http:", "ws:") + "/websocket", proxy=None,
                             open_timeout=3, close_timeout=1) as ws:
                    self.assertEqual(ws.response.status_code, 101)
                    with self.assertRaises(ConnectionClosed) as closed:
                        ws.recv(timeout=3)
                    self.assertIsNotNone(closed.exception.rcvd)
                    self.wire("offline_handshake_closed", attempt=attempt + 1,
                              handshakeStatus=ws.response.status_code, closeCode=closed.exception.rcvd.code)
                    self.assertEqual(closed.exception.rcvd.code, 1013)
            except InvalidStatus as rejected:
                self.wire("offline_handshake_rejected", attempt=attempt + 1,
                          handshakeStatus=rejected.response.status_code)
                raise

    def test_existing_offline_and_recovered_subscription_do_not_replay_actions(self):
        before = self.http.get("/sim/api/v1/state").json()
        files_before = self.session.file_list()
        with connect(self.base.replace("http:", "ws:") + "/websocket", proxy=None,
                     open_timeout=3, close_timeout=1) as ws:
            self.subscribed(ws)
            self.offline()
            deadline = time.monotonic() + 3
            with self.assertRaises(ConnectionClosed) as closed:
                while True:
                    ws.recv(timeout=max(.001, deadline - time.monotonic()))
            self.assertIsNotNone(closed.exception.rcvd)
            self.assertIn(closed.exception.rcvd.code, (1012, 1013))
            self.wire("existing_offline_closed", closeCode=closed.exception.rcvd.code)
        self.changed.clear()
        self.assertEqual(self.http.post("/sim/api/v1/action", json={"action": "recover"}).status_code, 200)
        self.assertTrue(self.changed.wait(3), "Engine didn't apply the recovered generation")
        with connect(self.base.replace("http:", "ws:") + "/websocket", proxy=None,
                     open_timeout=3, close_timeout=1) as ws:
            self.subscribed(ws)
            for _ in range(2):
                message = json.loads(ws.recv(timeout=3))
                self.assertEqual(message.get("jsonrpc"), "2.0")
                self.assertEqual(message.get("method"), "notify_status_update")
                self.assertIn("print_stats", message["params"][0])
                self.wire("recovered_notification", response=message)
        after = self.http.get("/sim/api/v1/state").json()
        self.assertEqual(after["history"], before["history"])
        self.assertEqual(after["job"], before["job"])
        self.assertEqual(self.session.file_list(), files_before)
        api = [event["message"] for event in after["events"] if event["category"] == "api"]
        self.assertFalse(any(value.startswith(("POST /printer/", "POST /server/files/upload")) for value in api))
        self.wire("no_business_replay", apiCalls=api, historyUnchanged=True, jobUnchanged=True, filesUnchanged=True)

    def test_offline_does_not_relax_host_origin_lan_or_management_rejections(self):
        self.offline()
        saved = (self.root / "config.json").read_bytes()
        for host, origin in [(f"rebind.example:{self.port}", f"http://rebind.example:{self.port}"),
                             (f"192.0.2.2:{self.port}", f"http://192.0.2.2:{self.port}"),
                             (f"127.0.0.1:{self.port}", "https://unrelated.invalid")]:
            for path in ("/websocket", "/sim/events"):
                with self.subTest(host=host, path=path):
                    with socket.create_connection(("127.0.0.1", self.port), timeout=3) as transport:
                        with self.assertRaises(InvalidStatus) as rejected:
                            with connect(f"ws://{host}{path}", sock=transport, origin=origin,
                                         proxy=None, open_timeout=3, close_timeout=1):
                                pass
                        self.assertEqual(rejected.exception.response.status_code, 403)
                        self.wire("unauthorized_handshake_rejected", host=host, path=path, handshakeStatus=403)
        response = self.http.post("/sim/api/v1/action", json={"action": "recover"},
                                  headers={"Host": f"192.0.2.2:{self.port}"})
        self.assertEqual(response.status_code, 403)
        self.assertTrue(self.session.faults["offline"])
        self.assertEqual((self.root / "config.json").read_bytes(), saved)
        self.wire("management_rejected", httpStatus=response.status_code, configUnchanged=True)


if __name__ == "__main__":
    unittest.main(verbosity=2)
