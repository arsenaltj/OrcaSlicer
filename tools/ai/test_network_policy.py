from __future__ import annotations

import io
import json
import os
import unittest
import urllib.request
from unittest import mock

from tools.ai import network_policy
from tools.ai import openai_preprocessor
from tools.ai import tripo_client


class NetworkPolicyTests(unittest.TestCase):
    def proxy_handler(self, opener: urllib.request.OpenerDirector) -> urllib.request.ProxyHandler:
        handlers = [handler for handler in opener.handlers if isinstance(handler, urllib.request.ProxyHandler)]
        self.assertEqual(len(handlers), 1)
        return handlers[0]

    def test_windows_no_proxy_does_not_suppress_registry_https_proxy(self) -> None:
        environment = {"no": "127.0.0.1,localhost"}
        registry = {
            "http": "http://registry-user:registry-secret@proxy.example:8080",
            "https": "http://registry-user:registry-secret@proxy.example:8080",
        }
        with mock.patch.object(network_policy, "_is_windows", return_value=True), \
                mock.patch.object(network_policy.urllib.request, "getproxies_environment", return_value=environment), \
                mock.patch.object(network_policy, "_windows_registry_proxies", return_value=registry), \
                mock.patch.object(network_policy.urllib.request, "proxy_bypass", return_value=False):
            opener = network_policy.build_opener()
            diagnostics = network_policy.network_diagnostics("https://provider.example/v1")

        self.assertEqual(self.proxy_handler(opener).proxies["https"], registry["https"])
        self.assertEqual(
            diagnostics,
            {"source": "windows_registry", "schemes": ["http", "https"], "bypass": False},
        )
        serialized = json.dumps(diagnostics)
        self.assertNotIn("registry-user", serialized)
        self.assertNotIn("registry-secret", serialized)
        self.assertNotIn("proxy.example", serialized)

    def test_windows_environment_proxy_wins_and_registry_fills_missing_scheme(self) -> None:
        environment = {"https": "http://environment.example:8443", "no": "localhost"}
        registry = {
            "http": "http://registry.example:8080",
            "https": "http://ignored.example:8080",
        }
        with mock.patch.object(network_policy, "_is_windows", return_value=True), \
                mock.patch.object(network_policy.urllib.request, "getproxies_environment", return_value=environment), \
                mock.patch.object(network_policy, "_windows_registry_proxies", return_value=registry), \
                mock.patch.object(network_policy.urllib.request, "proxy_bypass", return_value=False):
            opener = network_policy.build_opener()
            diagnostics = network_policy.network_diagnostics("https://provider.example/v1")

        proxies = self.proxy_handler(opener).proxies
        self.assertEqual(proxies["https"], environment["https"])
        self.assertEqual(proxies["http"], registry["http"])
        self.assertEqual(diagnostics["source"], "environment+windows_registry")
        self.assertEqual(diagnostics["schemes"], ["http", "https"])

    def test_non_windows_uses_urllib_default_proxy_semantics(self) -> None:
        defaults = {"https": "http://default.example:8080", "no": "localhost"}
        with mock.patch.object(network_policy, "_is_windows", return_value=False), \
                mock.patch.object(network_policy.urllib.request, "getproxies", return_value=defaults) as getproxies, \
                mock.patch.object(network_policy, "_windows_registry_proxies") as registry, \
                mock.patch.object(network_policy.urllib.request, "proxy_bypass", return_value=False):
            opener = network_policy.build_opener()
            diagnostics = network_policy.network_diagnostics("https://provider.example/v1")

        getproxies.assert_called()
        registry.assert_not_called()
        self.assertEqual(self.proxy_handler(opener).proxies["https"], defaults["https"])
        self.assertEqual(diagnostics, {"source": "urllib", "schemes": ["https"], "bypass": False})

    def test_diagnostics_report_direct_bypass_without_exposing_proxy_value(self) -> None:
        proxy = "http://user:top-secret@proxy.example:8080"
        with mock.patch.object(network_policy, "_is_windows", return_value=True), \
                mock.patch.object(network_policy.urllib.request, "getproxies_environment", return_value={"https": proxy}), \
                mock.patch.object(network_policy, "_windows_registry_proxies", return_value={}), \
                mock.patch.object(network_policy.urllib.request, "proxy_bypass", return_value=True):
            diagnostics = network_policy.network_diagnostics("https://provider.example/v1")

        self.assertEqual(diagnostics, {"source": "environment", "schemes": ["https"], "bypass": True})
        serialized = json.dumps(diagnostics)
        for secret in ("user", "top-secret", "proxy.example", "8080"):
            self.assertNotIn(secret, serialized)

    def test_build_opener_keeps_explicit_handlers(self) -> None:
        redirects = urllib.request.HTTPRedirectHandler()
        with mock.patch.object(network_policy, "_is_windows", return_value=False), \
                mock.patch.object(network_policy.urllib.request, "getproxies", return_value={}):
            opener = network_policy.build_opener(redirects)

        self.assertIn(redirects, opener.handlers)

    def test_openai_request_uses_network_opener_and_safe_metadata(self) -> None:
        response = mock.MagicMock()
        payload = b'{"ok":true}'
        response.status = 200
        response.headers = {"Content-Length": str(len(payload))}
        response.read = io.BytesIO(payload).read
        response.__enter__.return_value = response
        opener = mock.Mock()
        opener.open.return_value = response
        diagnostics = {"source": "windows_registry", "schemes": ["https"], "bypass": False}
        with mock.patch.dict(
            os.environ,
            {"OPENAI_API_KEY": "test-key", "OPENAI_BASE_URL": "https://provider.example/v1"},
        ), mock.patch.object(openai_preprocessor, "build_network_opener", return_value=opener) as build, \
                mock.patch.object(openai_preprocessor, "network_diagnostics", return_value=diagnostics), \
                mock.patch.object(openai_preprocessor, "diagnostic_event") as event:
            result = openai_preprocessor._provider_request("/responses", b"{}", "application/json")

        self.assertEqual(result, {"ok": True})
        build.assert_called_once()
        started = next(call for call in event.call_args_list if call.args[0] == "provider.request.started")
        self.assertEqual(started.kwargs["network"], diagnostics)
        self.assertNotIn("test-key", json.dumps(started.kwargs))

    def test_tripo_request_uses_network_opener_and_safe_metadata(self) -> None:
        response = mock.MagicMock()
        payload = b'{"code":0,"data":{"task_id":"task-1"}}'
        response.status = 200
        response.headers = {"Content-Length": str(len(payload))}
        response.read = io.BytesIO(payload).read
        response.__enter__.return_value = response
        opener = mock.Mock()
        opener.open.return_value = response
        diagnostics = {"source": "environment", "schemes": ["https"], "bypass": False}
        with mock.patch.dict(
            os.environ,
            {"TRIPO_API_KEY": "test-key", "TRIPO_API_BASE": "https://tripo.example/v3"},
        ), mock.patch.object(tripo_client, "build_network_opener", return_value=opener) as build, \
                mock.patch.object(tripo_client, "network_diagnostics", return_value=diagnostics), \
                mock.patch.object(tripo_client, "diagnostic_event") as event:
            result = tripo_client._request("GET", "/tasks/task-1")

        self.assertEqual(result["data"]["task_id"], "task-1")
        build.assert_called_once()
        started = next(call for call in event.call_args_list if call.args[0] == "tripo.request.started")
        self.assertEqual(started.kwargs["network"], diagnostics)
        self.assertNotIn("test-key", json.dumps(started.kwargs))


if __name__ == "__main__":
    unittest.main()
