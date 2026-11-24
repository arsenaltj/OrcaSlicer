from __future__ import annotations

import io
import http.client
import json
import os
import socket
import ssl
from pathlib import Path
import tempfile
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

    def test_tripo_https_download_uses_the_registry_proxy_without_resolving_the_target(self) -> None:
        target = "openapi.cdn.tripo3d.com"
        signed_path = "/model.glb?signature=private-download-signature"
        connection = mock.MagicMock()
        connection.makefile.side_effect = [
            io.BytesIO(b"HTTP/1.0 200 Connection established\r\n\r\n"),
            io.BytesIO(b"HTTP/1.1 200 OK\r\nContent-Length: 8\r\n\r\nglTFtest"),
        ]
        def resolve(host, port, *args, **kwargs):
            if host == target:
                raise socket.gaierror(11002, "local target resolution unavailable")
            self.assertEqual((host, port), ("proxy.example", 8080))
            return [(socket.AF_INET, socket.SOCK_STREAM, 6, "", ("127.0.0.1", 8080))]
        with tempfile.TemporaryDirectory() as directory, \
                mock.patch.object(network_policy, "_is_windows", return_value=True), \
                mock.patch.object(network_policy.urllib.request, "getproxies_environment", return_value={}), \
                mock.patch.object(network_policy, "_windows_registry_proxies",
                                  return_value={"https": "http://proxy.example:8080"}), \
                mock.patch.object(network_policy.urllib.request, "proxy_bypass", return_value=False), \
                mock.patch.object(socket, "getaddrinfo", side_effect=resolve) as lookup, \
                mock.patch.object(socket, "socket", return_value=connection), \
                mock.patch.object(ssl.SSLContext, "wrap_socket", return_value=connection) as tls, \
                mock.patch.object(tripo_client, "diagnostic_event") as event:
            output = tripo_client.download_task_artifact(
                {"output": {"model_url": "https://" + target + signed_path}}, Path(directory) / "model.glb")
            self.assertEqual(output.read_bytes(), b"glTFtest")
        self.assertEqual(lookup.call_count, 1)
        self.assertEqual(lookup.call_args.args[:2], ("proxy.example", 8080))
        self.assertEqual(tls.call_args.kwargs["server_hostname"], target)
        sent = b"".join(call.args[0] for call in connection.sendall.call_args_list)
        self.assertIn(b"CONNECT openapi.cdn.tripo3d.com:443", sent)
        self.assertIn(("GET " + signed_path).encode(), sent)
        completed = next(call for call in event.call_args_list if call.args[0] == "tripo.artifact_download.completed")
        self.assertEqual(completed.kwargs["network"],
                         {"source": "windows_registry", "schemes": ["https"], "bypass": False})
        self.assertNotIn("private-download-signature", str(event.call_args_list))

    def test_tripo_upload_retries_connection_reset_before_paid_creation(self) -> None:
        payload = b'{"code":0,"data":{"file_token":"upload-token"}}'
        response = mock.MagicMock()
        response.status = 200
        response.headers = {"Content-Length": str(len(payload))}
        response.read = io.BytesIO(payload).read
        response.__enter__.return_value = response
        opener = mock.Mock()
        opener.open.side_effect = [urllib.error.URLError(OSError("connection reset")), response]
        diagnostics = {"source": "environment", "schemes": ["https"], "bypass": False}
        with tempfile.TemporaryDirectory() as directory:
            reference = Path(directory) / "reference.png"
            reference.write_bytes(b"\x89PNG\r\n\x1a\nreference")
            with mock.patch.dict(
                os.environ,
                {"TRIPO_API_KEY": "test-key", "TRIPO_API_BASE": "https://tripo.example/v3"},
            ), mock.patch.object(tripo_client, "build_network_opener", return_value=opener), \
                    mock.patch.object(tripo_client, "network_diagnostics", return_value=diagnostics), \
                    mock.patch.object(tripo_client, "diagnostic_event") as event, \
                    mock.patch.object(tripo_client.time, "sleep") as sleep:
                self.assertEqual(tripo_client.upload_image(reference), "upload-token")

        self.assertEqual(opener.open.call_count, 2)
        sleep.assert_called_once()
        started = [call for call in event.call_args_list if call.args[0] == "tripo.request.started"]
        self.assertEqual([call.kwargs["attempt"] for call in started], [1, 2])
        self.assertTrue(all(call.kwargs["max_attempts"] == 2 for call in started))
        self.assertNotIn("test-key", json.dumps(event.call_args_list))

    def test_tripo_upload_retries_truncated_http_responses(self) -> None:
        payload = b'{"code":0,"data":{"file_token":"upload-token"}}'
        for interruption in (None, http.client.IncompleteRead(b'{"code":0', 40), "missing_length"):
            with self.subTest(interruption=type(interruption).__name__):
                interrupted = mock.MagicMock()
                interrupted.status = 200
                interrupted.headers = {} if interruption == "missing_length" else {"Content-Length": str(len(payload))}
                interrupted.__enter__.return_value = interrupted
                if interruption is None or interruption == "missing_length":
                    interrupted.read.return_value = payload[:10]
                else:
                    interrupted.read.side_effect = interruption
                complete = mock.MagicMock()
                complete.status = 200
                complete.headers = {"Content-Length": str(len(payload))}
                complete.read.return_value = payload
                complete.__enter__.return_value = complete
                opener = mock.Mock()
                opener.open.side_effect = [interrupted, complete]
                with tempfile.TemporaryDirectory() as directory:
                    reference = Path(directory) / "reference.png"
                    reference.write_bytes(b"\x89PNG\r\n\x1a\nreference")
                    with mock.patch.dict(os.environ, {"TRIPO_API_KEY": "test-key"}), \
                            mock.patch.object(tripo_client, "build_network_opener", return_value=opener), \
                            mock.patch.object(tripo_client, "diagnostic_event"), \
                            mock.patch.object(tripo_client.time, "sleep"):
                        self.assertEqual(tripo_client.upload_image(reference), "upload-token")
                self.assertEqual(opener.open.call_count, 2)

    def test_tripo_paid_creation_does_not_retry_interrupted_response(self) -> None:
        for mask in range(8):
            tokens = {"front": "front-token", **{view: f"{view}-token"
                for slot, view in enumerate(("left", "back", "right")) if mask & (1 << slot)}}
            for failure in (http.client.RemoteDisconnected("connection closed"), urllib.error.URLError(TimeoutError("timed out"))):
                with self.subTest(views=list(tokens), failure=type(failure).__name__):
                    opener = mock.Mock()
                    opener.open.side_effect = failure
                    with mock.patch.dict(os.environ, {"TRIPO_API_KEY": "test-key"}), \
                            mock.patch.object(tripo_client, "build_network_opener", return_value=opener), \
                            mock.patch.object(tripo_client, "diagnostic_event"), \
                            mock.patch.object(tripo_client.time, "sleep") as sleep:
                        with self.assertRaises(tripo_client.TripoError):
                            if mask == 0: tripo_client.create_image_task(tokens["front"])
                            else: tripo_client.create_multiview_task(tokens)
                    opener.open.assert_called_once()
                    sleep.assert_not_called()

    def test_tripo_paid_creation_does_not_retry_truncated_json(self) -> None:
        for mask in range(8):
            tokens = {"front": "front-token", **{view: f"{view}-token"
                for slot, view in enumerate(("left", "back", "right")) if mask & (1 << slot)}}
            for payload in (b'{"code":0,"data":', b'{"code":0,"data":{}}'):
                with self.subTest(views=list(tokens), payload=payload):
                    interrupted = mock.MagicMock()
                    interrupted.status = 200
                    interrupted.headers = {}
                    interrupted.read.return_value = payload
                    interrupted.__enter__.return_value = interrupted
                    opener = mock.Mock()
                    opener.open.return_value = interrupted
                    with mock.patch.dict(os.environ, {"TRIPO_API_KEY": "test-key"}), \
                            mock.patch.object(tripo_client, "build_network_opener", return_value=opener), \
                            mock.patch.object(tripo_client, "diagnostic_event"), \
                            mock.patch.object(tripo_client.time, "sleep") as sleep:
                        with self.assertRaisesRegex(tripo_client.TripoError, "invalid response|task reference"):
                            if mask == 0: tripo_client.create_image_task(tokens["front"])
                            else: tripo_client.create_multiview_task(tokens)
                    opener.open.assert_called_once()
                    sleep.assert_not_called()

    def test_tripo_timeout_is_reported_as_timeout(self) -> None:
        opener = mock.Mock()
        opener.open.side_effect = urllib.error.URLError(TimeoutError("timed out"))
        with mock.patch.dict(os.environ, {"TRIPO_API_KEY": "test-key"}), \
                mock.patch.object(tripo_client, "build_network_opener", return_value=opener), \
                mock.patch.object(tripo_client, "diagnostic_event"):
            with self.assertRaisesRegex(tripo_client.TripoError, "timed out"):
                tripo_client._request("GET", "/tasks/existing-task")

    def test_tripo_task_observers_recover_transient_errors_only_on_the_original_get(self) -> None:
        def response(payload: bytes):
            value = mock.MagicMock()
            value.status = 200
            value.headers = {"Content-Length": str(len(payload))}
            value.read.return_value = payload
            value.__enter__.return_value = value
            return value

        task_id = "original/task reference"
        endpoint = "https://tripo.example/v3/tasks/original%2Ftask%20reference"
        failures = (
            urllib.error.URLError(TimeoutError("timed out")),
            http.client.RemoteDisconnected("connection closed"),
            urllib.error.HTTPError(endpoint, 503, "temporarily unavailable", {}, None),
            response(b'{"code":0,"data":'),
            response(b"<html>temporary proxy response</html>"),
        )
        for observe in (tripo_client.get_task, tripo_client.wait_for_task):
            for failure in failures:
                with self.subTest(observer=observe.__name__, failure=type(failure).__name__):
                    completed = {"task_id": task_id, "status": "success", "progress": 100}
                    steps = [failure]
                    if observe is tripo_client.wait_for_task:
                        steps.append(response(b'{"code":0,"data":{"status":"running","progress":25}}'))
                    steps.append(response(json.dumps({"code": 0, "data": completed}).encode()))
                    opener = mock.Mock()
                    opener.open.side_effect = steps
                    with mock.patch.dict(os.environ, {"TRIPO_API_KEY": "test-key",
                                                      "TRIPO_API_BASE": "https://tripo.example/v3"}), \
                            mock.patch.object(tripo_client, "build_network_opener", return_value=opener), \
                            mock.patch.object(tripo_client, "diagnostic_event"), \
                            mock.patch.object(tripo_client.time, "sleep"):
                        self.assertEqual(observe(task_id), completed)
                    self.assertEqual(opener.open.call_count, len(steps))
                    for call in opener.open.call_args_list:
                        request = call.args[0]
                        self.assertEqual(request.get_method(), "GET")
                        self.assertEqual(request.full_url, endpoint)
                        self.assertIsNone(request.data)

    def test_tripo_task_polling_exhaustion_or_stop_never_creates_another_task(self) -> None:
        for outcome in ("exhausted", "stopped"):
            with self.subTest(outcome=outcome):
                opener = mock.Mock()
                opener.open.side_effect = urllib.error.URLError(TimeoutError("timed out"))
                stop = mock.Mock()
                stop.is_set.return_value = False
                stop.wait.return_value = outcome == "stopped"
                with mock.patch.dict(os.environ, {"TRIPO_API_KEY": "test-key"}), \
                        mock.patch.object(tripo_client, "build_network_opener", return_value=opener), \
                        mock.patch.object(tripo_client, "diagnostic_event"):
                    message = "cancelled" if outcome == "stopped" else "timed out"
                    with self.assertRaisesRegex(tripo_client.TripoError, message):
                        tripo_client.wait_for_task("original-task", stop_event=stop)
                self.assertEqual(opener.open.call_count, 1 if outcome == "stopped" else 4)
                for call in opener.open.call_args_list:
                    self.assertEqual(call.args[0].get_method(), "GET")
                    self.assertTrue(call.args[0].full_url.endswith("/tasks/original-task"))


if __name__ == "__main__":
    unittest.main()
