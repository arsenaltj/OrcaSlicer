from __future__ import annotations

import io
import json
import socket
import ssl
import unittest
import urllib.error
from unittest import mock

from tools.ai import ai_diagnostics


class AIDiagnosticsTests(unittest.TestCase):
    def test_safe_endpoint_removes_credentials_query_and_fragment(self) -> None:
        self.assertEqual(
            ai_diagnostics.safe_endpoint("https://user:secret@gateway.example:8443/openai/v1?api_key=hidden#part"),
            "https://gateway.example:8443/openai/v1",
        )

    def test_exception_details_preserves_network_reason_without_secrets(self) -> None:
        error = urllib.error.URLError(socket.gaierror(11001, "host failed token=very-secret"))
        details = ai_diagnostics.exception_details(error)
        self.assertEqual([item["type"] for item in details], ["URLError", "gaierror"])
        self.assertNotIn("very-secret", json.dumps(details))
        self.assertIn("<redacted>", json.dumps(details))

    def test_connection_failure_classification_distinguishes_common_causes(self) -> None:
        self.assertEqual(
            ai_diagnostics.classify_connection_error(urllib.error.URLError(socket.gaierror(11001, "missing"))),
            "dns",
        )
        self.assertEqual(
            ai_diagnostics.classify_connection_error(urllib.error.URLError(TimeoutError("timed out"))),
            "timeout",
        )
        self.assertEqual(
            ai_diagnostics.classify_connection_error(ssl.SSLCertVerificationError("certificate verify failed")),
            "tls_certificate",
        )
        self.assertEqual(
            ai_diagnostics.classify_connection_error(urllib.error.URLError(OSError("Tunnel connection failed"))),
            "proxy",
        )

    def test_event_includes_job_id_and_redacts_provider_data(self) -> None:
        stream = io.StringIO()
        with mock.patch.object(ai_diagnostics.sys, "stderr", stream):
            with ai_diagnostics.diagnostic_context("job-123"):
                ai_diagnostics.event(
                    "provider.connection.failed",
                    endpoint=ai_diagnostics.safe_endpoint("https://user:pw@example.test/v1?token=secret"),
                    detail="Authorization: Bearer sk-super-secret-value",
                )
        payload = json.loads(stream.getvalue())
        self.assertEqual(payload["job_id"], "job-123")
        self.assertEqual(payload["endpoint"], "https://example.test/v1")
        self.assertNotIn("super-secret", stream.getvalue())
        self.assertIn("<redacted>", payload["detail"])

    def test_event_write_failure_is_non_fatal(self) -> None:
        with mock.patch.object(ai_diagnostics.sys, "stderr", None):
            ai_diagnostics.event("diagnostics.unavailable", value="safe")


if __name__ == "__main__":
    unittest.main()
