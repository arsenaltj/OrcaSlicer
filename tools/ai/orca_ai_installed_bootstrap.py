from __future__ import annotations

import json
import os
from pathlib import Path
import re
import runpy
import sys
from typing import TextIO
from urllib.parse import urlsplit


MAX_LOG_BYTES = 5 * 1024 * 1024
LOG_BACKUPS = 3
INTERNAL_DEFAULTS_FILENAME = "orca_ai_internal_defaults.json"
BUILD_INFO_FILENAME = "orca_ai_build_info.json"
MAX_INTERNAL_DEFAULTS_BYTES = 32 * 1024
MAX_BUILD_INFO_BYTES = 16 * 1024
INTERNAL_CONFIG_MODE = "internal_locked"
INTERNAL_DEFAULT_NAMES = frozenset({
    "OPENAI_PRO_API",
    "OPENAI_PRO_URL",
    "OPENAI_API_KEY",
    "OPENAI_BASE_URL",
    "OPENAI_IMAGE_MODEL",
    "OPENAI_IMAGE_QUALITY",
    "OPENAI_TEXT_MODEL",
    "TRIPO_API_BASE",
    "TRIPO_API_KEY",
    "TRIPO_MODEL",
})
BUILD_INFO_ENVIRONMENT = {
    "application_version": "ORCASLICER_AI_APP_VERSION",
    "application_commit": "ORCASLICER_AI_APP_COMMIT",
    "package_revision": "ORCASLICER_AI_PACKAGE_REVISION",
    "distribution_channel": "ORCASLICER_AI_DISTRIBUTION_CHANNEL",
}


def load_build_info(build_info_path: Path | None = None) -> dict[str, str | int]:
    """Load non-secret package identity and expose it to the Sidecar process."""
    path = build_info_path or Path(__file__).with_name(BUILD_INFO_FILENAME)
    try:
        if not path.is_file() or path.stat().st_size > MAX_BUILD_INFO_BYTES:
            return {}
        payload = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError):
        return {}

    expected_keys = {
        "schema_version", "application_version", "application_commit", "package_revision",
        "distribution_channel", "sidecar_protocol_version", "sidecar_version",
    }
    if not isinstance(payload, dict) or set(payload) != expected_keys:
        return {}
    if type(payload.get("schema_version")) is not int or payload["schema_version"] != 1:
        return {}
    if type(payload.get("sidecar_protocol_version")) is not int or payload["sidecar_protocol_version"] != 2:
        return {}
    string_fields = expected_keys - {"schema_version", "sidecar_protocol_version"}
    if any(not isinstance(payload.get(name), str) or not payload[name] or len(payload[name]) > 128
           for name in string_fields):
        return {}
    if payload["application_commit"] != "unknown" and not re.fullmatch(r"[0-9A-Fa-f]{40}", payload["application_commit"]):
        return {}
    if not re.fullmatch(r"[0-9A-Za-z._+-]+", payload["application_version"]):
        return {}
    if not re.fullmatch(r"[0-9A-Za-z._-]+", payload["package_revision"]):
        return {}
    if payload["distribution_channel"] not in {"internal", "commercial"}:
        return {}
    if payload["sidecar_version"] != "orcaslicer-ai-sidecar-v9":
        return {}

    for field, environment_name in BUILD_INFO_ENVIRONMENT.items():
        os.environ[environment_name] = payload[field]
    return payload


def load_internal_defaults(defaults_path: Path | None = None) -> tuple[str, ...]:
    path = defaults_path or Path(__file__).with_name(INTERNAL_DEFAULTS_FILENAME)
    try:
        if not path.is_file() or path.stat().st_size > MAX_INTERNAL_DEFAULTS_BYTES:
            return ()
        payload = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError):
        return ()

    if (
        not isinstance(payload, dict)
        or type(payload.get("version")) is not int
        or payload["version"] != 1
        or payload.get("mode") != INTERNAL_CONFIG_MODE
    ):
        return ()
    if set(payload) - (INTERNAL_DEFAULT_NAMES | {"version", "mode"}):
        return ()

    defaults: dict[str, str] = {}
    for name in INTERNAL_DEFAULT_NAMES:
        value = payload.get(name)
        if value is None:
            continue
        if (
            not isinstance(value, str)
            or not value
            or len(value) > 8192
            or any(char in value for char in "\0\r\n")
        ):
            return ()
        defaults[name] = value

    required_names = {
        "OPENAI_PRO_API", "OPENAI_PRO_URL",
        "OPENAI_API_KEY", "OPENAI_BASE_URL", "TRIPO_API_KEY",
    }
    if not required_names.issubset(defaults):
        return ()
    for name in ("OPENAI_PRO_URL", "OPENAI_BASE_URL", "TRIPO_API_BASE"):
        value = defaults.get(name)
        if value is None:
            continue
        parsed = urlsplit(value)
        if (
            parsed.scheme != "https"
            or not parsed.hostname
            or parsed.username is not None
            or parsed.password is not None
            or parsed.query
            or parsed.fragment
        ):
            return ()

    for name, value in defaults.items():
        os.environ[name] = value
    os.environ["ORCASLICER_AI_CONFIG_MODE"] = INTERNAL_CONFIG_MODE
    return tuple(sorted(defaults))


def configure_runtime(data_directory: str) -> tuple[Path, Path]:
    data_dir = Path(data_directory).expanduser().resolve()
    output_dir = data_dir / "generated_models"
    log_dir = data_dir / "log"
    output_dir.mkdir(parents=True, exist_ok=True)
    log_dir.mkdir(parents=True, exist_ok=True)

    os.environ.setdefault("ORCASLICER_AI_OUTPUT_DIR", str(output_dir))
    os.environ.setdefault("PYTHONIOENCODING", "utf-8")
    os.environ.setdefault("PYTHONUTF8", "1")
    return output_dir, log_dir / "orca-ai-sidecar.log"


