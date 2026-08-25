#!/usr/bin/env python3
import unittest

from tools.ai.model_provider_gateway import (
    PaidTaskAuthorization,
    ProviderGatewayError,
    provider_policy,
)


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


if __name__ == "__main__":
    unittest.main()
