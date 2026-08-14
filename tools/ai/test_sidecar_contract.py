#!/usr/bin/env python3
import contextlib
import importlib.util
import json
import os
import sys
import tempfile
import threading
import unittest
import urllib.request
import uuid
from http.server import ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOOLS_AI = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(TOOLS_AI))


def load_module(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


MOCK = load_module("orca_ai_sidecar_mock_contract", ROOT / "ai_sidecar_mock.py")
PRODUCTION = load_module("orca_ai_sidecar_production_contract", TOOLS_AI / "orca_ai_sidecar.py")


@contextlib.contextmanager
def sidecar_server(handler):
    server = ThreadingHTTPServer(("127.0.0.1", 0), handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield server.server_address[1]
    finally:
        server.shutdown()
        server.server_close()
        thread.join()


@contextlib.contextmanager
def temporary_environment(**values):
    original = {key: os.environ.get(key) for key in values}
    try:
        for key, value in values.items():
            if value is None:
                os.environ.pop(key, None)
            else:
                os.environ[key] = value
        yield
    finally:
        for key, value in original.items():
            if value is None:
                os.environ.pop(key, None)
            else:
                os.environ[key] = value


class SidecarHealthContractTests(unittest.TestCase):
    def fetch_health(self, handler):
        with sidecar_server(handler) as port:
            with urllib.request.urlopen(f"http://127.0.0.1:{port}/health", timeout=5) as response:
                self.assertEqual(response.status, 200)
                return json.loads(response.read())

    def assert_contract(self, health):
        self.assertTrue(health["ok"])
        self.assertEqual(health["protocol_version"], 1)
        self.assertIsInstance(health["sidecar_version"], str)
        self.assertNotEqual(health["sidecar_version"], "")
        self.assertNotIn("features", health)

        capabilities = health["capabilities"]
        self.assertEqual(set(capabilities), {"config_proposal", "model_generation"})
        self.assertIsInstance(capabilities["config_proposal"]["available"], bool)

        generation = capabilities["model_generation"]
        self.assertIsInstance(generation["available"], bool)
        self.assertEqual(generation["sources"], ["text", "image"])
        self.assertEqual(generation["artifact_formats"], ["obj"])
        self.assertEqual(generation["face_limits"], [100000, 300000, 500000, 1000000])
        self.assertEqual(generation["default_face_limit"], 300000)

        payload = json.dumps(health)
        for secret in ("OPENAI_API_KEY", "TRIPO_API_KEY", "test-openai", "test-tripo"):
            self.assertNotIn(secret, payload)

    def test_latest_job_endpoint_returns_persisted_job_without_secrets(self):
        job_id = str(uuid.uuid4())
        with tempfile.TemporaryDirectory() as directory:
            job_directory = Path(directory) / job_id
            job_directory.mkdir()
            job = PRODUCTION.Job(id=job_id, source="text", directory=job_directory)
            job.state = "awaiting_confirmation"
            job.phase = "awaiting_confirmation"
            job.prepared_prompt = "printable object"
            with PRODUCTION._JOBS_LOCK:
                previous = dict(PRODUCTION._JOBS)
                PRODUCTION._JOBS.clear()
                PRODUCTION._JOBS[job.id] = job
            try:
                with sidecar_server(PRODUCTION.Handler) as port:
                    request = urllib.request.Request(
                        f"http://127.0.0.1:{port}/v1/orcaslicer/model-jobs/latest",
                        headers={"X-OrcaSlicer-Client": "native"},
                    )
                    with urllib.request.urlopen(request, timeout=5) as response:
                        payload = json.loads(response.read())
                self.assertEqual(payload["job"]["id"], job.id)
                self.assertEqual(payload["job"]["prepared_prompt"], "printable object")
                self.assertNotIn("API_KEY", json.dumps(payload))
            finally:
                with PRODUCTION._JOBS_LOCK:
                    PRODUCTION._JOBS.clear()
                    PRODUCTION._JOBS.update(previous)

    def test_mock_health_contract(self):
        self.assert_contract(self.fetch_health(MOCK.Handler))

    def test_production_health_contract_without_credentials(self):
        with temporary_environment(OPENAI_API_KEY=None, TRIPO_API_KEY=None):
            health = self.fetch_health(PRODUCTION.Handler)
        self.assert_contract(health)
        self.assertFalse(health["capabilities"]["config_proposal"]["available"])
        self.assertFalse(health["capabilities"]["model_generation"]["available"])

    def test_production_health_contract_with_openai_only(self):
        with temporary_environment(OPENAI_API_KEY="test-openai", TRIPO_API_KEY=None):
            health = self.fetch_health(PRODUCTION.Handler)
        self.assert_contract(health)
        self.assertTrue(health["capabilities"]["config_proposal"]["available"])
        self.assertFalse(health["capabilities"]["model_generation"]["available"])

    def test_production_health_contract_with_generation_credentials(self):
        with temporary_environment(
            OPENAI_API_KEY="test-openai",
            TRIPO_API_KEY="test-tripo",
            ORCASLICER_AI_ALLOW_PREPROCESS_FALLBACK=None,
        ):
            health = self.fetch_health(PRODUCTION.Handler)
        self.assert_contract(health)
        self.assertTrue(health["capabilities"]["config_proposal"]["available"])
        self.assertTrue(health["capabilities"]["model_generation"]["available"])

    def test_production_health_reports_provider_url_without_credentials(self):
        with temporary_environment(
            OPENAI_BASE_URL="https://laotie.dev/",
            OPENAI_API_KEY="test-openai",
            TRIPO_API_KEY="test-tripo",
        ):
            health = self.fetch_health(PRODUCTION.Handler)
        self.assertEqual(health["runtime"]["openai_base_url"], "https://laotie.dev")
        self.assertNotIn("test-openai", json.dumps(health))

    def test_production_health_contract_with_tripo_and_preprocess_fallback(self):
        with temporary_environment(
            OPENAI_API_KEY=None,
            TRIPO_API_KEY="test-tripo",
            ORCASLICER_AI_ALLOW_PREPROCESS_FALLBACK="1",
        ):
            health = self.fetch_health(PRODUCTION.Handler)
        self.assert_contract(health)
        self.assertFalse(health["capabilities"]["config_proposal"]["available"])
        self.assertTrue(health["capabilities"]["model_generation"]["available"])


if __name__ == "__main__":
    unittest.main()