def redirect_output(log_path: Path) -> TextIO:
    rotate_log(log_path)
    stream = log_path.open("a", encoding="utf-8", buffering=1)
    sys.stdout = stream
    sys.stderr = stream
    return stream


def rotate_log(log_path: Path, max_bytes: int = MAX_LOG_BYTES, backups: int = LOG_BACKUPS) -> None:
    try:
        if backups <= 0 or not log_path.is_file() or log_path.stat().st_size < max_bytes:
            return
        oldest = log_path.with_name(f"{log_path.name}.{backups}")
        oldest.unlink(missing_ok=True)
        for index in range(backups - 1, 0, -1):
            source = log_path.with_name(f"{log_path.name}.{index}")
            if source.exists():
                source.replace(log_path.with_name(f"{log_path.name}.{index + 1}"))
        log_path.replace(log_path.with_name(f"{log_path.name}.1"))
    except OSError:
        # Logging is diagnostic-only; a locked/unwritable log must not stop Orca.
        return


def log_bootstrap_failure(log_path: Path, code: str) -> None:
    """Best-effort preflight logging before stdout/stderr can be redirected."""
    try:
        rotate_log(log_path)
        payload = {"level": "ERROR", "event": "bootstrap.failed", "code": code}
        with log_path.open("a", encoding="utf-8") as stream:
            stream.write(json.dumps(payload, separators=(",", ":")) + "\n")
    except OSError:
        return


def verify_installed_configuration(
    build_info_path: Path | None = None,
    defaults_path: Path | None = None,
) -> dict[str, str | int]:
    """Validate package identity and provider payload without starting a service."""
    build_info = load_build_info(build_info_path)
    if not build_info:
        raise RuntimeError("Installed AI Sidecar build identity is missing or invalid.")
    path = defaults_path or Path(__file__).with_name(INTERNAL_DEFAULTS_FILENAME)
    channel = str(build_info["distribution_channel"])
    if channel == "internal":
        loaded = load_internal_defaults(path)
        if not loaded:
            raise RuntimeError("Internal package is missing a complete locked provider configuration.")
        return {
            "distribution_channel": channel,
            "configuration_mode": INTERNAL_CONFIG_MODE,
            "configured_count": len(loaded),
        }
    if path.exists():
        raise RuntimeError("Commercial package contains a provider configuration payload.")
    return {
        "distribution_channel": channel,
        "configuration_mode": "commercial_gateway",
        "configured_count": 0,
    }


def run_installed_sidecar(data_directory: str) -> None:
    _, log_path = configure_runtime(data_directory)
    build_info = load_build_info()
    if not build_info:
        log_bootstrap_failure(log_path, "build_identity_invalid")
        raise RuntimeError("Installed AI Sidecar build identity is missing or invalid.")
    parent_pid = os.environ.get("ORCASLICER_AI_PARENT_PID", "").strip()
    if not parent_pid.isascii() or not parent_pid.isdecimal() or not (0 < int(parent_pid) <= 0xFFFFFFFF):
        log_bootstrap_failure(log_path, "parent_process_invalid")
        raise RuntimeError("Installed AI Sidecar parent process identity is missing or invalid.")
    defaults_path = Path(__file__).with_name(INTERNAL_DEFAULTS_FILENAME)
    if build_info["distribution_channel"] == "commercial":
        if defaults_path.exists():
            log_bootstrap_failure(log_path, "commercial_credentials_present")
            raise RuntimeError("Commercial AI Sidecar must not contain package-only provider credentials.")
        # A commercial candidate must never silently fall back to long-lived
        # provider credentials inherited from the desktop environment.
        os.environ.pop("OPENAI_API_KEY", None)
        os.environ.pop("OPENAI_PRO_API", None)
        os.environ.pop("OPENAI_PRO_URL", None)
        os.environ.pop("TRIPO_API_KEY", None)
    else:
        loaded_defaults = load_internal_defaults(defaults_path)
        if defaults_path.exists() and not loaded_defaults:
            log_bootstrap_failure(log_path, "internal_defaults_invalid")
            raise RuntimeError("Installed AI Sidecar internal defaults are invalid.")
    os.environ["ORCASLICER_AI_REQUIRE_SESSION"] = "1"
    os.environ["PYTHONNOUSERSITE"] = "1"
    stream = redirect_output(log_path)
    script_path = Path(__file__).with_name("orca_ai_sidecar.py").resolve()
    if not script_path.is_file():
        print(f"Installed AI Sidecar is missing: {script_path}", file=stream)
        raise FileNotFoundError(script_path)

    sys.path.insert(0, str(script_path.parent))
    sys.argv = [str(script_path)]
    print(f"Starting installed OrcaSlicer AI Sidecar from {script_path}")
    runpy.run_path(str(script_path), run_name="__main__")


def main(argv: list[str] | None = None) -> int:
    arguments = list(sys.argv[1:] if argv is None else argv)
    if arguments == ["--verify-install"]:
        try:
            report = verify_installed_configuration()
        except RuntimeError as exc:
            print(f"Installed AI Sidecar verification failed: {exc}", file=sys.stderr)
            return 1
        print(
            "Installed AI Sidecar configuration: PASS "
            f"(channel={report['distribution_channel']}, mode={report['configuration_mode']}, "
            f"configured={report['configured_count']})"
        )
        return 0
    if len(arguments) != 1 or not arguments[0].strip():
        print("Usage: orca_ai_installed_bootstrap.py <orcaslicer-data-directory>", file=sys.stderr)
        return 2
    run_installed_sidecar(arguments[0])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
