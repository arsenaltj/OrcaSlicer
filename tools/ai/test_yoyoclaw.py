#!/usr/bin/env python3
import os
from pathlib import Path
from types import SimpleNamespace
import tempfile
import unittest
from unittest import mock

from tools.ai.model_provider_gateway import (
    ModelTaskRequest,
    PaidTaskAuthorization,
    ProviderGatewayError,
)
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
    def __init__(self, status_code, payload, chunks=()):
        self.status_code = status_code
        self._payload = payload
        self._chunks = tuple(chunks)

    def json(self):
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
        return self.responses.pop(0)

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


if __name__ == "__main__":
    unittest.main()
