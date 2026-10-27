"""YoYoClaw transport for the Orca AI sidecar.

The client deliberately does not retry generation submissions.  A transport
failure after a paid request has been accepted is ambiguous, so callers must
persist the task identity (when one is returned) and resume it instead of
submitting a second task.
"""
from __future__ import annotations

import base64
from contextlib import nullcontext
from dataclasses import dataclass
from datetime import datetime, timezone
import hashlib
import hmac
import json
import os
from pathlib import Path
import secrets
import time
from email.utils import format_datetime
from typing import Any, Callable, Mapping
from urllib.parse import quote, urlencode, urlsplit

try:
    import requests
except ImportError:  # pragma: no cover - exercised by the minimal bundled runtime
    requests = None  # type: ignore[assignment]

try:
    from cryptography.hazmat.primitives.ciphers.aead import AESGCM
except ImportError:  # pragma: no cover - exercised by the minimal bundled runtime
    AESGCM = None  # type: ignore[assignment,misc]


class _TextRequestTiming:
    def __init__(self, diagnostics: TextRequestDiagnostics, stage: str) -> None:
        self._diagnostics = diagnostics
        self._stage = stage

    def __enter__(self) -> None:
        self._started = self._diagnostics.clock()

    def __exit__(self, exc_type, exc_value, traceback) -> None:
        # Do not rewrite the traceback: application errors may be frozen dataclasses.
        self._diagnostics.record(self._stage, **{self._stage + "_ms": self._diagnostics.elapsed_ms(self._started)})


class TextRequestDiagnostics:
    """Opt-in timings and allowlisted metadata; never retain message content."""

    def __init__(self, observer: Callable[[str, dict[str, Any]], None], *, monotonic=time.monotonic):
        self.clock = monotonic
        self.metrics: dict[str, Any] = {}
        self._observer = observer

    def elapsed_ms(self, started: float) -> int:
        return max(0, round((self.clock() - started) * 1000))

    def record(self, stage: str, **fields: Any) -> None:
        self.metrics.update(fields)
        try:
            self._observer(stage, fields)
        except Exception:
            pass  # Diagnostics must not affect a paid request's outcome.

    def measure(self, stage: str) -> _TextRequestTiming:
        return _TextRequestTiming(self, stage)

    def mark(self, stage: str, started: float) -> None:
        key = stage + "_ms"
        if key not in self.metrics:
            self.record(stage, **{key: self.elapsed_ms(started)})

    def response_metadata(self, value: Any) -> None:
        if not isinstance(value, Mapping):
            return
        model = value.get("model")
        if isinstance(model, str) and model.strip() and len(model) <= 200:
            model = model.strip()
            if model != self.metrics.get("reported_model"):
                self.record("provider_model", reported_model=model)
        usage = value.get("usage")
        if isinstance(usage, Mapping):
            counts = {}
            for key in ("prompt_tokens", "completion_tokens", "total_tokens"):
                count = usage.get(key)
                if type(count) is int and 0 <= count <= 1_000_000_000:
                    counts[key] = count
            details = usage.get("completion_tokens_details")
            if isinstance(details, Mapping):
                count = details.get("reasoning_tokens")
                if type(count) is int and 0 <= count <= 1_000_000_000:
                    counts["reasoning_tokens"] = count
            if counts:
                self.record("usage", **counts)


DEFAULT_BASE_URL = "https://all-scenario-device-mirror.test.honor.com"
DEFAULT_DEVICE_NAME = "honor-3dprinter"
DEFAULT_SERIAL_NUMBER = "SN-LBR-61029"
DEFAULT_USER_ID = "61029"
# Local defaults match the verified standalone script. Environment overrides
# are resolved per field before authentication, never after a server failure.
DEFAULT_ROOT_SECRET = 'change-client-root-secret'
DEFAULT_ACCESS_KEY = 'change-hmac-access-key'
DEFAULT_HMAC_SECRET = 'change-hmac-secret'
DEFAULT_CLIENT_ID = 'change-hmac-client-id'
SIGNED_HEADERS = "date;x-client-id;x-request-nonce;x-content-sha256"
HKDF_SALT = b"hclaw-gateway-client-security"
MAX_IMAGE_BYTES = 20 * 1024 * 1024
MAX_IMAGE_BASE64_BYTES = 6 * 1024 * 1024
MAX_MODEL_BYTES = 512 * 1024 * 1024
DEFAULT_POLL_INTERVAL_SECONDS = 10.0
DEFAULT_TASK_TIMEOUT_SECONDS = 1800.0


