#!/usr/bin/env python3
import io
import json
import os
from pathlib import Path
from types import SimpleNamespace
import tempfile
import threading
import unittest
from unittest import mock

from tools.ai.model_provider_gateway import (
    ModelTaskRequest,
    PaidTaskAuthorization,
    ProviderGatewayError,
)
from tools.ai.ai_diagnostics import diagnostic_context
from tools.ai.yoyoclaw_client import (
    YoYoClawClient,
    YoYoClawConfig,
    YoYoClawError,
)
from tools.ai.yoyoclaw_provider_gateway import (
    GEOMETRY_LIBRARY,
    GEOMETRY_ROLE,
    IMAGE_LIBRARY,
    IMAGE_ROLE,
    PREFERRED_GEOMETRY_MODEL,
    PREFERRED_IMAGE_MODEL,
    YoYoClawModelProviderGateway,
    generate_design_image,
)


class _FakeYoYoClawClient:
    def __init__(self, *, image_models=None, geometry_models=None, image_default="",
                 geometry_default="", configured=True, submit=None):
        self.config = SimpleNamespace(configured=configured)
        self.image_models = image_models or []
        self.geometry_models = geometry_models or []
        self.image_default = image_default
        self.geometry_default = geometry_default
        self.submit = submit
        self.submit_calls = []
        self.generate_calls = []
        self.download_calls = []

    def whitelist(self, library):
        return self.image_models if library == IMAGE_LIBRARY else self.geometry_models

    def whitelist_catalog(self, library):
        return {
            "models": self.whitelist(library),
            "default_model": self.image_default if library == IMAGE_LIBRARY else self.geometry_default,
        }

    def submit_3d(self, model, image_path):
        self.submit_calls.append((model, Path(image_path)))
        if isinstance(self.submit, BaseException):
            raise self.submit
        return self.submit or "yoyoclaw-task-1"

    def generate_image(self, model, prompt, image_path=None):
        self.generate_calls.append((model, prompt, image_path))
        return "https://example.invalid/design.png"

    def download(self, url, destination, max_bytes):
        self.download_calls.append((url, Path(destination), max_bytes))
        Path(destination).write_bytes(b"PNG")
        return Path(destination)


class _TransportResponse:
    def __init__(self, status_code, payload, chunks=(), headers=None):
        self.status_code = status_code
        self._payload = payload
        self._chunks = tuple(chunks)
        self.headers = headers or {}

    def json(self):
        if isinstance(self._payload, Exception):
            raise self._payload
        return self._payload

    def iter_content(self, _chunk_size):
        return iter(self._chunks)


class _TransportSession:
    def __init__(self, responses, download_response=None):
        self.responses = list(responses)
        self.download_response = download_response
        self.requests = []
        self.gets = []

    def request(self, method, url, **kwargs):
        self.requests.append((method, url, kwargs))
        response = self.responses.pop(0)
        if isinstance(response, Exception):
            raise response
        return response

    def get(self, url, **kwargs):
        self.gets.append((url, kwargs))
        return self.download_response


def _configured_yoyoclaw_config():
    return YoYoClawConfig(
        base_url="https://yoyoclaw.example.invalid",
        root_secret="root-secret",
        access_key="access-key",
        hmac_secret="hmac-secret",
        client_id="client-id",
        device_name="device",
        serial_number="serial",
        user_id="user",
    )


class _Clock:
    def __init__(self):
        self.now = 0.0
        self.sleeps = []

    def monotonic(self):
        return self.now

    def sleep(self, seconds):
        self.sleeps.append(seconds)
        self.now += seconds


