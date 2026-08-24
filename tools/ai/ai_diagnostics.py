from __future__ import annotations

from contextlib import contextmanager
from contextvars import ContextVar
from datetime import datetime, timezone
import json
import os
import re
import socket
import ssl
import sys
import threading
from typing import Any, Iterator
import urllib.parse


_JOB_ID: ContextVar[str] = ContextVar("orcaslicer_ai_job_id", default="")
_WRITE_LOCK = threading.Lock()
_MAX_TEXT = 500
_SECRET_PATTERNS = (
    (re.compile(r"(?i)(authorization\s*[:=]\s*bearer\s+)[^\s,;]+"), r"\1<redacted>"),
    (re.compile(r"(?i)(bearer\s+)[^\s,;]+"), r"\1<redacted>"),
    (re.compile(r"(?i)((?:api[_-]?key|token|secret)\s*[:=]\s*)[^\s,;]+"), r"\1<redacted>"),
    (re.compile(r"\bsk-[A-Za-z0-9_-]{8,}\b"), "<redacted>"),
)
_URL_PATTERN = re.compile(r"https?://[^\s\"'<>]+", re.IGNORECASE)


def safe_endpoint(value: str) -> str:
    """Return a credential-free URL without query or fragment data."""
    try:
        parsed = urllib.parse.urlsplit(str(value))
        if parsed.scheme.lower() not in {"http", "https"} or not parsed.hostname:
            return "<invalid-endpoint>"
        host = parsed.hostname
        if ":" in host and not host.startswith("["):
            host = f"[{host}]"
        try:
            port = f":{parsed.port}" if parsed.port is not None else ""
        except ValueError:
            return "<invalid-endpoint>"
        path = parsed.path or "/"
        return urllib.parse.urlunsplit((parsed.scheme.lower(), host + port, path, "", ""))
    except Exception:
        return "<invalid-endpoint>"


def redact_text(value: Any) -> str:
    text = str(value)
    for pattern, replacement in _SECRET_PATTERNS:
        text = pattern.sub(replacement, text)
    text = _URL_PATTERN.sub(lambda match: safe_endpoint(match.group(0).rstrip(".,);]")), text)
    if len(text) > _MAX_TEXT:
        text = text[:_MAX_TEXT] + "..."
    return text


def _safe_value(value: Any) -> Any:
    if value is None or isinstance(value, (bool, int, float)):
        return value
    if isinstance(value, dict):
        return {redact_text(key): _safe_value(item) for key, item in value.items()}
    if isinstance(value, (list, tuple, set)):
        return [_safe_value(item) for item in value]
    return redact_text(value)


def exception_details(error: BaseException) -> list[dict[str, Any]]:
    details: list[dict[str, Any]] = []
    seen: set[int] = set()
    current: BaseException | None = error
    while current is not None and len(details) < 5 and id(current) not in seen:
        seen.add(id(current))
        item: dict[str, Any] = {
            "type": type(current).__name__,
            "message": redact_text(current),
        }
        error_number = getattr(current, "errno", None)
        if isinstance(error_number, int):
            item["errno"] = error_number
        details.append(item)
        reason = getattr(current, "reason", None)
        if isinstance(reason, BaseException) and id(reason) not in seen:
            current = reason
        else:
            current = current.__cause__ or current.__context__
    return details


def classify_connection_error(error: BaseException) -> str:
    seen: set[int] = set()
    current: BaseException | None = error
    while current is not None and id(current) not in seen:
        seen.add(id(current))
        message = str(current).lower()
        if isinstance(current, ssl.SSLCertVerificationError):
            return "tls_certificate"
        if isinstance(current, ssl.SSLError):
            return "tls"
        if isinstance(current, socket.gaierror):
            return "dns"
        if isinstance(current, (TimeoutError, socket.timeout)):
            return "timeout"
        if isinstance(current, ConnectionRefusedError):
            return "connection_refused"
        if isinstance(current, ConnectionResetError):
            return "connection_reset"
        if "proxy" in message or "tunnel connection failed" in message:
            return "proxy"
        reason = getattr(current, "reason", None)
        current = reason if isinstance(reason, BaseException) else current.__cause__ or current.__context__
    return "network"


@contextmanager
def diagnostic_context(job_id: str) -> Iterator[None]:
    token = _JOB_ID.set(redact_text(job_id))
    try:
        yield
    finally:
        _JOB_ID.reset(token)


def event(name: str, *, level: str = "INFO", **fields: Any) -> None:
    """Write one best-effort JSON event; diagnostics must never break production work."""
    try:
        payload: dict[str, Any] = {
            "timestamp": datetime.now(timezone.utc).isoformat(timespec="milliseconds").replace("+00:00", "Z"),
            "level": redact_text(level).upper(),
            "event": redact_text(name),
            "pid": os.getpid(),
            "thread": threading.current_thread().name,
        }
        job_id = _JOB_ID.get()
        if job_id:
            payload["job_id"] = job_id
        for key, value in fields.items():
            safe_key = re.sub(r"[^a-zA-Z0-9_.-]", "_", str(key))[:80]
            payload[safe_key] = _safe_value(value)
        encoded = json.dumps(payload, ensure_ascii=False, separators=(",", ":"))
        with _WRITE_LOCK:
            sys.stderr.write(encoded + "\n")
            sys.stderr.flush()
    except Exception:
        pass