class YoYoClawError(RuntimeError):
    """A bounded provider error safe to expose in the sidecar status model."""

    def __init__(
        self,
        message: str,
        *,
        code: str = "provider_failed",
        category: str = "provider",
        retryable: bool = False,
        ambiguous: bool = False,
        status: int | None = None,
    ) -> None:
        super().__init__(message)
        self.code = code
        self.category = category
        self.retryable = retryable
        self.ambiguous = ambiguous
        self.status = status


def _require_requests() -> Any:
    if requests is None:
        raise YoYoClawError(
            "YOYOClaw support is unavailable because the bundled HTTP dependency is missing.",
            code="provider_dependencies_missing", category="configuration",
        )
    return requests


def _require_aesgcm() -> Any:
    if AESGCM is None:
        raise YoYoClawError(
            "YOYOClaw support is unavailable because the bundled encryption dependency is missing.",
            code="provider_dependencies_missing", category="configuration",
        )
    return AESGCM


def _is_requests_error(error: BaseException) -> bool:
    return requests is not None and isinstance(error, requests.RequestException)


def _is_requests_timeout(error: BaseException) -> bool:
    return requests is not None and isinstance(error, requests.Timeout)


@dataclass(frozen=True)
class YoYoClawConfig:
    base_url: str = DEFAULT_BASE_URL
    root_secret: str = DEFAULT_ROOT_SECRET
    access_key: str = DEFAULT_ACCESS_KEY
    hmac_secret: str = DEFAULT_HMAC_SECRET
    client_id: str = DEFAULT_CLIENT_ID
    device_name: str = DEFAULT_DEVICE_NAME
    serial_number: str = DEFAULT_SERIAL_NUMBER
    user_id: str = DEFAULT_USER_ID

    @classmethod
    def defaults(cls) -> "YoYoClawConfig":
        return cls(
            base_url=DEFAULT_BASE_URL,
            root_secret=DEFAULT_ROOT_SECRET,
            access_key=DEFAULT_ACCESS_KEY,
            hmac_secret=DEFAULT_HMAC_SECRET,
            client_id=DEFAULT_CLIENT_ID,
            device_name=DEFAULT_DEVICE_NAME,
            serial_number=DEFAULT_SERIAL_NUMBER,
            user_id=DEFAULT_USER_ID,
        )

    @classmethod
    def from_environment(cls) -> "YoYoClawConfig":
        """Use nonblank environment values, with independent code defaults."""
        def value(default: str, *names: str) -> str:
            for name in names:
                try:
                    configured = os.getenv(name, "").strip()
                except (OSError, ValueError, TypeError):
                    continue
                if configured:
                    return configured
            return default

        return cls(
            base_url=value(DEFAULT_BASE_URL, "YOYOCLAW_BASE_URL"),
            root_secret=value(DEFAULT_ROOT_SECRET, "YOYOCLAW_ROOT_SECRET"),
            # Prefer the standalone script's names; retain the shorter aliases.
            access_key=value(DEFAULT_ACCESS_KEY, "YOYOCLAW_HMAC_ACCESS_KEY", "YOYOCLAW_ACCESS_KEY"),
            hmac_secret=value(DEFAULT_HMAC_SECRET, "YOYOCLAW_HMAC_SECRET"),
            client_id=value(DEFAULT_CLIENT_ID, "YOYOCLAW_HMAC_CLIENT_ID", "YOYOCLAW_CLIENT_ID"),
            device_name=value(DEFAULT_DEVICE_NAME, "YOYOCLAW_DEVICE_NAME"),
            serial_number=value(DEFAULT_SERIAL_NUMBER, "YOYOCLAW_SERIAL_NUMBER"),
            user_id=value(DEFAULT_USER_ID, "YOYOCLAW_USER_ID"),
        )

    @property
    def configured(self) -> bool:
        return bool(
            self.base_url
            and self.root_secret
            and self.access_key
            and self.hmac_secret
            and self.client_id
            and self.device_name
            and self.serial_number
            and self.user_id
        )


def _require_http_url(value: str, *, https: bool = False) -> str:
    parsed = urlsplit(str(value).strip())
    if parsed.scheme not in {"http", "https"} or not parsed.hostname or parsed.username or parsed.password:
        raise ValueError("YOYOClaw endpoint must be an HTTP/HTTPS URL without credentials.")
    if https and parsed.scheme != "https":
        raise ValueError("YOYOClaw authentication endpoint must use HTTPS.")
    return str(value).strip().rstrip("/")