class YoYoClawConfigTests(unittest.TestCase):
    def setUp(self):
        self.defaults = _configured_yoyoclaw_config()
        for field in self.defaults.__dataclass_fields__:
            patcher = mock.patch(
                "tools.ai.yoyoclaw_client.DEFAULT_" + field.upper(),
                getattr(self.defaults, field),
            )
            patcher.start()
            self.addCleanup(patcher.stop)

    def test_client_uses_environment_overrides_and_keeps_explicit_config(self):
        expected = YoYoClawConfig(
            base_url="https://env.invalid", root_secret="env-secret",
            access_key="env-access", hmac_secret="env-hmac", client_id="env-client",
            device_name="env-device", serial_number="env-serial", user_id="env-user",
        )
        environment = {
            "YOYOCLAW_BASE_URL": " https://env.invalid ",
            "YOYOCLAW_ROOT_SECRET": "env-secret",
            "YOYOCLAW_HMAC_ACCESS_KEY": "env-access",
            "YOYOCLAW_HMAC_SECRET": "env-hmac",
            "YOYOCLAW_HMAC_CLIENT_ID": "env-client",
            "YOYOCLAW_DEVICE_NAME": "env-device",
            "YOYOCLAW_SERIAL_NUMBER": "env-serial",
            "YOYOCLAW_USER_ID": "env-user",
        }
        with mock.patch.dict(os.environ, environment, clear=True):
            self.assertEqual(YoYoClawConfig.from_environment(), expected)
            self.assertEqual(YoYoClawClient(session=object()).config, expected)
            self.assertIs(YoYoClawClient(self.defaults, session=object()).config, self.defaults)

    def test_missing_empty_and_blank_fields_fall_back_independently(self):
        for missing in (None, "", " \t\n "):
            with self.subTest(value=missing):
                environment = {"YOYOCLAW_USER_ID": "override-user"}
                if missing is not None:
                    environment.update({
                        "YOYOCLAW_" + field.upper(): missing
                        for field in self.defaults.__dataclass_fields__ if field != "user_id"
                    })
                    environment["YOYOCLAW_HMAC_ACCESS_KEY"] = missing
                    environment["YOYOCLAW_HMAC_CLIENT_ID"] = missing
                with mock.patch.dict(os.environ, environment, clear=True):
                    config = YoYoClawConfig.from_environment()
                self.assertTrue(config.configured)
                self.assertEqual(config.user_id, "override-user")
                for field in self.defaults.__dataclass_fields__:
                    if field != "user_id":
                        self.assertEqual(getattr(config, field), getattr(self.defaults, field))

    def test_unreadable_environment_field_does_not_discard_other_overrides(self):
        def getenv(name, default):
            if name == "YOYOCLAW_ROOT_SECRET":
                raise OSError("environment unavailable")
            return "override-user" if name == "YOYOCLAW_USER_ID" else default

        with mock.patch("tools.ai.yoyoclaw_client.os.getenv", side_effect=getenv):
            config = YoYoClawConfig.from_environment()
        self.assertEqual(config.root_secret, self.defaults.root_secret)
        self.assertEqual(config.user_id, "override-user")
        self.assertTrue(config.configured)

    def test_original_script_names_take_precedence_over_short_aliases(self):
        environment = {"YOYOCLAW_ACCESS_KEY": "short-key", "YOYOCLAW_CLIENT_ID": "short-client"}
        with mock.patch.dict(os.environ, environment, clear=True):
            config = YoYoClawConfig.from_environment()
            self.assertEqual(config.access_key, "short-key")
            self.assertEqual(config.client_id, "short-client")
            with mock.patch.dict(os.environ, {
                "YOYOCLAW_HMAC_ACCESS_KEY": "script-key", "YOYOCLAW_HMAC_CLIENT_ID": "script-client",
            }):
                config = YoYoClawConfig.from_environment()
        self.assertEqual(config.access_key, "script-key")
        self.assertEqual(config.client_id, "script-client")

    def test_authentication_failure_preserves_environment_credentials(self):
        session = _TransportSession([_TransportResponse(403, {"code": "access_denied"})])
        with mock.patch.dict(os.environ, {"YOYOCLAW_HMAC_ACCESS_KEY": "rejected-key"}, clear=True):
            client = YoYoClawClient(session=session)
        with mock.patch.object(client, "_login_payload", return_value={}):
            with self.assertRaises(YoYoClawError) as raised:
                client.login()
        self.assertEqual(raised.exception.category, "authentication")
        self.assertEqual(len(session.requests), 1)
        self.assertEqual(session.requests[0][2]["headers"]["X-HMAC-ACCESS-KEY"], "rejected-key")
        self.assertEqual(client.config.access_key, "rejected-key")
        self.assertFalse(client.authenticated)


