#!/usr/bin/env python3
import contextlib
import json
import shutil
import socket
import subprocess
import threading
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


CHECKER = Path(__file__).with_name("check_sidecar_capability.ps1")
POWERSHELL = shutil.which("powershell.exe") or shutil.which("powershell")


def handler_for(payload):
    class HealthHandler(BaseHTTPRequestHandler):
        def do_GET(self):
            if self.path != "/health":
                self.send_error(404)
                return
            body = json.dumps(payload).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, *_args):
            pass

    return HealthHandler


@contextlib.contextmanager
def health_server(payload):
    server = ThreadingHTTPServer(("127.0.0.1", 0), handler_for(payload))
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield f"http://127.0.0.1:{server.server_address[1]}"
    finally:
        server.shutdown()
        server.server_close()
        thread.join()


def unused_loopback_endpoint():
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        port = listener.getsockname()[1]
    return f"http://127.0.0.1:{port}"


@unittest.skipUnless(POWERSHELL, "PowerShell is required")
class SidecarReadinessTests(unittest.TestCase):
    def run_checker(self, endpoint, expected_version=""):
        command = [
            POWERSHELL,
            "-NoProfile",
            "-ExecutionPolicy",
            "Bypass",
            "-File",
            str(CHECKER),
            "-Endpoint",
            endpoint,
        ]
        if expected_version:
            command.extend(("-ExpectedSidecarVersion", expected_version))
        result = subprocess.run(
            command,
            check=False,
            capture_output=True,
            text=True,
            timeout=10,
        )
        return result.returncode

    def ready_health(self):
        return {
            "ok": True,
            "protocol_version": 1,
            "sidecar_version": "orcaslicer-ai-sidecar-v4",
            "capabilities": {
                "model_generation": {
                    "available": True,
                    "sources": ["text", "image"],
                    "artifact_formats": ["obj"],
                }
            },
        }

    def test_ready_generation_capability_returns_zero(self):
        with health_server(self.ready_health()) as endpoint:
            self.assertEqual(self.run_checker(endpoint), 0)

    def test_unavailable_generation_capability_returns_two(self):
        health = self.ready_health()
        health["capabilities"]["model_generation"]["available"] = False
        with health_server(health) as endpoint:
            self.assertEqual(self.run_checker(endpoint), 2)

    def test_incompatible_protocol_returns_two(self):
        health = self.ready_health()
        health["protocol_version"] = 2
        with health_server(health) as endpoint:
            self.assertEqual(self.run_checker(endpoint), 2)

    def test_matching_sidecar_version_returns_zero(self):
        with health_server(self.ready_health()) as endpoint:
            self.assertEqual(self.run_checker(endpoint, "orcaslicer-ai-sidecar-v4"), 0)

    def test_stale_sidecar_version_requests_restart(self):
        health = self.ready_health()
        health["sidecar_version"] = "orcaslicer-ai-sidecar-v2"
        with health_server(health) as endpoint:
            self.assertEqual(self.run_checker(endpoint, "orcaslicer-ai-sidecar-v4"), 3)

    def test_unreachable_sidecar_returns_one(self):
        self.assertEqual(self.run_checker(unused_loopback_endpoint()), 1)


if __name__ == "__main__":
    unittest.main()