def _json_bytes(value: Any) -> bytes:
    import json

    return json.dumps(value, ensure_ascii=False, separators=(",", ":")).encode("utf-8")


def _b64url(value: bytes) -> str:
    return base64.urlsafe_b64encode(value).rstrip(b"=").decode("ascii")


def _derive_key(root_secret: str, purpose: str) -> bytes:
    prk = hmac.new(HKDF_SALT, root_secret.encode("utf-8"), hashlib.sha256).digest()
    return hmac.new(prk, purpose.encode("utf-8") + b"\x01", hashlib.sha256).digest()


def _encrypt_field(root_secret: str, plaintext: str, purpose: str, aad: str) -> str:
    nonce = secrets.token_bytes(12)
    ciphertext = _require_aesgcm()(_derive_key(root_secret, purpose)).encrypt(
        nonce, plaintext.encode("utf-8"), aad.encode("utf-8")
    )
    return f"{_b64url(nonce)}.{_b64url(ciphertext)}"


def _image_data_url(path: str | os.PathLike[str]) -> str:
    image_path = Path(path)
    try:
        data = image_path.read_bytes()
    except OSError:
        raise YoYoClawError("The YOYOClaw image input could not be read.", code="invalid_model_request", category="validation") from None
    if not data or len(data) > MAX_IMAGE_BYTES:
        raise YoYoClawError("The YOYOClaw image input is empty or exceeds 20 MB.", code="invalid_model_request", category="validation")
    if data.startswith(b"\x89PNG\r\n\x1a\n"):
        mime = "image/png"
    elif data.startswith(b"\xff\xd8\xff"):
        mime = "image/jpeg"
    elif data.startswith(b"RIFF") and data[8:12] == b"WEBP":
        mime = "image/webp"
    else:
        raise YoYoClawError("The YOYOClaw image input must be PNG, JPEG, or WebP.", code="invalid_model_request", category="validation")
    return f"data:{mime};base64,{base64.b64encode(data).decode('ascii')}"


def _extract_image_url(result: Mapping[str, Any]) -> str | None:
    images = result.get("data")
    if isinstance(images, list):
        for item in images:
            if isinstance(item, Mapping) and isinstance(item.get("url"), str) and item["url"].strip():
                return item["url"].strip()
    output = result.get("output")
    choices = output.get("choices") if isinstance(output, Mapping) else None
    if isinstance(choices, list):
        for choice in choices:
            message = choice.get("message") if isinstance(choice, Mapping) else None
            content = message.get("content") if isinstance(message, Mapping) else None
            if isinstance(content, list):
                for item in content:
                    if isinstance(item, Mapping) and isinstance(item.get("image"), str) and item["image"].strip():
                        return item["image"].strip()
    return None