class YoYoClawCatalogTests(unittest.TestCase):
    def test_catalog_exposes_only_the_preferred_models_for_each_role(self):
        client = _FakeYoYoClawClient(
            image_models=[
                {"name": "Qwen-Image-3.0-PC-YOYO"},
                {"name": "Qwen-Image-3.1-PC-YOYO"},
                {"name": "Doubao-Seedream-5.0-Pro-PC-YOYO", "default": True},
                {"name": "qwen-image-disabled", "available": False},
                {"name": "qwen-image-maintenance", "status": "maintenance"},
            ],
            geometry_models=[
                {"name": "Hy-3D-3.1-PC-YOYO"},
                {"name": "Hy-3D-3.2-PC-YOYO"},
                {"name": "Hy-3D-Texture-PC-YOYO", "default": True},
                {"name": "hy-3d-3.1-disabled", "enabled": False},
                {"name": "hy-3d-3.1-offline", "state": "offline"},
            ],
        )
        gateway = YoYoClawModelProviderGateway(lambda: client)

        image = gateway.catalog(IMAGE_ROLE)
        geometry = gateway.catalog(GEOMETRY_ROLE)

        self.assertEqual(image["state"], "ready")
        self.assertEqual([item["id"] for item in image["models"]], [PREFERRED_IMAGE_MODEL])
        self.assertEqual(image["default_model"], PREFERRED_IMAGE_MODEL)
        self.assertEqual([item["id"] for item in geometry["models"]], [PREFERRED_GEOMETRY_MODEL])
        self.assertEqual(geometry["default_model"], PREFERRED_GEOMETRY_MODEL)

    def test_catalog_uses_a_compatible_server_default_then_highest_version(self):
        client = _FakeYoYoClawClient(
            image_models=[
                {"name": "Qwen-Image-3.1-PC-YOYO"},
                {"name": "Qwen-Image-4.0-PC-YOYO"},
            ],
            image_default="Qwen-Image-3.1-PC-YOYO",
            geometry_models=[
                {"name": "Hy-3D-3.0-PC-YOYO"},
                {"name": "Hy-3D-3.2-PC-YOYO"},
                {"name": "Hy-3D-Texture-PC-YOYO"},
            ],
        )
        gateway = YoYoClawModelProviderGateway(lambda: client)

        image = gateway.catalog(IMAGE_ROLE)
        geometry = gateway.catalog(GEOMETRY_ROLE)

        self.assertEqual(image["default_model"], "image-model-library/Qwen-Image-3.1-PC-YOYO")
        self.assertEqual(geometry["default_model"], "3d-model-library/Hy-3D-3.2-PC-YOYO")

    def test_catalog_fails_closed_when_only_incompatible_models_exist(self):
        client = _FakeYoYoClawClient(
            image_models=[{"name": "Doubao-Seedream-5.0-Pro-PC-YOYO"}],
            geometry_models=[{"name": "Hy-3D-Texture-PC-YOYO"}, {"name": "other-3d"}],
        )
        gateway = YoYoClawModelProviderGateway(lambda: client)

        for role in (IMAGE_ROLE, GEOMETRY_ROLE):
            with self.subTest(role=role):
                catalog = gateway.catalog(role)
                self.assertEqual(catalog["state"], "empty")
                self.assertEqual(catalog["models"], [])
                self.assertEqual(catalog["default_model"], "")
                self.assertEqual(catalog["error"]["code"], "compatible_model_unavailable")

    def test_unconfigured_and_empty_catalogs_are_safe(self):
        unconfigured = YoYoClawModelProviderGateway(
            lambda: _FakeYoYoClawClient(configured=False)
        ).catalog(IMAGE_ROLE)
        self.assertEqual(unconfigured["state"], "not_configured")
        self.assertEqual(unconfigured["models"], [])

        empty = YoYoClawModelProviderGateway(
            lambda: _FakeYoYoClawClient(configured=True)
        ).catalog(GEOMETRY_ROLE)
        self.assertEqual(empty["state"], "empty")
        self.assertEqual(empty["models"], [])


