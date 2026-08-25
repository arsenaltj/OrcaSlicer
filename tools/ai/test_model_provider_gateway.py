#!/usr/bin/env python3
import os
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from tools.ai.model_provider_gateway import (
    ModelProviderGateway,
    ModelTaskRequest,
    PaidTaskAuthorization,
    ProviderGatewayError,
    provider_policy,
)
from tools.ai.tripo_client import TripoError


class ProviderPolicyTests(unittest.TestCase):
    def test_quality_first_provider_order_has_no_automatic_fallback(self):
        policy = provider_policy()

        self.assertEqual(policy.design_providers, ("gpt", "image2"))
        self.assertEqual(policy.geometry_provider, "tripo")
        self.assertFalse(policy.automatic_fallback)
        self.assertEqual(policy.max_paid_model_tasks_per_confirmation, 1)


class PaidTaskAuthorizationTests(unittest.TestCase):
    def test_confirmed_authorization_is_consumed_once(self):
        authorization = PaidTaskAuthorization.confirmed("job-1:model:1")

        authorization.consume("tripo", "model_generation")

        with self.assertRaises(ProviderGatewayError) as raised:
            authorization.consume("tripo", "model_generation")
        self.assertEqual(raised.exception.code, "authorization_consumed")
        self.assertEqual(raised.exception.category, "authorization")
        self.assertFalse(raised.exception.retryable)
        self.assertFalse(raised.exception.ambiguous)

    def test_authorization_rejects_wrong_provider_without_being_consumed(self):
        authorization = PaidTaskAuthorization.confirmed("job-1:model:1")

        with self.assertRaises(ProviderGatewayError) as raised:
            authorization.consume("image2", "model_generation")
        self.assertEqual(raised.exception.code, "authorization_scope_mismatch")

        authorization.consume("tripo", "model_generation")

    def test_authorization_rejects_wrong_operation_without_being_consumed(self):
        authorization = PaidTaskAuthorization.confirmed("job-1:model:1")

        with self.assertRaises(ProviderGatewayError) as raised:
            authorization.consume("tripo", "design_image")
        self.assertEqual(raised.exception.code, "authorization_scope_mismatch")

        authorization.consume("tripo", "model_generation")

    def test_blank_request_id_is_rejected(self):
        with self.assertRaises(ProviderGatewayError) as raised:
            PaidTaskAuthorization.confirmed("   ")

        self.assertEqual(raised.exception.code, "invalid_authorization")


