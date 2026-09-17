#!/usr/bin/env python3
import contextlib
import importlib.util
import runpy
import sys
import tempfile
import threading
import unittest
from unittest import mock
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
    def test_retired_cli_cannot_submit_even_with_legacy_confirmation(self):
        args = [str(TOOLS_AI / "smoke_model_generation.py"), "--source", "text",
                "--prompt", "fixture", "--output-dir", "unused", "--confirm-paid-call"]
        with mock.patch.object(sys, "argv", args), \
                mock.patch("urllib.request.urlopen", side_effect=AssertionError("No network allowed")) as transport:
            with self.assertRaises(SystemExit) as stopped:
                runpy.run_path(args[0], run_name="__main__")
            self.assertIsInstance(stopped.exception.code, str)
            self.assertIn("retired", stopped.exception.code)
            transport.assert_not_called()

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