class YoYoClawGenerationTests(unittest.TestCase):
    def test_image_generation_validates_whitelist_and_keeps_existing_output_path(self):
        client = _FakeYoYoClawClient(image_models=[{"name": "qwen-image-2.0"}])
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "design.png"
            result = generate_design_image(
                "qwen-image-2.0", "a printable vase", output,
                client_factory=lambda: client,
            )
            self.assertTrue(output.exists())
            self.assertEqual(output.read_bytes(), b"PNG")

        self.assertEqual(result, output)
        self.assertEqual(client.generate_calls[0][0], "image-model-library/qwen-image-2.0")

        with self.assertRaises(YoYoClawError) as raised:
            generate_design_image(
                "missing-image-model", "a vase", output,
                client_factory=lambda: client,
            )
        self.assertEqual(raised.exception.code, "model_unavailable")

    def test_geometry_requires_confirmed_2d_image_and_explicit_authorization(self):
        client = _FakeYoYoClawClient(geometry_models=[{"name": "hy-3d-3.1-yoyo"}])
        gateway = YoYoClawModelProviderGateway(lambda: client)

        with self.assertRaises(ProviderGatewayError) as raised:
            gateway.start_or_reuse_model_task(
                ModelTaskRequest(source="text", model_id="hy-3d-3.1-yoyo")
            )
        self.assertEqual(raised.exception.code, "invalid_model_request")
        self.assertEqual(client.submit_calls, [])

        with tempfile.TemporaryDirectory() as directory:
            reference = Path(directory) / "confirmed-design.png"
            reference.write_bytes(b"confirmed design")
            request = ModelTaskRequest(
                source="image", image_path=reference, model_id="hy-3d-3.1-yoyo"
            )

            with self.assertRaises(ProviderGatewayError) as raised:
                gateway.start_or_reuse_model_task(request)
            self.assertEqual(raised.exception.code, "authorization_required")
            self.assertEqual(client.submit_calls, [])

            authorization = PaidTaskAuthorization.confirmed("job-1:model:1", "yoyoclaw")
            result = gateway.start_or_reuse_model_task(request, authorization=authorization)

        self.assertEqual(result.task_id, "yoyoclaw-task-1")
        self.assertFalse(result.reused)
        self.assertEqual(len(client.submit_calls), 1)
        self.assertTrue(authorization.consumed)

    def test_existing_remote_task_is_reused_and_ambiguous_submission_is_reported(self):
        client = _FakeYoYoClawClient(
            geometry_models=[{"name": "hy-3d-3.1-yoyo"}],
            submit=YoYoClawError(
                "request accepted but response was lost",
                code="provider_connection_failed", category="network", ambiguous=True,
            ),
        )
        gateway = YoYoClawModelProviderGateway(lambda: client)
        reused = gateway.start_or_reuse_model_task(
            ModelTaskRequest(source="image"), existing_task_id="remote-task-42"
        )
        self.assertTrue(reused.reused)
        self.assertEqual(reused.task_id, "remote-task-42")
        self.assertEqual(client.submit_calls, [])

        with tempfile.TemporaryDirectory() as directory:
            reference = Path(directory) / "confirmed-design.png"
            reference.write_bytes(b"confirmed design")
            authorization = PaidTaskAuthorization.confirmed("job-2:model:1", "yoyoclaw")
            with self.assertRaises(ProviderGatewayError) as raised:
                gateway.start_or_reuse_model_task(
                    ModelTaskRequest(
                        source="image", image_path=reference, model_id="hy-3d-3.1-yoyo"
                    ),
                    authorization=authorization,
                )

        self.assertTrue(raised.exception.ambiguous)
        self.assertEqual(raised.exception.code, "provider_connection_failed")
        self.assertTrue(authorization.consumed)
        self.assertEqual(len(client.submit_calls), 1)