class ModelTaskGatewayTests(unittest.TestCase):
    def gateway(self, **overrides):
        dependencies = {
            "create_text_task": mock.Mock(return_value="text-task"),
            "upload_image": mock.Mock(return_value="image-token"),
            "create_image_task": mock.Mock(return_value="image-task"),
            "create_conversion": mock.Mock(return_value="conversion-task"),
            "wait_for_task": mock.Mock(return_value={"status": "success"}),
            "download_task_artifact": mock.Mock(return_value=Path("artifact.obj")),
        }
        dependencies.update(overrides)
        return ModelProviderGateway(**dependencies), dependencies

    def test_existing_task_is_reused_without_authorization_or_provider_calls(self):
        gateway, dependencies = self.gateway()

        result = gateway.start_or_reuse_model_task(
            ModelTaskRequest(source="text", prompt="printable radio", face_limit=300000),
            existing_task_id="existing-task",
        )

        self.assertEqual(result.provider, "tripo")
        self.assertEqual(result.task_id, "existing-task")
        self.assertTrue(result.reused)
        for dependency in dependencies.values():
            dependency.assert_not_called()

    def test_existing_conversion_is_reused_without_provider_call(self):
        gateway, dependencies = self.gateway()

        result = gateway.start_or_reuse_conversion(
            "generation-task", "obj", existing_task_id="existing-conversion", allow_create=False
        )

        self.assertEqual(result.task_id, "existing-conversion")
        self.assertTrue(result.reused)
        dependencies["create_conversion"].assert_not_called()

    def test_conversion_creation_requires_explicit_permission(self):
        gateway, dependencies = self.gateway()

        with self.assertRaises(ProviderGatewayError) as raised:
            gateway.start_or_reuse_conversion("generation-task", "obj", allow_create=False)

        self.assertEqual(raised.exception.code, "conversion_creation_not_allowed")
        dependencies["create_conversion"].assert_not_called()

    def test_conversion_is_created_once_when_explicitly_allowed(self):
        gateway, dependencies = self.gateway()

        result = gateway.start_or_reuse_conversion("generation-task", "obj", allow_create=True)

        self.assertEqual(result.task_id, "conversion-task")
        self.assertFalse(result.reused)
        dependencies["create_conversion"].assert_called_once_with("generation-task", "obj")

    def test_wait_delegates_cancellation_and_progress_without_new_task(self):
        gateway, dependencies = self.gateway()
        stop_event = object()
        progress = mock.Mock()

        result = gateway.wait_for_task("existing-task", stop_event=stop_event, progress=progress)

        self.assertEqual(result, {"status": "success"})
        dependencies["wait_for_task"].assert_called_once_with(
            "existing-task", stop_event=stop_event, progress=progress
        )
        dependencies["create_text_task"].assert_not_called()
        dependencies["create_image_task"].assert_not_called()

    def test_download_delegates_the_byte_limit(self):
        gateway, dependencies = self.gateway()
        task_result = {"output": {"model_url": "https://example.invalid/model.obj"}}
        destination = Path("bounded-artifact.download")

        result = gateway.download_artifact(task_result, destination, 12345)

        self.assertEqual(result, Path("artifact.obj"))
        dependencies["download_task_artifact"].assert_called_once_with(task_result, destination, 12345)

    def test_unsafe_artifact_error_is_classified_without_model_fallback(self):
        download = mock.Mock(side_effect=TripoError("Tripo returned an unsafe artifact location."))
        gateway, dependencies = self.gateway(download_task_artifact=download)

        with self.assertRaises(ProviderGatewayError) as raised:
            gateway.download_artifact({}, Path("artifact.download"), 100)

        self.assertEqual(raised.exception.code, "unsafe_artifact")
        self.assertEqual(raised.exception.category, "security")
        self.assertFalse(raised.exception.retryable)
        self.assertFalse(raised.exception.ambiguous)
        dependencies["create_text_task"].assert_not_called()
        dependencies["create_image_task"].assert_not_called()

    def test_text_task_requires_and_consumes_explicit_authorization(self):
        gateway, dependencies = self.gateway()
        authorization = PaidTaskAuthorization.confirmed("job-1:model:1")

        result = gateway.start_or_reuse_model_task(
            ModelTaskRequest(source="text", prompt="printable radio", face_limit=300000),
            authorization=authorization,
        )

        self.assertEqual(result.task_id, "text-task")
        self.assertFalse(result.reused)
        dependencies["create_text_task"].assert_called_once_with("printable radio", 300000)
        dependencies["upload_image"].assert_not_called()
        dependencies["create_image_task"].assert_not_called()
        with self.assertRaises(ProviderGatewayError) as raised:
            gateway.start_or_reuse_model_task(
                ModelTaskRequest(source="text", prompt="second task", face_limit=300000),
                authorization=authorization,
            )
        self.assertEqual(raised.exception.code, "authorization_consumed")
        self.assertEqual(dependencies["create_text_task"].call_count, 1)

    def test_image_task_uploads_one_reference_then_creates_one_task(self):
        gateway, dependencies = self.gateway()
        authorization = PaidTaskAuthorization.confirmed("job-2:model:1")
        with tempfile.TemporaryDirectory() as directory:
            reference = Path(directory) / "reference.png"
            reference.write_bytes(b"preview")

            result = gateway.start_or_reuse_model_task(
                ModelTaskRequest(source="image", image_path=reference, face_limit=500000),
                authorization=authorization,
            )

        self.assertEqual(result.task_id, "image-task")
        dependencies["upload_image"].assert_called_once_with(reference)
        dependencies["create_image_task"].assert_called_once_with("image-token", 500000)
        dependencies["create_text_task"].assert_not_called()

    def test_missing_authorization_performs_no_provider_call(self):
        gateway, dependencies = self.gateway()

        with self.assertRaises(ProviderGatewayError) as raised:
            gateway.start_or_reuse_model_task(
                ModelTaskRequest(source="text", prompt="printable radio", face_limit=300000)
            )

        self.assertEqual(raised.exception.code, "authorization_required")
        for dependency in dependencies.values():
            dependency.assert_not_called()

    def test_provider_failure_is_classified_without_fallback_or_retry(self):
        create_text = mock.Mock(side_effect=TripoError("Could not connect to Tripo."))
        gateway, dependencies = self.gateway(create_text_task=create_text)

        with self.assertRaises(ProviderGatewayError) as raised:
            gateway.start_or_reuse_model_task(
                ModelTaskRequest(source="text", prompt="printable radio", face_limit=300000),
                authorization=PaidTaskAuthorization.confirmed("job-3:model:1"),
            )

        self.assertEqual(raised.exception.code, "provider_unavailable")
        self.assertEqual(raised.exception.category, "availability")
        self.assertTrue(raised.exception.retryable)
        self.assertTrue(raised.exception.ambiguous)
        create_text.assert_called_once_with("printable radio", 300000)
        dependencies["upload_image"].assert_not_called()
        dependencies["create_image_task"].assert_not_called()

    def test_configuration_availability_is_read_without_provider_call(self):
        gateway, dependencies = self.gateway()

        with mock.patch.dict(os.environ, {"TRIPO_API_KEY": ""}, clear=False):
            self.assertFalse(gateway.model_generation_available())
        with mock.patch.dict(os.environ, {"TRIPO_API_KEY": "configured"}, clear=False):
            self.assertTrue(gateway.model_generation_available())

        for dependency in dependencies.values():
            dependency.assert_not_called()


if __name__ == "__main__":
    unittest.main()
