"""Readiness must identify the local listener without using a configured proxy."""
from __future__ import annotations

from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import sys
import tempfile
import threading
import unittest
from unittest.mock import patch
import urllib.request

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from hf_virtual_printer.launcher import existing_endpoint


class ProbeHandler(BaseHTTPRequestHandler):
    def do_GET(self):
        self.server.requests.append(self.path)
        if self.server.redirect:
            self.send_response(302)
            self.send_header("Location", self.server.redirect)
            self.end_headers()
        else:
            content = json.dumps(self.server.identity).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(content)))
            self.end_headers()
            self.wfile.write(content)

    def log_message(self, *_):
        pass


class LauncherProbeTests(unittest.TestCase):
    def start_server(self, port):
        server = ThreadingHTTPServer(("127.0.0.1", port), ProbeHandler)
        server.requests, server.redirect = [], None
        server.identity = {"result": {"hf_virtual_printer": True, "instance_uuid": "probe-identity"}}
        thread = threading.Thread(target=server.serve_forever, kwargs={"poll_interval": .01})
        thread.start()
        self.addCleanup(thread.join, 5)
        self.addCleanup(server.server_close)
        self.addCleanup(server.shutdown)
        return server

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="HF 本机探测 ")
        self.addCleanup(self.temp.cleanup)
        self.runtime = Path(self.temp.name) / "runtime.json"
        for port in range(7125, 7136):
            try:
                self.server = self.start_server(port)
                break
            except OSError:
                continue
        else:
            self.fail("No isolated listener port available")
        self.declaration = {"schema": 1, "host": "127.0.0.1", "port": port, "instance_uuid": "probe-identity"}
        self.endpoint = f"http://127.0.0.1:{port}"
        self.save_declaration(self.declaration)

    def save_declaration(self, value):
        self.runtime.write_text(json.dumps(value), encoding="utf-8")

    def test_proxy_environment_cannot_intercept_the_loopback_probe(self):
        proxy = self.start_server(0)
        proxy.identity = {"error": "proxy must not receive a local probe"}
        proxy_url = f"http://127.0.0.1:{proxy.server_port}"
        environment = {"HTTP_PROXY": proxy_url, "http_proxy": proxy_url,
                       "ALL_PROXY": proxy_url, "all_proxy": proxy_url,
                       "NO_PROXY": "", "no_proxy": ""}
        global_opener = urllib.request.build_opener(urllib.request.ProxyHandler({"http": proxy_url}))
        with patch.dict(os.environ, environment), patch.object(urllib.request, "_opener", global_opener), \
                patch.object(urllib.request, "proxy_bypass", return_value=False):
            self.assertEqual(existing_endpoint(self.runtime), self.endpoint)
            self.assertIs(urllib.request._opener, global_opener)
        self.assertEqual(proxy.requests, [])
        self.assertEqual(self.server.requests, ["/server/info"])

    def test_probe_does_not_follow_a_redirect_to_another_listener(self):
        target = self.start_server(0)
        self.server.redirect = f"http://127.0.0.1:{target.server_port}/identity"
        self.assertIsNone(existing_endpoint(self.runtime))
        self.assertEqual(target.requests, [])

    def test_probe_requires_matching_virtual_identity(self):
        for identity in [{"result": {"hf_virtual_printer": True, "instance_uuid": "wrong"}},
                         {"result": {"hf_virtual_printer": False, "instance_uuid": "probe-identity"}},
                         {"result": []}, [], {"result": None}, {}]:
            with self.subTest(identity=identity):
                self.server.identity = identity
                self.assertIsNone(existing_endpoint(self.runtime))
        self.server.identity = {"result": {"hf_virtual_printer": True, "instance_uuid": "probe-identity"}}
        self.assertEqual(existing_endpoint(self.runtime), self.endpoint)

    def test_invalid_runtime_declarations_do_not_make_requests(self):
        for value in [[], None, {}, {**self.declaration, "schema": True},
                      {**self.declaration, "host": "rebind.example"},
                      {**self.declaration, "port": 80}, {**self.declaration, "port": "7125"},
                      {**self.declaration, "instance_uuid": ""}]:
            with self.subTest(value=value):
                self.save_declaration(value)
                self.assertIsNone(existing_endpoint(self.runtime))
        self.assertEqual(self.server.requests, [])

    def test_missing_file_and_unavailable_listener_are_not_ready(self):
        self.runtime.unlink()
        self.assertIsNone(existing_endpoint(self.runtime))
        self.save_declaration(self.declaration)
        self.server.shutdown()
        self.server.server_close()
        self.assertIsNone(existing_endpoint(self.runtime))


if __name__ == "__main__":
    unittest.main(verbosity=2)
