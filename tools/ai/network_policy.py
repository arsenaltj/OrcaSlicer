from __future__ import annotations

from dataclasses import dataclass
import os
from typing import Any, Mapping
import urllib.parse
import urllib.request


_HTTP_SCHEMES = ("http", "https")


@dataclass(frozen=True)
class _ProxyPolicy:
    proxies: dict[str, str]
    source: str


def _is_windows() -> bool:
    return os.name == "nt"


def _windows_registry_proxies() -> dict[str, str]:
    """Read WinINET proxy settings without exposing them outside this module."""
    getter = getattr(urllib.request, "getproxies_registry", None)
    if not callable(getter):
        return {}
    try:
        values = getter()
    except (OSError, ValueError):
        return {}
    return dict(values) if isinstance(values, Mapping) else {}


def _proxy_value(values: Mapping[str, Any], scheme: str) -> str:
    value = values.get(scheme, "")
    return value.strip() if isinstance(value, str) else ""


def _resolve_proxy_policy() -> _ProxyPolicy:
    if not _is_windows():
        values = urllib.request.getproxies()
        proxies = dict(values)
        has_proxy = any(
            scheme != "no" and isinstance(value, str) and bool(value.strip())
            for scheme, value in proxies.items()
        )
        return _ProxyPolicy(proxies=proxies, source="urllib" if has_proxy else "none")

    environment = urllib.request.getproxies_environment()
    registry = _windows_registry_proxies()
    proxies: dict[str, str] = {}
    used_environment = False
    used_registry = False
    for scheme in _HTTP_SCHEMES:
        value = _proxy_value(environment, scheme)
        if value:
            proxies[scheme] = value
            used_environment = True
            continue
        value = _proxy_value(registry, scheme)
        if value:
            proxies[scheme] = value
            used_registry = True

    if used_environment and used_registry:
        source = "environment+windows_registry"
    elif used_environment:
        source = "environment"
    elif used_registry:
        source = "windows_registry"
    else:
        source = "none"
    return _ProxyPolicy(proxies=proxies, source=source)


def _target_host(url: str) -> tuple[str, str]:
    try:
        parsed = urllib.parse.urlsplit(str(url))
        scheme = parsed.scheme.lower()
        hostname = parsed.hostname or ""
        if not hostname:
            return scheme, ""
        try:
            port = parsed.port
        except ValueError:
            return scheme, ""
        if port is None:
            return scheme, hostname
        if ":" in hostname and not hostname.startswith("["):
            hostname = f"[{hostname}]"
        return scheme, f"{hostname}:{port}"
    except Exception:
        return "", ""


def network_diagnostics(url: str) -> dict[str, object]:
    """Return proxy routing metadata that never contains an address or credential."""
    policy = _resolve_proxy_policy()
    scheme, host = _target_host(url)
    proxy_configured = scheme in policy.proxies or "all" in policy.proxies
    bypass = not proxy_configured
    if proxy_configured and host:
        try:
            bypass = bool(urllib.request.proxy_bypass(host))
        except (OSError, ValueError):
            bypass = False
    schemes = [scheme_name for scheme_name in _HTTP_SCHEMES if scheme_name in policy.proxies]
    return {"source": policy.source, "schemes": schemes, "bypass": bool(bypass)}


def build_opener(*handlers: Any) -> urllib.request.OpenerDirector:
    """Build a urllib opener with consistent, Windows-safe proxy selection."""
    policy = _resolve_proxy_policy()
    return urllib.request.build_opener(urllib.request.ProxyHandler(policy.proxies), *handlers)
