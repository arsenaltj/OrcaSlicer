from __future__ import annotations

import json
from io import BytesIO
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
import unittest
import urllib.request

from PIL import Image


TOOLS_AI = Path(__file__).resolve().parent
BOOTSTRAP = TOOLS_AI / "orca_ai_installed_bootstrap.py"
LOCAL_OPENER = urllib.request.build_opener(urllib.request.ProxyHandler({}))


def _free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.bind(("127.0.0.1", 0))
        return int(listener.getsockname()[1])


def _multipart_image() -> tuple[bytes, str]:
    boundary = "----OrcaDiagnosticFailure"
    parts: list[bytes] = []
    fields = {
        "request_id": "offline-diagnostic-test",
        "instruction": "preserve the subject",
        "style": "sculpture",
        "custom_style": "",
        "palette": "[]",
        "palette_roles": "{}",
        "print": "{}",
    }
    for name, value in fields.items():
        parts.append(
            f"--{boundary}\r\nContent-Disposition: form-data; name=\"{name}\"\r\n\r\n{value}\r\n".encode()
        )
    parts.append(
        f"--{boundary}\r\nContent-Disposition: form-data; name=\"image\"; filename=\"input.png\"\r\n"
        "Content-Type: image/png\r\n\r\n".encode()
        + _valid_png_bytes()
        + b"\r\n"
    )
    parts.append(f"--{boundary}--\r\n".encode())
    return b"".join(parts), boundary


def _valid_png_bytes() -> bytes:
    output = BytesIO()
    Image.new("RGB", (96, 96), (230, 230, 230)).save(output, format="PNG")
    return output.getvalue()


class DiagnosticFailureFlowTests(unittest.TestCase):
    def test_installed_sidecar_correlates_connection_failure_with_job_id(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            data_dir = Path(directory)
            port = _free_port()
            environment = dict(os.environ)
            environment.update(
                OPENAI_API_KEY="test-openai",
                TRIPO_API_KEY="test-tripo",
                OPENAI_BASE_URL="https://127.0.0.1:1/v1",
                ORCASLICER_AI_SIDECAR_HOST="127.0.0.1",
                ORCASLICER_AI_SIDECAR_PORT=str(port),
                ORCASLICER_AI_OUTPUT_DIR=str(data_dir / "generated_models"),
                NO_PROXY="127.0.0.1,localhost",
                no_proxy="127.0.0.1,localhost",
            )
            process = subprocess.Popen(
                [sys.executable, str(BOOTSTRAP), str(data_dir)],
                cwd=TOOLS_AI,
                env=environment,
            )
            try:
                health_url = f"http://127.0.0.1:{port}/health"
                deadline = time.time() + 10
                while True:
                    try:
                        with LOCAL_OPENER.open(health_url, timeout=1) as response:
                            self.assertEqual(response.status, 200)
                            break
                    except OSError:
                        if process.poll() is not None or time.time() >= deadline:
                            log_path = data_dir / "log" / "orca-ai-sidecar.log"
                            detail = log_path.read_text(encoding="utf-8") if log_path.is_file() else "log missing"
                            self.fail(f"Installed diagnostic Sidecar did not become healthy:\n{detail}")
                        time.sleep(0.05)

                body, boundary = _multipart_image()
                create = urllib.request.Request(
                    f"http://127.0.0.1:{port}/v1/orcaslicer/model-jobs/image",
                    data=body,
                    method="POST",
                    headers={
                        "X-OrcaSlicer-Client": "native",
                        "Content-Type": f"multipart/form-data; boundary={boundary}",
                    },
                )
                with LOCAL_OPENER.open(create, timeout=5) as response:
                    job_id = json.loads(response.read())["job"]["id"]

                status_url = f"http://127.0.0.1:{port}/v1/orcaslicer/model-jobs/{job_id}"
                deadline = time.time() + 10
                state = ""
                while time.time() < deadline:
                    status = urllib.request.Request(status_url, headers={"X-OrcaSlicer-Client": "native"})
                    with LOCAL_OPENER.open(status, timeout=2) as response:
                        job = json.loads(response.read())["job"]
                    state = job["state"]
                    if state == "failed":
                        break
                    time.sleep(0.05)
                self.assertEqual(state, "failed")
                self.assertEqual(job["message"], "Could not connect to the preprocessing service.")

                log_path = data_dir / "log" / "orca-ai-sidecar.log"
                events = [
                    json.loads(line)
                    for line in log_path.read_text(encoding="utf-8").splitlines()
                    if line.startswith("{")
                ]
                failures = [event for event in events if event.get("event") == "provider.connection.failed"]
                self.assertEqual(len(failures), 1)
                self.assertEqual(failures[0]["job_id"], job_id)
                self.assertEqual(failures[0]["failure_kind"], "connection_refused")
                self.assertEqual(failures[0]["endpoint"], "https://127.0.0.1:1/v1/images/edits")
                serialized = json.dumps(events)
                self.assertNotIn("test-openai", serialized)
                self.assertNotIn("test-tripo", serialized)
                self.assertNotIn("preserve the subject", serialized)
            finally:
                if process.poll() is None:
                    process.terminate()
                process.wait(timeout=10)


if __name__ == "__main__":
    unittest.main()