class YoYoClawTransportTests(unittest.TestCase):
    def test_download_reports_bytes_and_keeps_unknown_or_encoded_lengths_unknown(self):
        for headers, expected in (({"Content-Length": "4"}, 4), ({}, None),
                                  ({"Content-Length": "4", "Content-Encoding": "gzip"}, None)):
            with self.subTest(headers=headers), tempfile.TemporaryDirectory() as directory:
                response = _TransportResponse(200, {}, chunks=(b"ab", b"cd"), headers=headers)
                client = YoYoClawClient(session=_TransportSession([], download_response=response))
                reports = []
                destination = Path(directory) / "model.glb"
                client.download_result_artifact({"data": [{"url": "https://cdn.example.invalid/model.glb"}]},
                    destination, 4, progress=lambda received, total: reports.append((received, total)))
                self.assertEqual(destination.read_bytes(), b"abcd")
                self.assertEqual(reports, [(0, expected), (2, expected), (4, expected)])

    def test_download_failure_never_reports_a_complete_file(self):
        response = _TransportResponse(200, {}, chunks=(b"abc", b"def"), headers={"Content-Length": "6"})
        client = YoYoClawClient(session=_TransportSession([], download_response=response))
        reports = []
        with tempfile.TemporaryDirectory() as directory:
            destination = Path(directory) / "model.glb"
            with self.assertRaises(YoYoClawError):
                client.download("https://cdn.example.invalid/model.glb", destination, 4,
                                progress=lambda received, total: reports.append((received, total)))
            self.assertEqual(reports, [(0, 6), (3, 6)])
            self.assertFalse(destination.exists())
            self.assertFalse(destination.with_name("model.glb.part").exists())

    def test_transport_auth_catalog_generation_poll_and_download(self):
        session = _TransportSession([
            _TransportResponse(200, {"code": "OK", "data": {
                "access_token": "access-token", "refresh_token": "refresh-token", "expires_in": 3600,
            }}),
            _TransportResponse(200, {"code": "OK", "data": {
                "models": [{"name": "qwen-image-2.0"}], "default_model": "qwen-image-2.0",
            }}),
            _TransportResponse(200, {"code": "OK", "data": {
                "data": [{"url": "https://cdn.example.invalid/design.png"}],
            }}),
            _TransportResponse(200, {"code": "OK", "data": {"id": "task-1"}}),
            _TransportResponse(200, {"code": "OK", "data": {"status": "running", "progress": 25}}),
            _TransportResponse(200, {"code": "OK", "data": {
                "status": "completed", "data": [{"url": "https://cdn.example.invalid/model.glb"}],
            }}),
        ], _TransportResponse(200, {}, chunks=(b"gl", b"b")))
        client = YoYoClawClient(
            _configured_yoyoclaw_config(), session=session, monotonic=lambda: 0.0,
            sleep=lambda _seconds: None,
        )

        catalog = client.whitelist_catalog(IMAGE_LIBRARY)
        self.assertEqual(catalog["models"][0]["name"], "qwen-image-2.0")
        self.assertEqual(catalog["default_model"], "qwen-image-2.0")
        self.assertEqual(client.generate_image("image-model-library/qwen-image-2.0", "a vase"),
                         "https://cdn.example.invalid/design.png")
        with tempfile.TemporaryDirectory() as directory:
            reference = Path(directory) / "confirmed.png"
            reference.write_bytes(b"confirmed-design")
            self.assertEqual(client.submit_3d("3d-model-library/hy-3d-3.1", reference), "task-1")

            progress = []
            result = client.wait_3d(
                "3d-model-library/hy-3d-3.1", "task-1", progress=progress.append,
                timeout=10, interval=0,
            )
            self.assertEqual(result["status"], "completed")
            self.assertEqual(progress, [25])

            destination = Path(directory) / "model.glb"
            self.assertEqual(client.download("https://cdn.example.invalid/model.glb", destination, 4), destination)
            self.assertEqual(destination.read_bytes(), b"glb")

        self.assertEqual(len(session.requests), 6)
        login_method, login_url, login_kwargs = session.requests[0]
        self.assertEqual((login_method, login_url), ("POST", "https://yoyoclaw.example.invalid/yoyoclaw/auth/login"))
        self.assertIn("X-HMAC-SIGNATURE", login_kwargs["headers"])
        self.assertEqual(login_kwargs["headers"]["X-HMAC-ACCESS-KEY"], "access-key")
        self.assertTrue(session.gets[0][0].endswith("/model.glb"))

    def test_transport_rejects_unsafe_download_urls(self):
        client = YoYoClawClient(_configured_yoyoclaw_config(), session=_TransportSession([]))
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(YoYoClawError) as raised:
                client.download("file:///secret/model.glb", Path(directory) / "model.glb", 100)
        self.assertEqual(raised.exception.code, "unsafe_artifact")


