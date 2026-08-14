#!/usr/bin/env python3
import contextlib
import importlib.util
import sys
import tempfile
import threading
import unittest
from http.server import ThreadingHTTPServer
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
TOOLS_AI = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS_AI))


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


MOCK = load_module("orca_ai_sidecar_mock_smoke", ROOT / "ai_sidecar_mock.py")

from smoke_model_generation import (  # noqa: E402
    ModelGenerationSmokeClient,
    PaidCallConfirmationRequired,
)


@contextlib.contextmanager
def mock_sidecar():
    server = ThreadingHTTPServer(("127.0.0.1", 0), MOCK.Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield f"http://127.0.0.1:{server.server_address[1]}"
    finally:
        server.shutdown()
        server.server_close()
        thread.join()
        with MOCK._jobs_lock:
            MOCK._jobs.clear()


class ModelGenerationSmokeTests(unittest.TestCase):
    def client(self, endpoint):
        return ModelGenerationSmokeClient(endpoint, poll_interval=0.01, timeout=5)

    def test_paid_confirmation_is_required_before_job_creation(self):
        with mock_sidecar() as endpoint, tempfile.TemporaryDirectory() as output:
            with self.assertRaises(PaidCallConfirmationRequired):
                self.client(endpoint).run_text(
                    "printable calibration cube",
                    Path(output),
                    confirm_paid_call=False,
                )
            with MOCK._jobs_lock:
                self.assertEqual(MOCK._jobs, {})

    def test_text_smoke_downloads_and_cleans_up_artifact(self):
        with mock_sidecar() as endpoint, tempfile.TemporaryDirectory() as output:
            result = self.client(endpoint).run_text(
                "printable calibration cube",
                Path(output),
                confirm_paid_call=True,
            )

            self.assertEqual(result.source, "text")
            self.assertEqual(result.artifact_format, "obj")
            self.assertTrue(result.artifact_path.is_file())
            self.assertGreater(result.artifact_path.stat().st_size, 0)
            self.assertIn("awaiting_confirmation", result.states)
            self.assertEqual(result.states[-1], "ready")
            with MOCK._jobs_lock:
                self.assertEqual(MOCK._jobs, {})

    def test_image_smoke_downloads_and_cleans_up_artifact(self):
        with mock_sidecar() as endpoint, tempfile.TemporaryDirectory() as output:
            image_path = Path(output) / "reference.png"
            image_path.write_bytes(MOCK.TINY_PNG)

            result = self.client(endpoint).run_image(
                image_path,
                "Create a printable figurine with a flat base.",
                Path(output),
                confirm_paid_call=True,
            )

            self.assertEqual(result.source, "image")
            self.assertEqual(result.artifact_format, "obj")
            self.assertTrue(result.artifact_path.is_file())
            self.assertGreater(result.artifact_path.stat().st_size, 0)
            self.assertIn("awaiting_confirmation", result.states)
            self.assertEqual(result.states[-1], "ready")
            with MOCK._jobs_lock:
                self.assertEqual(MOCK._jobs, {})


if __name__ == "__main__":
    unittest.main()
