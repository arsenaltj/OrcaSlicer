#!/usr/bin/env python3
import contextlib
import importlib.util
import json
import os
import sys
import tempfile
import threading
import time
import unittest
import urllib.error
import urllib.request
import uuid
from http.server import ThreadingHTTPServer
from pathlib import Path
from unittest import mock

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
        self.assertEqual(
            generation["styles"],
            ["q_cartoon", "low_poly", "cel_shaded", "enamel_inlay", "sculpture", "custom"],
        )
        self.assertEqual(generation["artifact_formats"], ["obj"])
        self.assertEqual(generation["face_limits"], [100000, 300000, 500000, 1000000])
        self.assertEqual(generation["default_face_limit"], 300000)
        self.assertIn("model_reference", generation["printable_image_pipeline"]["outputs"])
        self.assertIsInstance(generation["palette_recommendation"]["available"], bool)
        self.assertEqual(generation["palette_recommendation"]["max_colors"], 4)

        payload = json.dumps(health)
        for secret in ("OPENAI_API_KEY", "TRIPO_API_KEY", "test-openai", "test-tripo"):
            self.assertNotIn(secret, payload)

    def test_custom_style_contract_is_explicit_and_bounded(self):
        self.assertEqual(
            PRODUCTION._normalize_custom_style("  粗线条木刻玩具  ", "custom"),
            "粗线条木刻玩具",
        )
        with self.assertRaisesRegex(PRODUCTION.RequestError, "required"):
            PRODUCTION._normalize_custom_style("", "custom")
        with self.assertRaisesRegex(PRODUCTION.RequestError, "only allowed"):
            PRODUCTION._normalize_custom_style("woodcut", "q_cartoon")
        with self.assertRaisesRegex(PRODUCTION.RequestError, "1000-byte"):
            PRODUCTION._normalize_custom_style("a" * 1001, "custom")

    def test_custom_style_is_persisted_and_exposed_for_task_recovery(self):
        job_id = str(uuid.uuid4())
        with tempfile.TemporaryDirectory() as directory:
            job_directory = Path(directory) / job_id
            job_directory.mkdir()
            job = PRODUCTION.Job(
                id=job_id,
                source="text",
                directory=job_directory,
                style="custom",
                custom_style="复古木刻版画",
            )
            PRODUCTION._persist_job(job)
            restored = PRODUCTION._load_job(job_directory)
            self.assertIsNotNone(restored)
            self.assertEqual(restored.custom_style, "复古木刻版画")
            self.assertEqual(PRODUCTION._public_job(restored)["custom_style"], "复古木刻版画")

    def test_palette_recommendation_is_persisted_and_exposed_for_task_recovery(self):
        job_id = str(uuid.uuid4())
        recommendation = {
            "summary": "温暖主体配合深色结构",
            "colors": [
                {"hex": "#D96B43", "name": "陶土橙", "role": "primary", "usage": "主体", "reason": "视觉中心"},
                {"hex": "#2B2422", "name": "深棕", "role": "structure", "usage": "结构", "reason": "稳定轮廓"},
                {"hex": "#F2D7B5", "name": "暖白", "role": "light", "usage": "高光", "reason": "明暗层次"},
                {"hex": "#2F6B5F", "name": "墨绿", "role": "accent", "usage": "点缀", "reason": "冷暖对比"},
            ],
        }
        with tempfile.TemporaryDirectory() as directory:
            job_directory = Path(directory) / job_id
            job_directory.mkdir()
            job = PRODUCTION.Job(id=job_id, source="text", directory=job_directory)
            job.state = "awaiting_palette_confirmation"
            job.phase = "awaiting_palette_confirmation"
            job.palette_recommendation = recommendation
            PRODUCTION._persist_job(job)

            restored = PRODUCTION._load_job(job_directory)

            self.assertIsNotNone(restored)
            self.assertEqual(restored.palette_recommendation, recommendation)
            public = PRODUCTION._public_job(restored)
            self.assertEqual(public["palette_recommendation"], recommendation)
            self.assertFalse(public["palette_recommendation_confirmed"])

    def test_recommendation_worker_forwards_stored_reference_image(self):
        job_id = str(uuid.uuid4())
        with tempfile.TemporaryDirectory() as directory:
            job_directory = Path(directory) / job_id
            job_directory.mkdir()
            image_path = job_directory / "input.png"
            image_path.write_bytes(b"\x89PNG\r\n\x1a\nexample")
            job = PRODUCTION.Job(id=job_id, source="image", directory=job_directory, user_prompt="保留主体")
            job.input_path = image_path
            result = mock.Mock()
            result.as_dict.return_value = {
                "summary": "summary",
                "colors": [
                    {"hex": "#D96B43", "name": "a", "role": "primary", "usage": "u", "reason": "r"},
                    {"hex": "#2B2422", "name": "b", "role": "structure", "usage": "u", "reason": "r"},
                    {"hex": "#F2D7B5", "name": "c", "role": "light", "usage": "u", "reason": "r"},
                    {"hex": "#2F6B5F", "name": "d", "role": "accent", "usage": "u", "reason": "r"},
                ],
            }
            with mock.patch.object(PRODUCTION, "recommend_printable_palette", return_value=result) as recommend:
                PRODUCTION._recommend_palette_job(job)

            self.assertEqual(job.state, "awaiting_palette_confirmation")
            self.assertEqual(recommend.call_args.kwargs["image_path"], image_path)

    def test_text_recommendation_confirmation_continues_the_same_job(self):
        result = mock.Mock()
        result.as_dict.return_value = {
            "summary": "summary",
            "colors": [
                {"hex": "#D96B43", "name": "a", "role": "primary", "usage": "u", "reason": "r"},
                {"hex": "#2B2422", "name": "b", "role": "structure", "usage": "u", "reason": "r"},
                {"hex": "#F2D7B5", "name": "c", "role": "light", "usage": "u", "reason": "r"},
                {"hex": "#2F6B5F", "name": "d", "role": "accent", "usage": "u", "reason": "r"},
            ],
        }
        with tempfile.TemporaryDirectory() as directory, temporary_environment(
            OPENAI_API_KEY="test-openai", ORCASLICER_AI_OUTPUT_DIR=directory
        ), mock.patch.object(PRODUCTION, "recommend_printable_palette", return_value=result), mock.patch.object(
            PRODUCTION, "_preprocess_text_job"
        ) as preprocess:
            with PRODUCTION._JOBS_LOCK:
                previous = dict(PRODUCTION._JOBS)
                PRODUCTION._JOBS.clear()
            try:
                with sidecar_server(PRODUCTION.Handler) as port:
                    create = urllib.request.Request(
                        f"http://127.0.0.1:{port}/v1/orcaslicer/model-jobs/recommend-text-palette",
                        data=json.dumps({"request_id": "r1", "prompt": "一只机械麒麟", "style": "q_cartoon", "print": {}}).encode(),
                        method="POST",
                        headers={"X-OrcaSlicer-Client": "native", "Content-Type": "application/json"},
                    )
                    with urllib.request.urlopen(create, timeout=5) as response:
                        job_id = json.loads(response.read())["job"]["id"]
                    deadline = time.time() + 5
                    while time.time() < deadline:
                        with PRODUCTION._JOBS_LOCK:
                            state = PRODUCTION._JOBS[job_id].state
                        if state == "awaiting_palette_confirmation":
                            break
                        time.sleep(0.01)
                    self.assertEqual(state, "awaiting_palette_confirmation")

                    confirm = urllib.request.Request(
                        f"http://127.0.0.1:{port}/v1/orcaslicer/model-jobs/{job_id}/confirm-palette",
                        data=json.dumps({
                            "palette": ["#D96B43", "#2B2422", "#F2D7B5", "#2F6B5F"],
                            "palette_roles": {"primary": "#D96B43", "structure": "#2B2422", "light": "#F2D7B5", "accent": "#2F6B5F"},
                        }).encode(),
                        method="POST",
                        headers={"X-OrcaSlicer-Client": "native", "Content-Type": "application/json"},
                    )
                    with urllib.request.urlopen(confirm, timeout=5) as response:
                        payload = json.loads(response.read())

                self.assertEqual(payload["job"]["id"], job_id)
                self.assertTrue(payload["job"]["palette_recommendation_confirmed"])
                self.assertEqual(payload["job"]["palette"][0], "#D96B43")
                deadline = time.time() + 2
                while preprocess.call_count == 0 and time.time() < deadline:
                    time.sleep(0.01)
                preprocess.assert_called_once()
            finally:
                with PRODUCTION._JOBS_LOCK:
                    PRODUCTION._JOBS.clear()
                    PRODUCTION._JOBS.update(previous)

    def test_image_palette_recommendation_route_stores_the_reference(self):
        result = mock.Mock()
        result.as_dict.return_value = {
            "summary": "summary",
            "colors": [
                {"hex": "#D96B43", "name": "a", "role": "primary", "usage": "u", "reason": "r"},
                {"hex": "#2B2422", "name": "b", "role": "structure", "usage": "u", "reason": "r"},
                {"hex": "#F2D7B5", "name": "c", "role": "light", "usage": "u", "reason": "r"},
                {"hex": "#2F6B5F", "name": "d", "role": "accent", "usage": "u", "reason": "r"},
            ],
        }
        boundary = "----OrcaPaletteTest"
        parts = []
        for name, value in {
            "request_id": "r2",
            "instruction": "保留主体",
            "style": "cel_shaded",
            "custom_style": "",
            "print": "{}",
        }.items():
            parts.append(
                f"--{boundary}\r\nContent-Disposition: form-data; name=\"{name}\"\r\n\r\n{value}\r\n".encode("utf-8")
            )
        parts.append(
            f"--{boundary}\r\nContent-Disposition: form-data; name=\"image\"; filename=\"input.png\"\r\n"
            "Content-Type: image/png\r\n\r\n".encode("ascii")
            + b"\x89PNG\r\n\x1a\nexample\r\n"
        )
        parts.append(f"--{boundary}--\r\n".encode("ascii"))
        body = b"".join(parts)
        with tempfile.TemporaryDirectory() as directory, temporary_environment(
            OPENAI_API_KEY="test-openai", ORCASLICER_AI_OUTPUT_DIR=directory
        ), mock.patch.object(PRODUCTION, "recommend_printable_palette", return_value=result) as recommend:
            with PRODUCTION._JOBS_LOCK:
                previous = dict(PRODUCTION._JOBS)
                PRODUCTION._JOBS.clear()
            try:
                with sidecar_server(PRODUCTION.Handler) as port:
                    request = urllib.request.Request(
                        f"http://127.0.0.1:{port}/v1/orcaslicer/model-jobs/recommend-image-palette",
                        data=body,
                        method="POST",
                        headers={
                            "X-OrcaSlicer-Client": "native",
                            "Content-Type": f"multipart/form-data; boundary={boundary}",
                        },
                    )
                    with urllib.request.urlopen(request, timeout=5) as response:
                        job_id = json.loads(response.read())["job"]["id"]
                    deadline = time.time() + 5
                    while time.time() < deadline:
                        with PRODUCTION._JOBS_LOCK:
                            job = PRODUCTION._JOBS[job_id]
                            state = job.state
                        if state == "awaiting_palette_confirmation":
                            break
                        time.sleep(0.01)

                self.assertEqual(state, "awaiting_palette_confirmation")
                self.assertTrue(job.input_path.is_file())
                self.assertEqual(recommend.call_args.kwargs["image_path"], job.input_path)
            finally:
                with PRODUCTION._JOBS_LOCK:
                    PRODUCTION._JOBS.clear()
                    PRODUCTION._JOBS.update(previous)

    def test_confirm_palette_rejects_wrong_job_state(self):
        job_id = str(uuid.uuid4())
        with tempfile.TemporaryDirectory() as directory:
            job_directory = Path(directory) / job_id
            job_directory.mkdir()
            job = PRODUCTION.Job(id=job_id, source="text", directory=job_directory, state="awaiting_confirmation")
            with PRODUCTION._JOBS_LOCK:
                previous = dict(PRODUCTION._JOBS)
                PRODUCTION._JOBS.clear()
                PRODUCTION._JOBS[job_id] = job
            try:
                with sidecar_server(PRODUCTION.Handler) as port:
                    request = urllib.request.Request(
                        f"http://127.0.0.1:{port}/v1/orcaslicer/model-jobs/{job_id}/confirm-palette",
                        data=json.dumps({"palette": ["#D96B43"]}).encode(),
                        method="POST",
                        headers={"X-OrcaSlicer-Client": "native", "Content-Type": "application/json"},
                    )
                    with self.assertRaises(urllib.error.HTTPError) as error:
                        urllib.request.urlopen(request, timeout=5)
                self.assertEqual(error.exception.code, 409)
            finally:
                with PRODUCTION._JOBS_LOCK:
                    PRODUCTION._JOBS.clear()
                    PRODUCTION._JOBS.update(previous)

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

    def test_public_job_exposes_persisted_model_quality_report(self):
        job_id = str(uuid.uuid4())
        with tempfile.TemporaryDirectory() as directory:
            job_directory = Path(directory) / job_id
            job_directory.mkdir()
            quality = {
                "schema_version": 1,
                "gate_version": "structural-v1",
                "status": "review",
                "errors": [],
                "warnings": ["tiny_detached_components"],
                "metrics": {"component_count": 3},
            }
            (job_directory / PRODUCTION.MODEL_QUALITY_FILENAME).write_text(
                json.dumps(quality), encoding="utf-8"
            )
            job = PRODUCTION.Job(id=job_id, source="image", directory=job_directory)

            public = PRODUCTION._public_job(job)

            self.assertEqual(public["model_quality"], quality)

    def test_public_job_exposes_persisted_visual_quality_and_view_sheet(self):
        job_id = str(uuid.uuid4())
        with tempfile.TemporaryDirectory() as directory:
            job_directory = Path(directory) / job_id
            job_directory.mkdir()
            visual = {
                "schema_version": 1,
                "review_version": "visual-v1",
                "status": "review",
                "score": 72,
                "warnings": ["visual_silhouette_unclear"],
            }
            (job_directory / PRODUCTION.VISUAL_QUALITY_FILENAME).write_text(json.dumps(visual), encoding="utf-8")
            (job_directory / "model-view-sheet.png").write_bytes(b"\x89PNG\r\n\x1a\nexample")
            job = PRODUCTION.Job(id=job_id, source="image", directory=job_directory)

            public = PRODUCTION._public_job(job)

            self.assertEqual(public["visual_quality"], visual)
            self.assertTrue(public["model_views"]["ready"])

    def test_job_status_endpoint_returns_registered_job(self):
        job_id = str(uuid.uuid4())
        with tempfile.TemporaryDirectory() as directory:
            job_directory = Path(directory) / job_id
            job_directory.mkdir()
            job = PRODUCTION.Job(id=job_id, source="text", directory=job_directory)
            job.state = "awaiting_confirmation"
            job.phase = "awaiting_confirmation"
            with PRODUCTION._JOBS_LOCK:
                previous = dict(PRODUCTION._JOBS)
                PRODUCTION._JOBS.clear()
                PRODUCTION._JOBS[job.id] = job
            try:
                with sidecar_server(PRODUCTION.Handler) as port:
                    request = urllib.request.Request(
                        f"http://127.0.0.1:{port}/v1/orcaslicer/model-jobs/{job.id}",
                        headers={"X-OrcaSlicer-Client": "native"},
                    )
                    with urllib.request.urlopen(request, timeout=5) as response:
                        payload = json.loads(response.read())
                self.assertEqual(payload["job"]["id"], job.id)
                self.assertEqual(payload["job"]["state"], "awaiting_confirmation")
            finally:
                with PRODUCTION._JOBS_LOCK:
                    PRODUCTION._JOBS.clear()
                    PRODUCTION._JOBS.update(previous)

    def test_recheck_analyzes_only_the_registered_obj_without_paid_calls(self):
        job_id = str(uuid.uuid4())
        tetrahedron = (
            "v 0 0 0 1 0 0\n"
            "v 10 0 0 1 0 0\n"
            "v 0 10 0 1 0 0\n"
            "v 0 0 10 1 0 0\n"
            "f 1 3 2\n"
            "f 1 2 4\n"
            "f 1 4 3\n"
            "f 2 3 4\n"
        )
        with tempfile.TemporaryDirectory() as directory:
            job_directory = Path(directory) / job_id
            job_directory.mkdir()
            artifact = job_directory / "model-vertex-color.obj"
            artifact.write_text(tetrahedron, encoding="ascii")
            job = PRODUCTION.Job(id=job_id, source="image", directory=job_directory)
            job.state = "ready"
            job.phase = "ready"
            job.artifact_path = artifact
            job.artifact_format = "obj"
            with PRODUCTION._JOBS_LOCK:
                previous = dict(PRODUCTION._JOBS)
                PRODUCTION._JOBS.clear()
                PRODUCTION._JOBS[job.id] = job
            try:
                paid = [
                    mock.patch.object(PRODUCTION, name)
                    for name in ("create_text_task", "create_image_task", "create_conversion", "upload_image")
                ]
                with paid[0] as text_task, paid[1] as image_task, paid[2] as conversion, paid[3] as upload:
                    with sidecar_server(PRODUCTION.Handler) as port:
                        request = urllib.request.Request(
                            f"http://127.0.0.1:{port}/v1/orcaslicer/model-jobs/{job.id}/recheck",
                            data=b"{}",
                            method="POST",
                            headers={"X-OrcaSlicer-Client": "native", "Content-Type": "application/json"},
                        )
                        with urllib.request.urlopen(request, timeout=5) as response:
                            payload = json.loads(response.read())
                    self.assertEqual(payload["job"]["model_quality"]["status"], "pass")
                    self.assertTrue((job_directory / PRODUCTION.MODEL_QUALITY_FILENAME).is_file())
                    for provider in (text_task, image_task, conversion, upload):
                        provider.assert_not_called()
            finally:
                with PRODUCTION._JOBS_LOCK:
                    PRODUCTION._JOBS.clear()
                    PRODUCTION._JOBS.update(previous)

    def test_recheck_rejects_an_artifact_outside_the_registered_job_directory(self):
        job_id = str(uuid.uuid4())
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            job_directory = root / job_id
            job_directory.mkdir()
            artifact = root / "outside.obj"
            artifact.write_text("v 0 0 0\n", encoding="ascii")
            job = PRODUCTION.Job(id=job_id, source="text", directory=job_directory)
            job.state = "ready"
            job.phase = "ready"
            job.artifact_path = artifact
            job.artifact_format = "obj"
            with PRODUCTION._JOBS_LOCK:
                previous = dict(PRODUCTION._JOBS)
                PRODUCTION._JOBS.clear()
                PRODUCTION._JOBS[job.id] = job
            try:
                with sidecar_server(PRODUCTION.Handler) as port:
                    request = urllib.request.Request(
                        f"http://127.0.0.1:{port}/v1/orcaslicer/model-jobs/{job.id}/recheck",
                        data=b"{}",
                        method="POST",
                        headers={"X-OrcaSlicer-Client": "native", "Content-Type": "application/json"},
                    )
                    with self.assertRaises(urllib.error.HTTPError) as raised:
                        urllib.request.urlopen(request, timeout=5)
                self.assertEqual(raised.exception.code, 409)
            finally:
                with PRODUCTION._JOBS_LOCK:
                    PRODUCTION._JOBS.clear()
                    PRODUCTION._JOBS.update(previous)

    def test_recheck_adopts_a_legacy_model_library_entry_without_paid_calls(self):
        job_id = str(uuid.uuid4())
        tetrahedron = (
            "v 0 0 0 1 0 0\n"
            "v 10 0 0 1 0 0\n"
            "v 0 10 0 1 0 0\n"
            "v 0 0 10 1 0 0\n"
            "f 1 3 2\n"
            "f 1 2 4\n"
            "f 1 4 3\n"
            "f 2 3 4\n"
        )
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            job_directory = root / job_id
            job_directory.mkdir()
            (job_directory / "model-vertex-color.obj").write_text(tetrahedron, encoding="ascii")
            with PRODUCTION._JOBS_LOCK:
                previous = dict(PRODUCTION._JOBS)
                PRODUCTION._JOBS.clear()
            try:
                paid = [
                    mock.patch.object(PRODUCTION, name)
                    for name in ("create_text_task", "create_image_task", "create_conversion", "upload_image")
                ]
                with temporary_environment(ORCASLICER_AI_OUTPUT_DIR=str(root)):
                    with paid[0] as text_task, paid[1] as image_task, paid[2] as conversion, paid[3] as upload:
                        with sidecar_server(PRODUCTION.Handler) as port:
                            request = urllib.request.Request(
                                f"http://127.0.0.1:{port}/v1/orcaslicer/model-jobs/{job_id}/recheck",
                                data=b"{}",
                                method="POST",
                                headers={"X-OrcaSlicer-Client": "native", "Content-Type": "application/json"},
                            )
                            with urllib.request.urlopen(request, timeout=5) as response:
                                payload = json.loads(response.read())
                        self.assertEqual(payload["job"]["state"], "ready")
                        self.assertEqual(payload["job"]["model_quality"]["status"], "pass")
                        self.assertTrue((job_directory / PRODUCTION.MODEL_QUALITY_FILENAME).is_file())
                        self.assertTrue((job_directory / PRODUCTION.JOB_STATE_FILENAME).is_file())
                        for provider in (text_task, image_task, conversion, upload):
                            provider.assert_not_called()
            finally:
                with PRODUCTION._JOBS_LOCK:
                    PRODUCTION._JOBS.clear()
                    PRODUCTION._JOBS.update(previous)

    def test_visual_review_uses_only_registered_artifact_and_returns_advisory_report(self):
        job_id = str(uuid.uuid4())
        with tempfile.TemporaryDirectory() as directory:
            job_directory = Path(directory) / job_id
            job_directory.mkdir()
            artifact = job_directory / "model-vertex-color.obj"
            artifact.write_text(
                "v 0 0 0 1 0 0\nv 10 0 0 0 1 0\nv 0 10 0 0 0 1\nf 1 2 3\n",
                encoding="ascii",
            )
            job = PRODUCTION.Job(id=job_id, source="text", directory=job_directory)
            job.state = "ready"
            job.phase = "ready"
            job.artifact_path = artifact
            job.artifact_format = "obj"
            with PRODUCTION._JOBS_LOCK:
                previous = dict(PRODUCTION._JOBS)
                PRODUCTION._JOBS.clear()
                PRODUCTION._JOBS[job.id] = job

            def review(obj_path, root, **kwargs):
                self.assertEqual(Path(obj_path), artifact.resolve())
                report = {
                    "schema_version": 1,
                    "review_version": "visual-v1",
                    "status": "review",
                    "score": 70,
                    "confidence": 0.8,
                    "summary": "建议检查轮廓。",
                    "warnings": ["visual_silhouette_unclear"],
                    "errors": [],
                    "checks": {},
                }
                (Path(root) / PRODUCTION.VISUAL_QUALITY_FILENAME).write_text(json.dumps(report), encoding="utf-8")
                return report

            try:
                with mock.patch.object(PRODUCTION, "review_model_visual_quality", side_effect=review) as visual_review:
                    with sidecar_server(PRODUCTION.Handler) as port:
                        request = urllib.request.Request(
                            f"http://127.0.0.1:{port}/v1/orcaslicer/model-jobs/{job.id}/visual-review",
                            data=b"{}",
                            method="POST",
                            headers={"X-OrcaSlicer-Client": "native", "Content-Type": "application/json"},
                        )
                        with urllib.request.urlopen(request, timeout=5) as response:
                            payload = json.loads(response.read())
                self.assertEqual(payload["job"]["visual_quality"]["status"], "review")
                visual_review.assert_called_once()
            finally:
                with PRODUCTION._JOBS_LOCK:
                    PRODUCTION._JOBS.clear()
                    PRODUCTION._JOBS.update(previous)

    def test_mock_health_contract(self):
        self.assert_contract(self.fetch_health(MOCK.Handler))

    def test_mock_palette_recommendation_supports_offline_gui_flow(self):
        with MOCK._jobs_lock:
            previous = dict(MOCK._jobs)
            MOCK._jobs.clear()
        try:
            with sidecar_server(MOCK.Handler) as port:
                create = urllib.request.Request(
                    f"http://127.0.0.1:{port}/v1/orcaslicer/model-jobs/recommend-text-palette",
                    data=json.dumps({"request_id": "mock", "prompt": "一只机械麒麟"}).encode(),
                    method="POST",
                    headers={"X-OrcaSlicer-Client": "native", "Content-Type": "application/json"},
                )
                with urllib.request.urlopen(create, timeout=5) as response:
                    created = json.loads(response.read())["job"]
                self.assertEqual(created["state"], "awaiting_palette_confirmation")
                self.assertEqual(len(created["palette_recommendation"]["colors"]), 4)

                confirm = urllib.request.Request(
                    f"http://127.0.0.1:{port}/v1/orcaslicer/model-jobs/{created['id']}/confirm-palette",
                    data=json.dumps({"palette": ["#D96B43", "#2B2422", "#F2D7B5", "#2F6B5F"]}).encode(),
                    method="POST",
                    headers={"X-OrcaSlicer-Client": "native", "Content-Type": "application/json"},
                )
                with urllib.request.urlopen(confirm, timeout=5) as response:
                    confirmed = json.loads(response.read())["job"]
            self.assertTrue(confirmed["palette_recommendation_confirmed"])
            self.assertEqual(confirmed["state"], "preprocessing")
        finally:
            with MOCK._jobs_lock:
                MOCK._jobs.clear()
                MOCK._jobs.update(previous)

    def test_production_health_contract_without_credentials(self):
        with temporary_environment(OPENAI_API_KEY=None, TRIPO_API_KEY=None):
            health = self.fetch_health(PRODUCTION.Handler)
        self.assert_contract(health)
        self.assertFalse(health["capabilities"]["config_proposal"]["available"])
        self.assertFalse(health["capabilities"]["model_generation"]["available"])
        self.assertFalse(health["capabilities"]["model_generation"]["palette_recommendation"]["available"])

    def test_production_health_contract_with_openai_only(self):
        with temporary_environment(OPENAI_API_KEY="test-openai", TRIPO_API_KEY=None):
            health = self.fetch_health(PRODUCTION.Handler)
        self.assert_contract(health)
        self.assertTrue(health["capabilities"]["config_proposal"]["available"])
        self.assertFalse(health["capabilities"]["model_generation"]["available"])
        self.assertTrue(health["capabilities"]["model_generation"]["palette_recommendation"]["available"])

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
        self.assertTrue(health["capabilities"]["model_generation"]["palette_recommendation"]["available"])

    def test_production_health_reports_provider_url_without_credentials(self):
        with temporary_environment(
            OPENAI_BASE_URL="https://laotie.dev/",
            OPENAI_API_KEY="test-openai",
            TRIPO_API_KEY="test-tripo",
            ORCASLICER_AI_CONFIG_MODE="internal_locked",
        ):
            health = self.fetch_health(PRODUCTION.Handler)
        self.assertEqual(health["runtime"]["openai_base_url"], "https://laotie.dev")
        self.assertEqual(health["runtime"]["configuration_mode"], "internal_locked")
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
        self.assertFalse(health["capabilities"]["model_generation"]["palette_recommendation"]["available"])


if __name__ == "__main__":
    unittest.main()