class YoYoClawClient:
    def __init__(
        self,
        config: YoYoClawConfig | None = None,
        *,
        session: Any | None = None,
        monotonic: Callable[[], float] = time.monotonic,
        sleep: Callable[[float], None] = time.sleep,
    ) -> None:
        self.config = config or YoYoClawConfig.from_environment()
        self.base_url = _require_http_url(self.config.base_url)
        self._session = session
        if self._session is None and requests is not None:
            self._session = requests.Session()
        self._monotonic = monotonic
        self._sleep = sleep
        self._access_token = ""
        self._refresh_token = ""
        self._expires_at = 0.0
        self._fingerprint = ""

    def close(self) -> None:
        close = getattr(self._session, "close", None)
        if callable(close):
            close()

    @property
    def authenticated(self) -> bool:
        return bool(self._access_token)

    def _login_payload(self) -> dict[str, str]:
        cfg = self.config
        return {
            "device_name_ciphertext": _encrypt_field(cfg.root_secret, cfg.device_name, "device-name-field", "device-name-field"),
            "sn_ciphertext": _encrypt_field(cfg.root_secret, cfg.serial_number, "sn-field", f"sn-field|{cfg.device_name}"),
            "user_id_ciphertext": _encrypt_field(cfg.root_secret, cfg.user_id, "user-id-field", f"user-id-field|{cfg.device_name}"),
        }

    def _login_headers(self, body: bytes) -> dict[str, str]:
        path = "/yoyoclaw/auth/login"
        date = format_datetime(datetime.now(timezone.utc), usegmt=True)
        nonce = secrets.token_hex(16)
        digest = hashlib.sha256(body).hexdigest()
        signing_string = (
            f"POST\n{path}\n\n{self.config.access_key}\n{date}\n"
            f"date:{date}\n"
            f"x-client-id:{self.config.client_id}\n"
            f"x-request-nonce:{nonce}\n"
            f"x-content-sha256:{digest}\n"
        )
        signature = base64.b64encode(
            hmac.new(self.config.hmac_secret.encode("utf-8"), signing_string.encode("utf-8"), hashlib.sha256).digest()
        ).decode("ascii")
        return {
            "Date": date,
            "X-HMAC-ALGORITHM": "hmac-sha256",
            "X-HMAC-ACCESS-KEY": self.config.access_key,
            "X-HMAC-SIGNED-HEADERS": SIGNED_HEADERS,
            "X-HMAC-SIGNATURE": signature,
            "X-Client-ID": self.config.client_id,
            "X-Request-Nonce": nonce,
            "X-Content-SHA256": digest,
        }

    @staticmethod
    def _response_data(response: Any) -> tuple[int, Any]:
        status = int(getattr(response, "status_code", 0) or 0)
        try:
            result = response.json()
        except (ValueError, TypeError):
            raise YoYoClawError("YOYOClaw returned a non-JSON response.", code="invalid_provider_response", category="provider", status=status) from None
        if not isinstance(result, Mapping):
            raise YoYoClawError("YOYOClaw returned an invalid JSON object.", code="invalid_provider_response", category="provider", status=status)
        return status, result

    def _request(
        self,
        method: str,
        path: str,
        *,
        body: bytes | None = None,
        headers: Mapping[str, str] | None = None,
        timeout: float = 180.0,
        creation: bool = False,
        stream: bool = False,
    ) -> Any:
        request_headers = {"Accept": "application/json", "Content-Type": "application/json"}
        if self._fingerprint:
            request_headers["X-HCLAW-Device-Fingerprint"] = self._fingerprint
        if headers:
            request_headers.update(headers)
        session = self._session
        if session is None:
            _require_requests()
            raise AssertionError("requests session initialization failed")
        try:
            response = session.request(
                method, self.base_url + path, data=body, headers=request_headers,
                timeout=(15, timeout), allow_redirects=False, stream=stream,
            )
        except Exception as exc:
            if _is_requests_error(exc):
                raise YoYoClawError(
                    "YOYOClaw network request failed; the operation may have been accepted remotely.",
                    code="provider_connection_failed", category="network", retryable=not creation,
                    ambiguous=creation,
                ) from None
            raise
        status, result = self._response_data(response)
        if not 200 <= status < 300 or (result.get("code") not in (None, "OK")):
            error = result.get("error") if isinstance(result.get("error"), Mapping) else {}
            provider_code = error.get("code") or result.get("code") or "provider_http_error"
            if not isinstance(provider_code, str) or len(provider_code) > 100:
                provider_code = "provider_http_error"
            category = "authentication" if status in {401, 403} else "rate_limit" if status == 429 else "network" if status >= 500 else "provider"
            retryable = status == 429 or status >= 500
            raise YoYoClawError(
                f"YOYOClaw request failed (HTTP {status}, code={provider_code}).",
                code=provider_code, category=category, retryable=(retryable and not creation),
                ambiguous=creation and (status >= 500 or status in {408, 409}), status=status,
            )
        if stream:
            return response
        return result.get("data", result)

    def _set_tokens(self, data: Any) -> None:
        if not isinstance(data, Mapping) or not isinstance(data.get("access_token"), str) or not data["access_token"]:
            raise YoYoClawError("YOYOClaw authentication response did not contain an access token.", code="authentication_failed", category="authentication")
        self._access_token = data["access_token"]
        refresh = data.get("refresh_token")
        if isinstance(refresh, str) and refresh:
            self._refresh_token = refresh
        try:
            expires = float(data.get("expires_in", 3600))
        except (TypeError, ValueError):
            expires = 3600.0
        self._expires_at = self._monotonic() + max(60.0, expires)

    def login(self) -> None:
        if not self.config.configured:
            raise YoYoClawError("YOYOClaw credentials are not configured.", code="provider_not_configured", category="configuration")
        _require_http_url(self.base_url, https=True)
        self._fingerprint = hashlib.sha256(self.config.user_id.encode("utf-8")).hexdigest()
        body = _json_bytes(self._login_payload())
        data = self._request("POST", "/yoyoclaw/auth/login", body=body, headers=self._login_headers(body), timeout=60, creation=False)
        self._set_tokens(data)

    def _refresh(self) -> None:
        if not self._refresh_token:
            self.login()
            return
        try:
            data = self._request("POST", "/yoyoclaw/auth/refresh", body=_json_bytes({"refresh_token": self._refresh_token}), timeout=60)
            self._set_tokens(data)
        except YoYoClawError:
            self._access_token = ""
            self._refresh_token = ""
            raise

    def _ensure_token(self) -> None:
        if not self._access_token:
            self.login()
        elif self._monotonic() >= self._expires_at - 60:
            self._refresh()

    def api(self, method: str, path: str, payload: Mapping[str, Any] | None = None, *, timeout: float = 180, creation: bool = False) -> Any:
        self._ensure_token()
        return self._request(
            method, path,
            body=None if payload is None else _json_bytes(payload),
            headers={"Authorization": f"Bearer {self._access_token}"},
            timeout=timeout,
            creation=creation,
        )

    def whitelist_catalog(self, library: str) -> dict[str, Any]:
        if library not in {"model-library", "image-model-library", "3d-model-library"}:
            raise YoYoClawError("Unsupported YOYOClaw model library.", code="invalid_model_request", category="validation")
        data = self.api("GET", f"/yoyoclaw/model/{library}/whitelist", timeout=60)
        if isinstance(data, list):
            models = data
            default_model = ""
        elif isinstance(data, Mapping):
            models = data.get("models")
            if not isinstance(models, list):
                models = data.get("items")
            if not isinstance(models, list):
                models = data.get("list")
            default_model = data.get("default_model")
            if not isinstance(default_model, str) or not default_model.strip():
                default_model = data.get("defaultModel")
            if not isinstance(default_model, str):
                default_model = ""
        else:
            models = None
            default_model = ""
        if not isinstance(models, list):
            raise YoYoClawError("YOYOClaw whitelist response is invalid.", code="invalid_provider_response", category="provider")
        return {
            "models": [dict(item) for item in models if isinstance(item, Mapping)],
            "default_model": default_model.strip(),
        }

    def whitelist(self, library: str) -> list[dict[str, Any]]:
        """Return the model rows while keeping the legacy client contract."""
        return self.whitelist_catalog(library)["models"]

    @staticmethod
    def _text_from_non_stream_response(result: Any, diagnostics: TextRequestDiagnostics | None = None) -> str:
        if not isinstance(result, Mapping):
            raise YoYoClawError(
                "YOYOClaw returned an invalid text response.",
                code="invalid_provider_response", category="provider",
            )
        if result.get("code") not in (None, "OK"):
            raise YoYoClawError(
                "YOYOClaw text request failed.",
                code=str(result.get("code") or "provider_http_error")[:100],
                category="provider",
            )
        value = result.get("data", result)
        if diagnostics is not None:
            diagnostics.response_metadata(result)
            if value is not result:
                diagnostics.response_metadata(value)
        choices = value.get("choices", []) if isinstance(value, Mapping) else []
        chunks: list[str] = []
        if isinstance(choices, list):
            for choice in choices:
                if not isinstance(choice, Mapping):
                    continue
                message = choice.get("message", choice.get("delta", {}))
                content = message.get("content") if isinstance(message, Mapping) else None
                if isinstance(content, str):
                    chunks.append(content)
        if not chunks:
            raise YoYoClawError(
                "YOYOClaw text response did not contain message content.",
                code="invalid_provider_response", category="provider",
            )
        return "".join(chunks)

    @staticmethod
    def _text_from_sse_response(
        response: Any, diagnostics: TextRequestDiagnostics | None = None, started: float = 0.0,
    ) -> str:
        answer: list[str] = []
        event_data: list[str] = []
        saw_done = False

        def consume(data: str) -> None:
            nonlocal saw_done
            if not data:
                return
            if diagnostics is not None:
                diagnostics.mark("first_event", started)
            if data.strip() == "[DONE]":
                saw_done = True
                if diagnostics is not None:
                    diagnostics.mark("done", started)
                return
            try:
                event = json.loads(data)
            except (TypeError, json.JSONDecodeError):
                raise YoYoClawError(
                    "YOYOClaw returned malformed SSE data.",
                    code="invalid_provider_response", category="provider",
                ) from None
            if not isinstance(event, Mapping):
                raise YoYoClawError(
                    "YOYOClaw returned an invalid SSE event.",
                    code="invalid_provider_response", category="provider",
                )
            if event.get("code") not in (None, "OK"):
                raise YoYoClawError(
                    "YOYOClaw text stream reported an error.",
                    code=str(event.get("code") or "provider_http_error")[:100],
                    category="provider",
                )
            value = event.get("data", event)
            if diagnostics is not None:
                diagnostics.response_metadata(event)
                if value is not event:
                    diagnostics.response_metadata(value)
            choices = value.get("choices", []) if isinstance(value, Mapping) else []
            if not isinstance(choices, list):
                return
            for choice in choices:
                if not isinstance(choice, Mapping):
                    continue
                delta = choice.get("delta", {})
                content = delta.get("content") if isinstance(delta, Mapping) else None
                if diagnostics is not None and isinstance(delta, Mapping) and delta.get("reasoning_content"):
                    diagnostics.mark("first_reasoning", started)
                if isinstance(content, str) and content:
                    if diagnostics is not None:
                        diagnostics.mark("first_content", started)
                        diagnostics.metrics["last_content_ms"] = diagnostics.elapsed_ms(started)
                    answer.append(content)

        try:
            lines = response.iter_lines(decode_unicode=True)
            for raw_line in lines:
                if raw_line is None:
                    continue
                line = raw_line.decode("utf-8") if isinstance(raw_line, bytes) else str(raw_line)
                if line == "":
                    consume("\n".join(event_data))
                    event_data.clear()
                    continue
                if line.startswith(":"):
                    continue
                if line.startswith("data:"):
                    event_data.append(line[5:].lstrip())
            if event_data:
                consume("\n".join(event_data))
            if diagnostics is not None:
                eof_ms = diagnostics.elapsed_ms(started)
                fields = {"stream_eof_ms": eof_ms}
                if "first_content_ms" in diagnostics.metrics:
                    fields["content_receive_ms"] = diagnostics.metrics["last_content_ms"] - diagnostics.metrics["first_content_ms"]
                if "done_ms" in diagnostics.metrics:
                    fields["done_to_eof_ms"] = max(0, eof_ms - diagnostics.metrics["done_ms"])
                diagnostics.record("stream_finished", **fields)
        except YoYoClawError:
            raise
        except Exception as exc:
            if _is_requests_error(exc):
                raise YoYoClawError(
                    "YOYOClaw text stream timed out." if _is_requests_timeout(exc)
                    else "YOYOClaw text stream was interrupted.",
                    code="provider_timeout" if _is_requests_timeout(exc) else "provider_connection_failed",
                    category="availability" if _is_requests_timeout(exc) else "network",
                ) from None
            raise
        if not saw_done:
            raise YoYoClawError(
                "YOYOClaw text stream ended before its completion marker.",
                code="incomplete_provider_response", category="network",
            )
        if not answer:
            raise YoYoClawError(
                "YOYOClaw text stream did not contain answer content.",
                code="invalid_provider_response", category="provider",
            )
        return "".join(answer)

    def text_chat(
        self, model: str, messages: list[dict[str, Any]], *, timeout: float = 180.0,
        diagnostics: TextRequestDiagnostics | None = None,
    ) -> str:
        self._ensure_token()
        payload = {
            "provider": "model-library",
            "model": model,
            "messages": messages,
            "stream": True,
        }
        session = self._session
        if session is None:
            _require_requests()
            raise AssertionError("requests session initialization failed")
        headers = {
            "Accept": "text/event-stream, application/json",
            "Content-Type": "application/json",
            "Authorization": f"Bearer {self._access_token}",
        }
        if self._fingerprint:
            headers["X-HCLAW-Device-Fingerprint"] = self._fingerprint
        started = diagnostics.clock() if diagnostics is not None else 0.0
        if diagnostics is not None:
            diagnostics.record("chat_started", requested_model=model, reported_model=None)
        try:
            with diagnostics.measure("headers_wait") if diagnostics is not None else nullcontext():
                response = session.request(
                    "POST", self.base_url + "/yoyoclaw/model/chat/completions",
                    data=_json_bytes(payload), headers=headers, timeout=(15, timeout),
                    allow_redirects=False, stream=True,
                )
        except Exception as exc:
            if _is_requests_error(exc):
                raise YoYoClawError(
                    "YOYOClaw text request timed out." if _is_requests_timeout(exc)
                    else "YOYOClaw text request failed.",
                    code="provider_timeout" if _is_requests_timeout(exc) else "provider_connection_failed",
                    category="availability" if _is_requests_timeout(exc) else "network",
                ) from None
            raise

        try:
            status = int(getattr(response, "status_code", 0) or 0)
            if diagnostics is not None:
                diagnostics.record("http_response", http_status=status)
            if not 200 <= status < 300:
                try:
                    result = response.json()
                except (ValueError, TypeError):
                    result = {}
                error = result.get("error") if isinstance(result, Mapping) and isinstance(result.get("error"), Mapping) else {}
                provider_code = error.get("code") or (result.get("code") if isinstance(result, Mapping) else None)
                provider_code = provider_code if isinstance(provider_code, str) and len(provider_code) <= 100 else "provider_http_error"
                category = "authentication" if status in {401, 403} else "rate_limit" if status == 429 else "network" if status >= 500 else "provider"
                raise YoYoClawError(
                    f"YOYOClaw text request failed (HTTP {status}, code={provider_code}).",
                    code=provider_code, category=category, retryable=False, status=status,
                )
            content_type = str(getattr(response, "headers", {}).get("Content-Type", "")).lower()
            if "json" in content_type:
                try:
                    result = self._text_from_non_stream_response(response.json(), diagnostics)
                    if diagnostics is not None:
                        diagnostics.mark("json_body_complete", started)
                    return result
                except (ValueError, TypeError):
                    raise YoYoClawError(
                        "YOYOClaw returned a non-JSON text response.",
                        code="invalid_provider_response", category="provider",
                    ) from None
            return self._text_from_sse_response(response, diagnostics, started)
        finally:
            close = getattr(response, "close", None)
            if callable(close):
                close()

    def generate_image(self, model: str, prompt: str, image_path: Path | None = None) -> str:
        content: list[dict[str, str]] = []
        if image_path is not None:
            content.append({"image": _image_data_url(image_path)})
        if isinstance(prompt, str) and prompt.strip():
            content.append({"text": prompt.strip()})
        if not content:
            raise YoYoClawError("YOYOClaw image generation requires text or an image.", code="invalid_model_request", category="validation")
        result = self.api(
            "POST", "/yoyoclaw/model/chat/completions",
            {"model": model, "input": {"messages": [{"role": "user", "content": content}]}, "parameters": {"prompt_extend": True}},
            timeout=600, creation=True,
        )
        if not isinstance(result, Mapping):
            raise YoYoClawError("YOYOClaw image response is invalid.", code="invalid_provider_result", category="validation", ambiguous=True)
        image_url = _extract_image_url(result)
        if not image_url:
            raise YoYoClawError("YOYOClaw image response did not contain an image URL.", code="invalid_provider_result", category="validation", ambiguous=True)
        try:
            return _require_http_url(image_url)
        except ValueError:
            raise YoYoClawError("YOYOClaw returned an invalid image URL.", code="invalid_provider_result", category="validation", ambiguous=True) from None

    def submit_3d(self, model: str, image_path: Path) -> str:
        try:
            raw = image_path.read_bytes()
        except OSError:
            raise YoYoClawError("The YOYOClaw 3D reference image could not be read.", code="invalid_model_request", category="validation") from None
        encoded = base64.b64encode(raw).decode("ascii")
        if not encoded or len(encoded) > MAX_IMAGE_BASE64_BYTES:
            raise YoYoClawError("The YOYOClaw 3D reference image exceeds the 6 MB encoded limit.", code="invalid_model_request", category="validation")
        result = self.api("POST", "/yoyoclaw/model/chat/completions", {"model": model, "image_base64": encoded}, timeout=180, creation=True)
        task_id = result.get("id") if isinstance(result, Mapping) else None
        if not isinstance(task_id, str) or not task_id.strip():
            raise YoYoClawError("YOYOClaw did not return a 3D task reference.", code="missing_task_reference", category="validation", ambiguous=True)
        return task_id.strip()

    def wait_3d(
        self,
        model: str,
        task_id: str,
        *,
        stop_event: Any = None,
        progress: Callable[[int | float | None], None] | None = None,
        timeout: float = DEFAULT_TASK_TIMEOUT_SECONDS,
        interval: float = DEFAULT_POLL_INTERVAL_SECONDS,
    ) -> dict[str, Any]:
        task_id = task_id.strip() if isinstance(task_id, str) else ""
        if not task_id:
            raise YoYoClawError("A YOYOClaw task reference is required.", code="invalid_task_reference", category="validation")
        path = "/yoyoclaw/model/3d-model-library/tasks/" + quote(task_id, safe="") + "?" + urlencode({"model": model})
        deadline = self._monotonic() + timeout
        while self._monotonic() < deadline:
            if stop_event is not None and stop_event.is_set():
                raise YoYoClawError("YOYOClaw task polling was cancelled locally.", code="provider_cancelled", category="cancellation")
            result = self.api("GET", path, timeout=60)
            if not isinstance(result, Mapping):
                raise YoYoClawError("YOYOClaw 3D task response is invalid.", code="invalid_provider_response", category="provider")
            status = str(result.get("status", "")).strip().lower()
            if progress is not None:
                value = result.get("progress")
                if isinstance(value, (int, float)):
                    progress(value)
            if status in {"completed", "succeeded", "success"}:
                return dict(result)
            if status in {"failed", "error", "cancelled", "canceled", "expired"}:
                raise YoYoClawError(f"YOYOClaw 3D task failed ({status}).", code="provider_task_failed", category="provider")
            if not status:
                raise YoYoClawError("YOYOClaw 3D task response omitted its status.", code="invalid_provider_response", category="provider")
            if stop_event is not None and stop_event.wait(interval):
                raise YoYoClawError("YOYOClaw task polling was cancelled locally.", code="provider_cancelled", category="cancellation")
            self._sleep(0 if stop_event is not None else interval)
        raise YoYoClawError("YOYOClaw 3D task polling timed out; resume the existing task before retrying.", code="provider_timeout", category="availability", retryable=True)

    def download(self, url: str, destination: str | os.PathLike[str], max_bytes: int) -> Path:
        try:
            target_url = _require_http_url(url)
        except ValueError:
            raise YoYoClawError("YOYOClaw returned an invalid download URL.", code="unsafe_artifact", category="security") from None
        path = Path(destination)
        temporary = path.with_name(path.name + ".part")
        total = 0
        try:
            session = self._session
            if session is None:
                _require_requests()
                raise AssertionError("requests session initialization failed")
            response = session.get(target_url, stream=True, timeout=(15, 180), allow_redirects=False)
            status = int(getattr(response, "status_code", 0) or 0)
            if status != 200:
                raise YoYoClawError(f"YOYOClaw artifact download failed (HTTP {status}).", code="artifact_download_failed", category="network", retryable=status >= 500)
            path.parent.mkdir(parents=True, exist_ok=True)
            temporary.unlink(missing_ok=True)
            with temporary.open("wb") as stream:
                for chunk in response.iter_content(1024 * 1024):
                    if not chunk:
                        continue
                    total += len(chunk)
                    if total > max_bytes:
                        raise YoYoClawError("YOYOClaw artifact exceeds the local size limit.", code="artifact_too_large", category="validation")
                    stream.write(chunk)
            if total == 0:
                raise YoYoClawError("YOYOClaw artifact is empty.", code="invalid_provider_result", category="validation")
            temporary.replace(path)
            return path
        except YoYoClawError:
            temporary.unlink(missing_ok=True)
            raise
        except Exception as exc:
            temporary.unlink(missing_ok=True)
            if _is_requests_error(exc):
                raise YoYoClawError("YOYOClaw artifact download failed; do not resubmit the task.", code="artifact_download_failed", category="network", retryable=True) from None
            raise

    def download_result_artifact(
        self,
        task_result: Mapping[str, Any],
        output_path: str | os.PathLike[str],
        max_bytes: int = MAX_MODEL_BYTES,
        *,
        output_format: str = "glb",
    ) -> Path:
        assets = task_result.get("data") if isinstance(task_result, Mapping) else None
        if not isinstance(assets, list):
            raise YoYoClawError("YOYOClaw 3D result did not contain an asset list.", code="invalid_provider_result", category="validation")
        wanted = output_format.lower().strip()
        candidates: list[str] = []
        for item in assets:
            if not isinstance(item, Mapping) or not isinstance(item.get("url"), str):
                continue
            url = item["url"].strip()
            suffix = Path(urlsplit(url).path).suffix.lower()
            if wanted == "glb" and suffix == ".glb":
                candidates.insert(0, url)
            elif wanted == "obj" and suffix in {".zip", ".obj"}:
                candidates.append(url)
        if not candidates:
            raise YoYoClawError("YOYOClaw did not return the selected model format.", code="invalid_provider_result", category="validation")
        return self.download(candidates[0], output_path, max_bytes)