class YoYoClawTaskQueryTests(unittest.TestCase):
    def client(self, responses, *, authenticated=True):
        clock = _Clock()
        session = _TransportSession(responses)
        client = YoYoClawClient(
            _configured_yoyoclaw_config(), session=session,
            monotonic=clock.monotonic, sleep=clock.sleep,
        )
        if authenticated:
            client._access_token = "private-access-token"
            client._expires_at = 3600
        return client, session, clock

    @staticmethod
    def completed():
        return _TransportResponse(200, {"code": "OK", "data": {"status": "completed"}})

    def test_non_json_and_network_errors_recover_using_only_the_same_task_get(self):
        from tools.ai.yoyoclaw_client import requests

        for failure in (
            _TransportResponse(200, ValueError("private response body")),
            _TransportResponse(503, ValueError("private response body")),
            _TransportResponse(429, {"code": "too_many_requests"}),
            requests.ConnectionError("private connection detail"),
            requests.Timeout("private timeout detail"),
        ):
            with self.subTest(failure=type(failure).__name__):
                client, session, clock = self.client([failure, self.completed()])
                result = client.wait_3d("3d-model-library/test-model", "saved-task")
                self.assertEqual(result["status"], "completed")
                self.assertEqual(clock.sleeps, [2])
                self.assertEqual([r[0] for r in session.requests], ["GET", "GET"])
                self.assertEqual(session.requests[0][1], session.requests[1][1])
                self.assertIn("/tasks/saved-task?", session.requests[0][1])
                self.assertEqual(session.gets, [])

    def test_consecutive_failure_budget_is_bounded_and_exhaustion_preserves_http_status(self):
        client, session, clock = self.client([
            _TransportResponse(503, ValueError("not JSON")) for _ in range(4)
        ])
        with self.assertRaises(YoYoClawError) as raised:
            client.wait_3d("model", "saved-task")
        self.assertEqual(clock.sleeps, [2, 4, 8])
        self.assertEqual(len(session.requests), 4)
        self.assertEqual(raised.exception.status, 503)
        self.assertEqual(raised.exception.category, "network")
        self.assertTrue(raised.exception.retryable)
        self.assertFalse(raised.exception.ambiguous)
        self.assertIn("resume the existing task", str(raised.exception))

    def test_valid_running_response_resets_consecutive_retry_budget(self):
        client, session, clock = self.client([
            *[_TransportResponse(502, None) for _ in range(3)],
            _TransportResponse(200, {"status": "running", "progress": 45}),
            *[_TransportResponse(504, None) for _ in range(3)],
            self.completed(),
        ])
        progress = []
        self.assertEqual(client.wait_3d("model", "saved-task", progress=progress.append)["status"], "completed")
        self.assertEqual(progress, [45])
        self.assertEqual(clock.sleeps, [2, 4, 8, 10, 2, 4, 8])
        self.assertEqual(len(session.requests), 8)

    def test_authentication_validation_and_terminal_failures_are_not_retried(self):
        for response, category in (
            (_TransportResponse(401, ValueError("not JSON")), "authentication"),
            (_TransportResponse(403, None), "authentication"),
            (_TransportResponse(400, {"code": "bad_request"}), "provider"),
            (_TransportResponse(404, ValueError("not JSON")), "provider"),
            (_TransportResponse(200, {"code": "invalid_model"}), "provider"),
            *[(_TransportResponse(200, {"status": status}), "provider")
              for status in ("failed", "error", "cancelled", "canceled", "expired")],
        ):
            with self.subTest(status=response.status_code, payload_type=type(response._payload).__name__):
                client, session, clock = self.client([response])
                with self.assertRaises(YoYoClawError) as raised:
                    client.wait_3d("model", "saved-task")
                self.assertEqual(raised.exception.category, category)
                self.assertFalse(raised.exception.retryable)
                self.assertEqual(len(session.requests), 1)
                self.assertEqual(clock.sleeps, [])

    def test_malformed_successful_status_responses_are_retried(self):
        for payload in (None, [], {"data": []}, {"status": " "}, {"status": ["completed"]}):
            with self.subTest(payload=payload):
                client, session, clock = self.client([_TransportResponse(200, payload), self.completed()])
                self.assertEqual(client.wait_3d("model", "saved-task")["status"], "completed")
                self.assertEqual(len(session.requests), 2)
                self.assertEqual(clock.sleeps, [2])

    def test_temporary_authentication_failure_retries_without_generation_submission(self):
        client, session, clock = self.client([
            _TransportResponse(503, ValueError("not JSON")),
            _TransportResponse(200, {"access_token": "private-access-token"}),
            self.completed(),
        ], authenticated=False)
        with mock.patch.object(client, "_login_payload", return_value={}):
            self.assertEqual(client.wait_3d("model", "saved-task")["status"], "completed")
        self.assertEqual(clock.sleeps, [2])
        self.assertEqual([r[0] for r in session.requests], ["POST", "POST", "GET"])
        self.assertTrue(all(r[1].endswith("/auth/login") for r in session.requests[:2]))
        self.assertIn("/tasks/saved-task?", session.requests[2][1])

    def test_temporary_refresh_failure_can_reauthenticate_and_continue_same_task(self):
        client, session, clock = self.client([
            _TransportResponse(502, ValueError("not JSON")),
            _TransportResponse(200, {"access_token": "new-access-token"}),
            self.completed(),
        ])
        client._refresh_token = "private-refresh-token"
        client._expires_at = 30
        with mock.patch.object(client, "_login_payload", return_value={}):
            self.assertEqual(client.wait_3d("model", "saved-task")["status"], "completed")
        self.assertTrue(session.requests[0][1].endswith("/auth/refresh"))
        self.assertTrue(session.requests[1][1].endswith("/auth/login"))
        self.assertIn("/tasks/saved-task?", session.requests[2][1])
        self.assertEqual(clock.sleeps, [2])

    def test_cancel_during_backoff_and_before_query(self):
        for already_cancelled in (True, False):
            with self.subTest(already_cancelled=already_cancelled):
                client, session, clock = self.client([_TransportResponse(503, None)])
                stop = threading.Event()
                if already_cancelled:
                    stop.set()
                with mock.patch.object(stop, "wait", side_effect=lambda _seconds: stop.set()) as wait:
                    with self.assertRaises(YoYoClawError) as raised:
                        client.wait_3d("model", "saved-task", stop_event=stop)
                    if not already_cancelled:
                        wait.assert_called_once_with(2)
                self.assertEqual(raised.exception.code, "provider_cancelled")
                self.assertEqual(len(session.requests), 0 if already_cancelled else 1)
                self.assertEqual(clock.sleeps, [])

    def test_cancel_while_request_is_in_flight_prevents_further_queries(self):
        client, session, clock = self.client([self.completed()])
        stop = threading.Event()
        request = session.request

        def cancel_in_request(*args, **kwargs):
            stop.set()
            return request(*args, **kwargs)

        session.request = cancel_in_request
        with self.assertRaises(YoYoClawError) as raised:
            client.wait_3d("model", "saved-task", stop_event=stop)
        self.assertEqual(raised.exception.code, "provider_cancelled")
        self.assertEqual(len(session.requests), 1)
        self.assertEqual(clock.sleeps, [])

    def test_expired_deadline_does_not_authenticate_or_query(self):
        client, session, clock = self.client([], authenticated=False)
        with self.assertRaises(YoYoClawError) as raised:
            client.wait_3d("model", "saved-task", timeout=0)
        self.assertEqual(raised.exception.code, "provider_timeout")
        self.assertEqual(session.requests, [])
        self.assertEqual(clock.sleeps, [])

    def test_backoff_and_regular_poll_wait_respect_remaining_deadline(self):
        for response in (_TransportResponse(503, None), _TransportResponse(200, {"status": "running"})):
            with self.subTest(status=response.status_code):
                client, session, clock = self.client([response])
                with self.assertRaises(YoYoClawError) as raised:
                    client.wait_3d("model", "saved-task", timeout=1)
                self.assertEqual(raised.exception.code, "provider_timeout")
                self.assertEqual(clock.sleeps, [1])
                self.assertEqual(len(session.requests), 1)
                self.assertEqual(session.requests[0][2]["timeout"], (0.5, 0.5))

    def test_authentication_uses_query_budget_and_cannot_start_get_after_deadline(self):
        client, session, clock = self.client([
            _TransportResponse(200, {"access_token": "private-access-token"}),
        ], authenticated=False)
        request = session.request

        def slow_auth(*args, **kwargs):
            response = request(*args, **kwargs)
            clock.now += 1
            return response

        session.request = slow_auth
        with mock.patch.object(client, "_login_payload", return_value={}):
            with self.assertRaises(YoYoClawError) as raised:
                client.wait_3d("model", "saved-task", timeout=1)
        self.assertEqual(raised.exception.code, "provider_timeout")
        self.assertEqual(len(session.requests), 1)
        self.assertTrue(session.requests[0][1].endswith("/auth/login"))
        self.assertEqual(session.requests[0][2]["timeout"], (0.5, 0.5))

    def test_failed_generation_submission_remains_single_and_ambiguous(self):
        for status in (200, 408, 503):
            with self.subTest(status=status):
                client, session, clock = self.client([_TransportResponse(status, ValueError("not JSON"))])
                with tempfile.TemporaryDirectory() as directory:
                    reference = Path(directory) / "confirmed.png"
                    reference.write_bytes(b"confirmed-design")
                    with self.assertRaises(YoYoClawError) as raised:
                        client.submit_3d("model", reference)
                self.assertTrue(raised.exception.ambiguous)
                self.assertFalse(raised.exception.retryable)
                self.assertEqual(len(session.requests), 1)
                self.assertEqual(session.requests[0][0], "POST")
                self.assertTrue(session.requests[0][1].endswith("/model/chat/completions"))
                self.assertEqual(clock.sleeps, [])

    def test_diagnostics_include_context_and_bounded_metadata_without_private_content(self):
        client, session, clock = self.client([
            _TransportResponse(503, ValueError("private-body"), headers={
                "Content-Type": "text/html; private-header=secret",
                "Content-Length": "123", "Set-Cookie": "private-cookie",
            }), self.completed(),
        ])
        output = io.StringIO()
        with mock.patch("sys.stderr", output), diagnostic_context("diagnostic-job"):
            client.wait_3d("private-query-model", "saved-task")
        encoded = output.getvalue()
        events = [json.loads(line) for line in encoded.splitlines()]
        self.assertEqual([e["event"] for e in events], [
            "yoyoclaw.request.failed", "yoyoclaw.task_query.retry", "yoyoclaw.task_query.recovered",
        ])
        failure = events[0]
        self.assertEqual(failure["http_status"], 503)
        self.assertEqual(failure["response_type"], "non_json")
        self.assertEqual(failure["content_type"], "text/html")
        self.assertEqual(failure["response_bytes"], 123)
        self.assertEqual(failure["method"], "GET")
        self.assertTrue(all(e["job_id"] == "diagnostic-job" for e in events))
        self.assertTrue(all(e["endpoint"].endswith("/tasks/saved-task") for e in events))
        for private in ("private-body", "private-header", "private-cookie", "private-query-model",
                        "private-access-token", "root-secret", "hmac-secret", "access-key"):
            self.assertNotIn(private, encoded)

    def test_untrusted_mime_length_and_error_code_are_not_copied_to_diagnostics(self):
        client, session, clock = self.client([
            _TransportResponse(400, {"code": "https://private.invalid/?secret=value"}, headers={
                "Content-Type": "private-mime", "Content-Length": "private-length",
            }),
        ])
        output = io.StringIO()
        with mock.patch("sys.stderr", output):
            with self.assertRaises(YoYoClawError) as raised:
                client.wait_3d("model", "saved-task")
        self.assertEqual(raised.exception.code, "provider_http_error")
        self.assertNotIn("private", str(raised.exception))
        event = json.loads(output.getvalue())
        self.assertEqual(event["content_type"], "other")
        self.assertNotIn("response_bytes", event)
        self.assertNotIn("private", output.getvalue())


if __name__ == "__main__":
    unittest.main()
