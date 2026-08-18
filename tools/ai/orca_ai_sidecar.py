#!/usr/bin/env python3
from __future__ import annotations

import atexit
from array import array
from collections import Counter, deque
import math
import json
import os
import re
import shutil
import socket
import stat
import sys
import threading
import time
import uuid
import zipfile
from concurrent.futures import Future, ThreadPoolExecutor
from dataclasses import asdict, dataclass, field
from email import policy
from email.parser import BytesParser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any, BinaryIO, Callable

from openai_preprocessor import OpenAIPreprocessorError, complete_text, generate_image, preprocess_image, preprocess_text
from printable_image_pipeline import PrintSettings, PrintableImageError, process_printable_image
from printable_model_quality import ModelQualityError, analyze_printable_obj, write_model_quality_report
from printable_visual_quality import REPORT_FILENAME as VISUAL_QUALITY_FILENAME, review_model_visual_quality
from printable_palette import MAX_PRINTABLE_COLORS, PrintablePaletteError, assign_palette_roles
from tripo_client import (
    TripoError,
    create_conversion,
    create_image_task,
    create_text_task,
    download_task_artifact,
    upload_image,
    wait_for_task,
)

HOST = os.environ.get("ORCASLICER_AI_SIDECAR_HOST", "127.0.0.1")
PORT = int(os.environ.get("ORCASLICER_AI_SIDECAR_PORT", "18764"))
SIDECAR_VERSION = "orcaslicer-ai-sidecar-v5"
MAX_REQUEST_BYTES = 256 * 1024
MAX_CHANGES = 8
MAX_PROMPT_BYTES = 2000
MAX_CUSTOM_STYLE_BYTES = 1000
MAX_IMAGE_BYTES = 20 * 1024 * 1024
MAX_MULTIPART_BYTES = MAX_IMAGE_BYTES + 256 * 1024
MAX_ARTIFACT_BYTES = 768 * 1024 * 1024
MAX_ARCHIVE_FILES = 128
MAX_UNPACKED_BYTES = 1024 * 1024 * 1024
MAX_TEXTURE_PIXELS = 64 * 1024 * 1024
MAX_PALETTE_COLORS = MAX_PRINTABLE_COLORS
MAX_MODEL_FACES = 1000000
MIN_MODEL_FACE_RATIO = 0.90
MAX_MODEL_FACE_RATIO = 1.25
MODEL_FACE_LIMITS = (100000, 300000, 500000, 1000000)
DEFAULT_MODEL_FACE_LIMIT = 300000
MAX_GENERATION_ATTEMPTS = 1
JOB_STATE_FILENAME = "job.json"
JOB_STATE_VERSION = 1
MAX_JOB_STATE_BYTES = 64 * 1024
MAX_LOCAL_REPAIR_DIAGONAL_RATIO = 0.05
MAX_LOCAL_REPAIR_FACE_RATIO = 0.01
MAX_LOCAL_BOUNDARY_EDGES = 64
DEFAULT_MODEL_SIZE_MM = 100.0
MODEL_ARTIFACT_FORMAT = "obj"
MODEL_QUALITY_FILENAME = "model-quality.json"
STYLE_IDS = ("q_cartoon", "low_poly", "cel_shaded", "enamel_inlay", "sculpture", "custom")
DEFAULT_IMAGE_INSTRUCTION = (
    "Stylize only the content already visible in the reference image. Preserve the exact crop, framing, visible regions, "
    "occlusions, subjects, objects, and background; do not add, remove, reveal, reconstruct, or extend anything."
)
_LOOPBACK_HOSTS = {"127.0.0.1", "localhost", "::1"}
_JOBS_LOCK = threading.RLock()
_JOBS: dict[str, "Job"] = {}
_EXECUTOR = ThreadPoolExecutor(max_workers=1, thread_name_prefix="orca-model-job")
_SHUTDOWN_LOCK = threading.Lock()
_SHUT_DOWN = False


def _environment_flag(name: str) -> bool:
    return os.environ.get(name, "").strip().lower() in {"1", "true", "yes", "on"}


def _preprocess_fallback_enabled() -> bool:
    return _environment_flag("ORCASLICER_AI_ALLOW_PREPROCESS_FALLBACK")


@dataclass
class Job:
    id: str
    source: str
    directory: Path
    state: str = "preprocessing"
    phase: str = "preprocessing"
    message: str = "Preparing model generation request."
    progress: int = 5
    palette: tuple[str, ...] = field(default_factory=tuple)
    palette_roles: dict[str, str] = field(default_factory=dict)
    print_settings: dict[str, Any] = field(default_factory=lambda: asdict(PrintSettings()))
    style: str = "sculpture"
    custom_style: str = ""
    face_limit: int = DEFAULT_MODEL_FACE_LIMIT
    user_prompt: str = ""
    prepared_prompt: str = ""
    input_path: Path | None = None
    raw_preview_path: Path | None = None
    strict_preview_path: Path | None = None
    preview_path: Path | None = None
    model_reference_path: Path | None = None
    preview_content_type: str = ""
    heatmap_path: Path | None = None
    metadata_path: Path | None = None
    background_mask_path: Path | None = None
    subject_mask_path: Path | None = None
    mask_paths: dict[str, Path] = field(default_factory=dict)
    image_metrics: dict[str, Any] = field(default_factory=dict)
    artifact_path: Path | None = None
    artifact_format: str = ""
    attempts: list[dict[str, Any]] = field(default_factory=list)
    updated_at: float = field(default_factory=time.time)
    stop_event: threading.Event = field(default_factory=threading.Event, repr=False)
    delete_requested: bool = field(default=False, repr=False)
    future: Future[Any] | None = field(default=None, repr=False)


class RequestError(Exception):
    def __init__(self, code: str, message: str, status: int, retryable: bool = False):
        super().__init__(message)
        self.code = code
        self.message = message
        self.status = status
        self.retryable = retryable


class JobStopped(Exception):
    pass


class SidecarRestart(Exception):
    """Stops local work while keeping a paid remote task resumable."""

    pass


def extract_allowed_keys(request: dict[str, Any]) -> dict[str, set[str]]:
    allowed: dict[str, set[str]] = {}
    scopes = request.get("allowed_changes", {}).get("scopes", {})
    if not isinstance(scopes, dict):
        return allowed

    for scope, scope_def in scopes.items():
        if scope not in ("print", "filament") or not isinstance(scope_def, dict):
            continue
        keys = scope_def.get("keys", {})
        if isinstance(keys, dict):
            allowed[scope] = {str(key) for key in keys}
    return allowed


def build_system_prompt(request: dict[str, Any]) -> str:
    allowed_changes = request.get("allowed_changes", {})
    guidance = request.get("optimization_guidance", [])
    return (
        "You are a conservative OrcaSlicer print-parameter proposal engine. "
        "Return exactly one JSON object and no markdown or text outside it. "
        "Use this schema: "
        '{"summary":string,"changes":[{"scope":"print"|"filament",'
        '"key":string,"new_value":string|number|boolean,"reason":string}],'
        '"questions":[string]}. '
        f"Return at most {MAX_CHANGES} changes. Only use scope/key pairs present in allowed_changes. "
        "Treat current config values as authoritative. Do not return unchanged values. "
        "Never propose printer or machine geometry, nozzle or bed changes, firmware, custom G-code, "
        "network or host settings, credentials, paths, file operations, profile writes, or commands. "
        "When available information is insufficient for a safe parameter change, explain that in "
        "summary or questions instead of guessing. Use Chinese for summary, reason, and questions.\n\n"
        "allowed_changes:\n"
        + json.dumps(allowed_changes, ensure_ascii=False, separators=(",", ":"))
        + "\n\noptimization_guidance:\n"
        + json.dumps(guidance, ensure_ascii=False, separators=(",", ":"))
    )


def build_user_payload(request: dict[str, Any]) -> dict[str, Any]:
    return {
        "request_id": request.get("request_id", ""),
        "user_message": request.get("user_message", ""),
        "model": request.get("model", {}),
        "config": request.get("config", {}),
    }


def provider_request(request: dict[str, Any]) -> dict[str, Any]:
    try:
        content = complete_text(
            build_system_prompt(request),
            json.dumps(build_user_payload(request), ensure_ascii=False, separators=(",", ":")),
        )
    except OpenAIPreprocessorError as exc:
        raise RuntimeError(str(exc)) from None
    return extract_json_object(content)


def extract_json_object(text: str) -> dict[str, Any]:
    stripped = text.strip()
    if stripped.startswith("```"):
        stripped = re.sub(r"^```(?:json)?\s*", "", stripped, flags=re.IGNORECASE)
        stripped = re.sub(r"\s*```$", "", stripped)

    try:
        parsed = json.loads(stripped)
    except json.JSONDecodeError:
        start = stripped.find("{")
        end = stripped.rfind("}")
        if start < 0 or end <= start:
            raise RuntimeError("The AI service response did not contain a JSON object")
        try:
            parsed = json.loads(stripped[start : end + 1])
        except json.JSONDecodeError as exc:
            raise RuntimeError("The AI service response contained invalid JSON") from exc

    if not isinstance(parsed, dict):
        raise RuntimeError("The AI service response was not a JSON object")
    return parsed


def normalize_proposal(raw: dict[str, Any], request: dict[str, Any]) -> dict[str, Any]:
    allowed = extract_allowed_keys(request)
    normalized: list[dict[str, Any]] = []
    seen: set[tuple[str, str]] = set()
    changes = raw.get("changes", [])

    if isinstance(changes, list):
        for change in changes:
            if not isinstance(change, dict):
                continue
            scope = str(change.get("scope", ""))
            key = str(change.get("key", ""))
            identity = (scope, key)
            if key not in allowed.get(scope, set()) or identity in seen:
                continue
            value = change.get("new_value", change.get("value"))
            if not isinstance(value, (str, int, float, bool)) or value is None:
                continue
            normalized.append(
                {
                    "scope": scope,
                    "key": key,
                    "new_value": value,
                    "reason": str(change.get("reason", "")),
                }
            )
            seen.add(identity)
            if len(normalized) >= MAX_CHANGES:
                break

    summary = raw.get("summary", "")
    questions = raw.get("questions", [])
    assistant_parts = [str(summary).strip()] if str(summary).strip() else []
    if isinstance(questions, list):
        assistant_parts.extend(str(question).strip() for question in questions if str(question).strip())
    if not assistant_parts:
        assistant_parts.append("AI service did not return a displayable explanation.")

    return {
        "request_id": str(request.get("request_id", "")),
        "assistant_text": "\n".join(assistant_parts),
        "proposal": {"changes": normalized},
    }


def _text_field(value: Any, name: str, *, allow_empty: bool = False) -> str:
    if not isinstance(value, str):
        raise RequestError("invalid_request", f"{name} must be a string.", 400)
    value = value.strip()
    if not allow_empty and not value:
        raise RequestError("invalid_request", f"{name} is required.", 400)
    if len(value.encode("utf-8")) > MAX_PROMPT_BYTES:
        raise RequestError("invalid_request", f"{name} exceeds the 2000-byte limit.", 400)
    return value


def _normalize_palette(value: Any) -> tuple[str, ...]:
    if not isinstance(value, list) or len(value) > MAX_PALETTE_COLORS:
        raise RequestError(
            "invalid_palette",
            f"palette must contain between 0 and {MAX_PALETTE_COLORS} colors.",
            400,
        )
    normalized: list[str] = []
    seen: set[str] = set()
    for color in value:
        if not isinstance(color, str) or re.fullmatch(r"#[0-9A-Fa-f]{6}", color) is None:
            raise RequestError("invalid_palette", "palette colors must use #RRGGBB format.", 400)
        canonical = color.upper()
        if canonical not in seen:
            seen.add(canonical)
            normalized.append(canonical)
    return tuple(normalized)


def _multipart_palette(value: Any) -> tuple[str, ...]:
    if not isinstance(value, str):
        raise RequestError("invalid_palette", "palette is required.", 400)
    try:
        parsed = json.loads(value)
    except json.JSONDecodeError:
        raise RequestError("invalid_palette", "palette must be a JSON color array.", 400) from None
    return _normalize_palette(parsed)


def _normalize_palette_roles(value: Any, palette: tuple[str, ...]) -> dict[str, str]:
    if not palette:
        if value in (None, {}):
            return {}
        raise RequestError("invalid_palette_roles", "palette roles require printable colors.", 400)
    if value is None:
        value = {}
    if not isinstance(value, dict) or any(not isinstance(key, str) or not isinstance(color, str) for key, color in value.items()):
        raise RequestError("invalid_palette_roles", "palette_roles must be a color-role object.", 400)
    try:
        return assign_palette_roles(palette, value).color_by_role
    except PrintablePaletteError as exc:
        raise RequestError("invalid_palette_roles", str(exc), 400) from None


def _multipart_palette_roles(value: Any, palette: tuple[str, ...]) -> dict[str, str]:
    if value in (None, ""):
        return _normalize_palette_roles(None, palette)
    if not isinstance(value, str):
        raise RequestError("invalid_palette_roles", "palette_roles must be valid JSON.", 400)
    try:
        parsed = json.loads(value)
    except json.JSONDecodeError:
        raise RequestError("invalid_palette_roles", "palette_roles must be valid JSON.", 400) from None
    return _normalize_palette_roles(parsed, palette)


def _image_type(data: bytes) -> str | None:
    if data.startswith(b"\x89PNG\r\n\x1a\n"):
        return "image/png"
    if data.startswith(b"\xff\xd8\xff"):
        return "image/jpeg"
    return None


def _normalize_style(value: Any) -> str:
    if value is None or value == "":
        return "sculpture"
    if not isinstance(value, str) or value not in STYLE_IDS:
        raise RequestError(
            "invalid_style", "style must be q_cartoon, low_poly, cel_shaded, enamel_inlay, sculpture, or custom.", 400
        )
    return value


def _normalize_custom_style(value: Any, style: str) -> str:
    if value is None:
        value = ""
    if not isinstance(value, str):
        raise RequestError("invalid_custom_style", "custom_style must be a string.", 400)
    description = value.strip()
    if style == "custom":
        if not description:
            raise RequestError("invalid_custom_style", "custom_style is required when style is custom.", 400)
        if len(description.encode("utf-8")) > MAX_CUSTOM_STYLE_BYTES:
            raise RequestError("invalid_custom_style", "custom_style exceeds the 1000-byte limit.", 400)
        return description
    if description:
        raise RequestError("invalid_custom_style", "custom_style is only allowed when style is custom.", 400)
    return ""


def _normalize_face_limit(value: Any) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value not in MODEL_FACE_LIMITS:
        raise RequestError(
            "invalid_face_limit",
            "face_limit must be 100000, 300000, 500000, or 1000000 triangles.",
            400,
        )
    return value


def _validate_face_target(face_count: int, face_limit: int) -> None:
    minimum = math.floor(face_limit * MIN_MODEL_FACE_RATIO)
    maximum = min(MAX_MODEL_FACES, math.ceil(face_limit * MAX_MODEL_FACE_RATIO))
    if face_count < minimum:
        raise TripoError(
            f"The generated OBJ contains {face_count} triangles; the {face_limit}-triangle target requires at least {minimum}."
        )
    if face_count > maximum:
        raise TripoError(
            f"The generated OBJ contains {face_count} triangles; the {face_limit}-triangle target allows at most {maximum}."
        )


def _legacy_face_error_is_recoverable(message: str, face_limit: int) -> bool:
    match = re.search(r"contains\s+(\d+)\s+triangles;\s+at least\s+(\d+)\s+are required", message, re.IGNORECASE)
    if match is None:
        return False
    face_count = int(match.group(1))
    minimum = math.floor(face_limit * MIN_MODEL_FACE_RATIO)
    maximum = min(MAX_MODEL_FACES, math.ceil(face_limit * MAX_MODEL_FACE_RATIO))
    return minimum <= face_count <= maximum


def _normalize_image_instruction(value: Any) -> str:
    if value is None:
        return DEFAULT_IMAGE_INSTRUCTION
    if not isinstance(value, str):
        raise RequestError("invalid_request", "instruction must be UTF-8 text.", 400)
    return value.strip() or DEFAULT_IMAGE_INSTRUCTION


def _normalize_print_settings(value: Any) -> dict[str, Any]:
    try:
        return asdict(PrintSettings.from_mapping(value))
    except PrintableImageError as exc:
        raise RequestError("invalid_print_settings", str(exc), 400) from None


def _model_output_root() -> Path:
    return Path(os.environ.get("ORCASLICER_AI_OUTPUT_DIR", Path.cwd() / "generated_models")).resolve()


def _new_job(
    source: str,
    palette: tuple[str, ...] = (),
    palette_roles: dict[str, str] | None = None,
    style: str = "sculpture",
    custom_style: str = "",
    print_settings: dict[str, Any] | None = None,
) -> Job:
    job_id = str(uuid.uuid4())
    output_root = _model_output_root()
    directory = output_root / job_id
    try:
        directory.mkdir(parents=True, exist_ok=False)
    except OSError:
        raise RequestError("service_unavailable", "The generated-model directory could not be created.", 503, True) from None
    job = Job(
        id=job_id,
        source=source,
        directory=directory,
        palette=palette,
        palette_roles=dict(palette_roles or {}),
        style=style,
        custom_style=custom_style,
        print_settings=print_settings or asdict(PrintSettings()),
    )
    _persist_job(job)
    return job


def _job_path_value(job: Job, path: Path | None) -> str:
    if path is None:
        return ""
    try:
        return path.resolve().relative_to(job.directory.resolve()).as_posix()
    except (OSError, ValueError):
        return ""


def _job_file(job: Job, value: Any) -> Path | None:
    if not isinstance(value, str) or not value:
        return None
    candidate = (job.directory / value).resolve()
    try:
        candidate.relative_to(job.directory.resolve())
    except ValueError:
        return None
    return candidate


def _persist_job(job: Job, *, touch: bool = True) -> None:
    if touch:
        job.updated_at = time.time()
    payload = {
        "version": JOB_STATE_VERSION,
        "id": job.id,
        "source": job.source,
        "state": job.state,
        "phase": job.phase,
        "message": job.message,
        "progress": job.progress,
        "palette": list(job.palette),
        "palette_roles": job.palette_roles,
        "print_settings": job.print_settings,
        "style": job.style,
        "custom_style": job.custom_style,
        "face_limit": job.face_limit,
        "user_prompt": job.user_prompt,
        "prepared_prompt": job.prepared_prompt,
        "input_path": _job_path_value(job, job.input_path),
        "raw_preview_path": _job_path_value(job, job.raw_preview_path),
        "strict_preview_path": _job_path_value(job, job.strict_preview_path),
        "preview_path": _job_path_value(job, job.preview_path),
        "model_reference_path": _job_path_value(job, job.model_reference_path),
        "preview_content_type": job.preview_content_type,
        "heatmap_path": _job_path_value(job, job.heatmap_path),
        "metadata_path": _job_path_value(job, job.metadata_path),
        "background_mask_path": _job_path_value(job, job.background_mask_path),
        "subject_mask_path": _job_path_value(job, job.subject_mask_path),
        "mask_paths": {key: _job_path_value(job, path) for key, path in job.mask_paths.items()},
        "image_metrics": job.image_metrics,
        "artifact_path": _job_path_value(job, job.artifact_path),
        "artifact_format": job.artifact_format,
        "attempts": job.attempts,
        "updated_at": job.updated_at,
    }
    temporary = job.directory / f"{JOB_STATE_FILENAME}.part"
    destination = job.directory / JOB_STATE_FILENAME
    try:
        encoded = json.dumps(payload, ensure_ascii=False, indent=2)
        if len(encoded.encode("utf-8")) > MAX_JOB_STATE_BYTES:
            raise OSError("job state exceeds its size limit")
        temporary.write_text(encoded, encoding="utf-8")
        os.replace(temporary, destination)
    except OSError:
        try:
            temporary.unlink(missing_ok=True)
        except OSError:
            pass


def _load_job(directory: Path) -> Job | None:
    state_path = directory / JOB_STATE_FILENAME
    try:
        if not state_path.is_file() or state_path.stat().st_size > MAX_JOB_STATE_BYTES:
            return None
        payload = json.loads(state_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError):
        return None
    if not isinstance(payload, dict) or payload.get("version") != JOB_STATE_VERSION:
        return None
    job_id = payload.get("id")
    source = payload.get("source")
    if not isinstance(job_id, str) or directory.name != job_id or source not in {"text", "image"}:
        return None
    try:
        palette = _normalize_palette(payload.get("palette", []))
        palette_roles = _normalize_palette_roles(payload.get("palette_roles"), palette)
        style = _normalize_style(payload.get("style"))
        custom_style = _normalize_custom_style(payload.get("custom_style"), style)
        face_limit = _normalize_face_limit(payload.get("face_limit", DEFAULT_MODEL_FACE_LIMIT))
        print_settings = _normalize_print_settings(payload.get("print_settings"))
    except RequestError:
        return None
    attempts = payload.get("attempts", [])
    if not isinstance(attempts, list) or any(not isinstance(attempt, dict) for attempt in attempts):
        return None
    job = Job(
        id=job_id,
        source=source,
        directory=directory,
        palette=palette,
        palette_roles=palette_roles,
        style=style,
        custom_style=custom_style,
        face_limit=face_limit,
        print_settings=print_settings,
    )
    job.state = str(payload.get("state", "failed"))
    job.phase = str(payload.get("phase", job.state))
    job.message = str(payload.get("message", "Recovered model job."))
    job.progress = max(0, min(int(payload.get("progress", 0)), 100))
    job.user_prompt = str(payload.get("user_prompt", ""))
    job.prepared_prompt = str(payload.get("prepared_prompt", ""))
    job.input_path = _job_file(job, payload.get("input_path"))
    job.raw_preview_path = _job_file(job, payload.get("raw_preview_path"))
    job.strict_preview_path = _job_file(job, payload.get("strict_preview_path"))
    job.preview_path = _job_file(job, payload.get("preview_path"))
    job.model_reference_path = _job_file(job, payload.get("model_reference_path"))
    job.preview_content_type = str(payload.get("preview_content_type", ""))
    job.heatmap_path = _job_file(job, payload.get("heatmap_path"))
    job.metadata_path = _job_file(job, payload.get("metadata_path"))
    job.background_mask_path = _job_file(job, payload.get("background_mask_path"))
    job.subject_mask_path = _job_file(job, payload.get("subject_mask_path"))
    raw_masks = payload.get("mask_paths", {})
    if isinstance(raw_masks, dict):
        job.mask_paths = {
            str(key): path for key, value in raw_masks.items()
            if (path := _job_file(job, value)) is not None
        }
    raw_metrics = payload.get("image_metrics", {})
    job.image_metrics = raw_metrics if isinstance(raw_metrics, dict) else {}
    job.artifact_path = _job_file(job, payload.get("artifact_path"))
    job.artifact_format = str(payload.get("artifact_format", ""))
    job.attempts = attempts
    try:
        job.updated_at = float(payload.get("updated_at", state_path.stat().st_mtime))
    except (TypeError, ValueError, OSError):
        job.updated_at = time.time()
    return job


def _restore_jobs() -> None:
    output_root = _model_output_root()
    try:
        directories = [path for path in output_root.iterdir() if path.is_dir()]
    except OSError:
        return
    restored: list[Job] = []
    for directory in directories:
        job = _load_job(directory)
        if job is None:
            continue
        latest_attempt = job.attempts[-1] if job.attempts else {}
        recoverable_error = str(latest_attempt.get("error", "")).lower()
        can_retry_download = (
            job.state == "failed"
            and isinstance(latest_attempt.get("generation_task_id"), str)
            and bool(latest_attempt.get("generation_task_id"))
            and isinstance(latest_attempt.get("conversion_task_id"), str)
            and bool(latest_attempt.get("conversion_task_id"))
            and (any(marker in recoverable_error for marker in (
                "unsafe artifact location",
                "invalid obj package",
                "artifact host could not be resolved",
                "artifact could not be downloaded",
                "temporarily unavailable",
                "rate limiting",
                "deadline expired",
            )) or _legacy_face_error_is_recoverable(recoverable_error, job.face_limit))
        )
        if can_retry_download:
            job.state = "queued"
            job.phase = "resuming"
            job.message = "Retrying the existing remote artifact download after restart."
            job.progress = max(75, job.progress)
        if job.state == "preprocessing":
            job.state = "failed"
            job.phase = "failed"
            job.message = "The sidecar restarted while creating the preview. Create the preview again manually."
            job.progress = 0
        if job.state in {"queued", "running", "stopping"}:
            generation_id = next(
                (attempt.get("generation_task_id") for attempt in reversed(job.attempts)
                 if isinstance(attempt.get("generation_task_id"), str) and attempt.get("generation_task_id")),
                "",
            )
            if generation_id:
                job.state = "queued"
                job.phase = "resuming"
                job.message = "Resuming the existing paid model task after restart."
                job.progress = max(20, job.progress)
            else:
                job.state = "failed"
                job.phase = "failed"
                job.message = "The sidecar restarted before the paid task reference was saved. Start a new generation manually."
                job.progress = 0
        restored.append(job)
    with _JOBS_LOCK:
        for job in restored:
            _JOBS[job.id] = job
            _persist_job(job, touch=False)
    for job in restored:
        if job.state == "queued" and job.phase == "resuming":
            _submit(job, _generate_job, job.prepared_prompt, True)


def _adopt_legacy_completed_job(job_id: str) -> Job | None:
    """Register a pre-manifest model library entry without accepting an arbitrary path."""
    try:
        if str(uuid.UUID(job_id)) != job_id.lower():
            return None
    except ValueError:
        return None
    output_root = _model_output_root()
    try:
        directory = (output_root / job_id).resolve(strict=True)
        directory.relative_to(output_root)
        if not directory.is_dir():
            return None
        artifact = (directory / "model-vertex-color.obj").resolve(strict=True)
        artifact.relative_to(directory)
        artifact_size = artifact.stat().st_size
        if not artifact.is_file() or artifact_size <= 0 or artifact_size > MAX_ARTIFACT_BYTES:
            return None
    except (OSError, ValueError):
        return None

    job = Job(id=job_id, source="image", directory=directory)
    job.state = "ready"
    job.phase = "ready"
    job.message = "Recovered historical model library entry."
    job.progress = 100
    job.artifact_path = artifact
    job.artifact_format = MODEL_ARTIFACT_FORMAT
    preview = directory / "preview.png"
    try:
        resolved_preview = preview.resolve(strict=True)
        resolved_preview.relative_to(directory)
        if resolved_preview.is_file() and resolved_preview.stat().st_size > 0:
            job.preview_path = resolved_preview
            job.preview_content_type = "image/png"
    except (OSError, ValueError):
        pass
    try:
        job.updated_at = artifact.stat().st_mtime
    except OSError:
        pass

    with _JOBS_LOCK:
        existing = _JOBS.get(job_id)
        if existing is not None:
            return existing
        _JOBS[job_id] = job
    return job


def _file_info(path: Path | None) -> tuple[bool, int]:
    if path is None:
        return False, 0
    try:
        size = path.stat().st_size
    except OSError:
        return False, 0
    return size > 0, size


def _stored_image_type(path: Path | None) -> str:
    if path is None:
        return ""
    try:
        with path.open("rb") as stream:
            return _image_type(stream.read(16)) or ""
    except OSError:
        return ""


def _read_job_report(job: Job, filename: str) -> dict[str, Any]:
    path = job.directory / filename
    try:
        if path.is_file() and path.stat().st_size <= MAX_JOB_STATE_BYTES:
            candidate = json.loads(path.read_text(encoding="utf-8"))
            if isinstance(candidate, dict):
                return candidate
    except (OSError, UnicodeError, json.JSONDecodeError):
        pass
    return {}


def _public_job(job: Job) -> dict[str, Any]:
    input_ready, input_size = _file_info(job.input_path)
    preview_ready, preview_size = _file_info(job.preview_path)
    raw_preview_ready, raw_preview_size = _file_info(job.raw_preview_path)
    strict_preview_ready, strict_preview_size = _file_info(job.strict_preview_path)
    model_reference_ready, model_reference_size = _file_info(job.model_reference_path)
    heatmap_ready, heatmap_size = _file_info(job.heatmap_path)
    metadata_ready, metadata_size = _file_info(job.metadata_path)
    artifact_ready, artifact_size = _file_info(job.artifact_path)
    model_quality = _read_job_report(job, MODEL_QUALITY_FILENAME)
    visual_quality = _read_job_report(job, VISUAL_QUALITY_FILENAME)
    model_view_sheet_ready, model_view_sheet_size = _file_info(job.directory / "model-view-sheet.png")
    artifact_filename = ""
    if artifact_ready:
        artifact_filename = f"orcaslicer-model-{job.id}.{job.artifact_format}"
    return {
        "id": job.id,
        "source": job.source,
        "style": job.style,
        "custom_style": job.custom_style,
        "face_limit": job.face_limit,
        "state": job.state,
        "phase": job.phase,
        "message": job.message,
        "progress": job.progress,
        "attempt": len(job.attempts),
        "max_attempts": MAX_GENERATION_ATTEMPTS,
        "prepared_prompt": job.prepared_prompt if job.source == "text" else "",
        "user_prompt": job.user_prompt,
        "palette": list(job.palette),
        "palette_roles": job.palette_roles,
        "print": job.print_settings,
        "image_metrics": job.image_metrics,
        "model_quality": model_quality,
        "visual_quality": visual_quality,
        "model_views": {
            "ready": model_view_sheet_ready,
            "size_bytes": model_view_sheet_size if model_view_sheet_ready else 0,
        },
        "updated_at": job.updated_at,
        "input": {
            "ready": input_ready,
            "content_type": _stored_image_type(job.input_path) if input_ready else "",
            "size_bytes": input_size if input_ready else 0,
        },
        "preview": {
            "ready": preview_ready,
            "content_type": job.preview_content_type if preview_ready else "",
            "size_bytes": preview_size if preview_ready else 0,
        },
        "image_outputs": {
            "raw_preview": {"ready": raw_preview_ready, "size_bytes": raw_preview_size},
            "strict_preview": {"ready": strict_preview_ready, "size_bytes": strict_preview_size},
            "clean_preview": {"ready": preview_ready, "size_bytes": preview_size},
            "model_reference": {"ready": model_reference_ready, "size_bytes": model_reference_size},
            "heatmap": {"ready": heatmap_ready, "size_bytes": heatmap_size},
            "metadata": {"ready": metadata_ready, "size_bytes": metadata_size},
            "masks": sorted(job.mask_paths),
        },
        "artifact": {
            "ready": artifact_ready,
            "format": job.artifact_format if artifact_ready else "",
            "color_encoding": "vertex_colors" if artifact_ready and job.artifact_format == "obj" else "",
            "filename": artifact_filename,
            "size_bytes": artifact_size if artifact_ready else 0,
        },
    }


def _cleanup_job(job: Job) -> None:
    # Job removal only releases in-memory state. Generated inputs, previews, and
    # model resources are user artifacts and remain available on disk.
    return


def _remove_job_state(job: Job) -> None:
    for name in (JOB_STATE_FILENAME, f"{JOB_STATE_FILENAME}.part"):
        try:
            (job.directory / name).unlink(missing_ok=True)
        except OSError:
            pass


def _finish_deleted(job: Job) -> None:
    with _JOBS_LOCK:
        if job.delete_requested:
            _JOBS.pop(job.id, None)
            _remove_job_state(job)
        else:
            _persist_job(job)


def _mark_stopped(job: Job) -> None:
    with _JOBS_LOCK:
        job.state = "stopped"
        job.phase = "stopped"
        job.message = "Model generation stopped."
        job.progress = 0
        job.artifact_path = None
        job.artifact_format = ""
        _persist_job(job)


def _stop_boundary(job: Job) -> None:
    if job.stop_event.is_set():
        if _SHUT_DOWN:
            raise SidecarRestart()
        _mark_stopped(job)
        raise JobStopped()


def _fail_job(job: Job, message: str) -> None:
    with _JOBS_LOCK:
        if job.stop_event.is_set():
            job.state = "stopped"
            job.phase = "stopped"
            job.message = "Model generation stopped."
            job.progress = 0
        else:
            job.state = "failed"
            job.phase = "failed"
            job.message = message
        job.artifact_path = None
        job.artifact_format = ""
        _persist_job(job)


def _generation_prompt(prompt: str, palette: tuple[str, ...]) -> str:
    suffix = (
        " Generate a watertight printable model with a stable flat base. Preserve meaningful separate parts "
        "and material regions in their original relative positions; do not create unintended floating debris, "
        "internal shells, holes, or non-manifold geometry."
    )
    if palette:
        suffix += " Use only these printable filament colors: " + ", ".join(palette) + "."
    else:
        suffix += " Preserve coherent natural colors with broad, clean material regions."
    max_prefix_bytes = MAX_PROMPT_BYTES - len(suffix.encode("utf-8"))
    prefix = prompt.strip().encode("utf-8")[:max(0, max_prefix_bytes)].decode("utf-8", errors="ignore").rstrip()
    return prefix + suffix


def _is_warm_skin_color(color: tuple[int, int, int]) -> bool:
    red, green, blue = color
    return (
        red > green >= blue
        and red - blue >= 14
        and red >= 100
        and blue >= 35
        and green >= red * 0.45
    )


def _is_printable_skin_color(color: tuple[int, int, int]) -> bool:
    red, green, blue = color
    return _is_warm_skin_color(color) and red - green >= 15


def _find_face_skin_mask(
    pixels: list[tuple[int, int, int]],
    width: int,
    height: int,
    background: bytes,
    foreground_box: tuple[int, int, int, int],
    style: str,
) -> bytes:
    left, top, right, bottom = foreground_box
    subject_width = max(1, right - left)
    subject_height = max(1, bottom - top)
    search_bottom = min(bottom, top + max(1, int(subject_height * 0.48)))
    candidates = bytearray(width * height)
    for y in range(top, search_bottom):
        row = y * width
        for x in range(left, right):
            offset = row + x
            if not background[offset] and _is_warm_skin_color(pixels[offset]):
                candidates[offset] = 1

    visited = bytearray(width * height)
    best_component: list[int] = []
    best_score = float("inf")
    target_y = top + subject_height * 0.08
    minimum_area = max(64, int(subject_width * subject_height * 0.0015))
    for seed in range(top * width, search_bottom * width):
        if not candidates[seed] or visited[seed]:
            continue
        visited[seed] = 1
        pending: deque[int] = deque([seed])
        component: list[int] = []
        sum_x = 0
        sum_y = 0
        while pending:
            offset = pending.popleft()
            x = offset % width
            y = offset // width
            component.append(offset)
            sum_x += x
            sum_y += y
            if x > left:
                neighbor = offset - 1
                if candidates[neighbor] and not visited[neighbor]:
                    visited[neighbor] = 1
                    pending.append(neighbor)
            if x + 1 < right:
                neighbor = offset + 1
                if candidates[neighbor] and not visited[neighbor]:
                    visited[neighbor] = 1
                    pending.append(neighbor)
            if y > top:
                neighbor = offset - width
                if candidates[neighbor] and not visited[neighbor]:
                    visited[neighbor] = 1
                    pending.append(neighbor)
            if y + 1 < search_bottom:
                neighbor = offset + width
                if candidates[neighbor] and not visited[neighbor]:
                    visited[neighbor] = 1
                    pending.append(neighbor)

        if len(component) < minimum_area:
            continue
        center_x = sum_x / len(component)
        center_y = sum_y / len(component)
        horizontal_distance = abs(center_x - (left + right) * 0.5) / subject_width
        if horizontal_distance > 0.32:
            continue
        vertical_distance = abs(center_y - target_y) / subject_height
        area_ratio = len(component) / (subject_width * subject_height)
        score = vertical_distance + horizontal_distance * 1.5 - min(0.08, area_ratio * 0.4)
        if score < best_score:
            best_score = score
            best_component = component

    mask = bytearray(width * height)
    if not best_component:
        return bytes(mask)
    component_left = min(offset % width for offset in best_component)
    component_right = max(offset % width for offset in best_component) + 1
    component_top = min(offset // width for offset in best_component)
    component_bottom = max(offset // width for offset in best_component) + 1
    component_width = component_right - component_left
    component_height = component_bottom - component_top
    expansion_x = int(max(2, min(component_width, subject_width * 0.18), subject_width * 0.08))
    expansion_y = int(max(2, min(component_height, subject_height * 0.18), subject_height * 0.08))
    face_region_bottom = top + int(subject_height * (0.39 if style == "q_cartoon" else 0.17))
    for y in range(
        max(top, component_top - expansion_y),
        min(search_bottom, face_region_bottom, component_bottom + expansion_y),
    ):
        row = y * width
        for x in range(max(left, component_left - expansion_x), min(right, component_right + expansion_x)):
            offset = row + x
            red, green, blue = pixels[offset]
            relaxed_skin = (
                red >= green >= blue
                and red - blue >= 6
                and red >= 120
                and green >= red * 0.68
            )
            if not background[offset] and relaxed_skin:
                mask[offset] = 255
    return bytes(mask)


def _quantize_image_to_palette(
    path: Path,
    palette: tuple[str, ...],
    style: str = "sculpture",
) -> dict[str, int]:
    try:
        from PIL import Image, ImageFilter, ImageOps, UnidentifiedImageError
    except ImportError:
        raise OpenAIPreprocessorError("Pillow is required to constrain preview colors.") from None
    palette_rgb = [tuple(int(color[index : index + 2], 16) for index in (1, 3, 5)) for color in palette]
    temporary = path.with_name(path.name + ".quantized")
    try:
        with Image.open(path) as source:
            if source.width <= 0 or source.height <= 0 or source.width * source.height > MAX_TEXTURE_PIXELS:
                raise OpenAIPreprocessorError("The prepared preview has an invalid size.")
            alpha = source.getchannel("A") if "A" in source.getbands() else None
            smoothed = source.convert("RGB").filter(ImageFilter.MedianFilter(size=3))
            palette_lab = [_srgb_to_lab(color) for color in palette_rgb]

            border_step = max(1, min(source.width, source.height) // 256)
            border_pixels = []
            for x in range(0, source.width, border_step):
                border_pixels.extend((smoothed.getpixel((x, 0)), smoothed.getpixel((x, source.height - 1))))
            for y in range(0, source.height, border_step):
                border_pixels.extend((smoothed.getpixel((0, y)), smoothed.getpixel((source.width - 1, y))))
            border_rgb = tuple(sorted(pixel[channel] for pixel in border_pixels)[len(border_pixels) // 2] for channel in range(3))
            smoothed_pixels = list(smoothed.getdata())
            border_chroma = max(border_rgb) - min(border_rgb)
            background_candidates = bytes(
                255
                if max(abs(pixel[channel] - border_rgb[channel]) for channel in range(3)) <= 36
                and (border_chroma > 24 or max(pixel) - min(pixel) <= max(24, border_chroma + 12))
                else 0
                for pixel in smoothed_pixels
            )
            connected_background = bytearray(source.width * source.height)
            pending: deque[int] = deque()

            def enqueue(offset: int) -> None:
                if background_candidates[offset] and not connected_background[offset]:
                    connected_background[offset] = 255
                    pending.append(offset)

            for x in range(source.width):
                enqueue(x)
                enqueue((source.height - 1) * source.width + x)
            for y in range(source.height):
                enqueue(y * source.width)
                enqueue(y * source.width + source.width - 1)
            while pending:
                offset = pending.popleft()
                x = offset % source.width
                if x > 0:
                    enqueue(offset - 1)
                if x + 1 < source.width:
                    enqueue(offset + 1)
                if offset >= source.width:
                    enqueue(offset - source.width)
                if offset + source.width < len(connected_background):
                    enqueue(offset + source.width)
            background_mask = Image.frombytes("L", source.size, bytes(connected_background))
            background_index = min(
                range(len(palette_lab)),
                key=lambda index: sum(
                    (left - right) ** 2 for left, right in zip(_srgb_to_lab(border_rgb), palette_lab[index])
                ),
            )

            cluster_source = smoothed.copy()
            cluster_source.paste(border_rgb, (0, 0, source.width, source.height), background_mask)
            adaptive = cluster_source.quantize(
                colors=min(64, max(len(palette_rgb) * 3, len(palette_rgb))),
                method=Image.Quantize.MEDIANCUT,
                dither=Image.Dither.NONE,
            )
            histogram = adaptive.getcolors(maxcolors=source.width * source.height) or []
            used_indices = sorted(index for count, index in histogram if count > 0)
            adaptive_palette = adaptive.getpalette() or []
            source_colors = [tuple(adaptive_palette[index * 3 : index * 3 + 3]) for index in used_indices]
            source_lab = [_srgb_to_lab(color) for color in source_colors]
            assignment = [
                min(
                    range(len(palette_rgb)),
                    key=lambda palette_index: sum(
                        (color_lab[channel] - palette_lab[palette_index][channel]) ** 2 for channel in range(3)
                    ),
                )
                for color_lab in source_lab
            ]
            index_map = {source_index: palette_index for source_index, palette_index in zip(used_indices, assignment)}
            mapped = adaptive.point([index_map.get(index, 0) for index in range(256)], mode="P")
            palette_bytes = [channel for color in palette_rgb for channel in color]
            palette_bytes.extend(list(palette_rgb[0]) * (256 - len(palette_rgb)))
            mapped.putpalette(palette_bytes)

            mapped_data = bytearray(mapped.getdata())
            background_data = bytes(background_mask.getdata())
            foreground_box = ImageOps.invert(background_mask).getbbox()
            if foreground_box is None:
                raise OpenAIPreprocessorError("The style preview does not contain a printable subject.")

            skin_palette = [index for index, color in enumerate(palette_rgb) if _is_printable_skin_color(color)]
            if skin_palette:
                skin_mask = _find_face_skin_mask(
                    smoothed_pixels,
                    source.width,
                    source.height,
                    background_data,
                    foreground_box,
                    style,
                )
                adaptive_data = bytes(adaptive.getdata())
                skin_assignment = {
                    source_index: min(
                        skin_palette,
                        key=lambda palette_index: sum(
                            (source_lab[source_position][channel] - palette_lab[palette_index][channel]) ** 2
                            for channel in range(3)
                        ),
                    )
                    for source_position, source_index in enumerate(used_indices)
                }
                for offset, is_skin in enumerate(skin_mask):
                    if is_skin:
                        mapped_data[offset] = skin_assignment.get(adaptive_data[offset], mapped_data[offset])

            for offset, is_background in enumerate(background_data):
                if is_background:
                    mapped_data[offset] = background_index

            mapped.putdata(mapped_data)
            mapped = mapped.filter(ImageFilter.ModeFilter(size=3))
            quantized = mapped.convert("RGB")
            if alpha is not None:
                quantized.putalpha(alpha)
            quantized.save(temporary, format="PNG")
            counts = quantized.convert("RGB").getcolors(maxcolors=source.width * source.height) or []
            usage = {"#%02X%02X%02X" % color: count for count, color in counts}
            if not set(usage).issubset(palette):
                raise OpenAIPreprocessorError("The style preview contains colors outside the printable filament palette.")
            if len(usage) < min(3, len(palette)):
                raise OpenAIPreprocessorError("The style preview needs more distinct printable color regions.")
        os.replace(temporary, path)
        return {color: usage[color] for color in palette if color in usage}
    except OpenAIPreprocessorError:
        raise
    except (OSError, UnidentifiedImageError, Image.DecompressionBombError):
        raise OpenAIPreprocessorError("The prepared preview could not be color constrained.") from None
    finally:
        try:
            temporary.unlink(missing_ok=True)
        except OSError:
            pass


def _apply_printable_image_pipeline(job: Job, raw_preview: Path) -> dict[str, int]:
    try:
        result = process_printable_image(
            raw_preview, job.directory, job.palette, job.print_settings, palette_roles=job.palette_roles
        )
    except PrintableImageError as exc:
        raise OpenAIPreprocessorError(str(exc)) from None
    job.raw_preview_path = raw_preview
    job.strict_preview_path = result.strict_preview
    job.preview_path = result.clean_preview
    job.model_reference_path = result.model_reference
    job.heatmap_path = result.heatmap
    job.metadata_path = result.metadata
    job.background_mask_path = result.background_mask
    job.subject_mask_path = result.subject_mask
    job.mask_paths = result.masks
    job.image_metrics = result.metrics
    return result.palette_usage


def _printable_preview_message(job: Job, fallback: str) -> str:
    if job.palette and not bool(job.image_metrics.get("palette_quality_ok", True)):
        subject_ratio = float(job.image_metrics.get("printable_subject_area_ratio", 0.0))
        continuity = float(job.image_metrics.get("largest_subject_component_ratio", 0.0))
        if subject_ratio < 0.18:
            return "The printable subject is too small in the preview; regenerate with a larger subject."
        if continuity < 0.90:
            return "The printable subject is disconnected; regenerate with one connected subject."
        return "The printable preview failed its geometry quality check; regenerate the preview."
    return fallback


def _preprocess_text_job(job: Job, prompt: str) -> None:
    try:
        _stop_boundary(job)
        prepared = _generation_prompt(preprocess_text(prompt, job.palette, job.style, job.custom_style), job.palette)
        if not prepared or len(prepared.encode("utf-8")) > MAX_PROMPT_BYTES:
            raise OpenAIPreprocessorError("The prepared prompt is empty or exceeds the 2000-byte limit.")
        if job.palette:
            raw_preview = job.directory / "style-preview-raw.png"
            generate_image(
                prompt, raw_preview, job.palette, job.style,
                str(job.print_settings.get("shadow_color", "blue")), job.palette_roles, job.custom_style,
            )
            color_usage = _apply_printable_image_pipeline(job, raw_preview)
            (job.directory / "preview-colors.json").write_text(
                json.dumps(
                    {
                        "style": job.style,
                        "palette_constrained": True,
                        "palette_pixels": color_usage,
                        "palette_roles": job.palette_roles,
                        "print": job.print_settings,
                        "metrics": job.image_metrics,
                    },
                    ensure_ascii=False,
                    indent=2,
                ),
                encoding="utf-8",
            )
            job.preview_content_type = "image/png"
        with _JOBS_LOCK:
            if job.stop_event.is_set():
                raise JobStopped()
            job.prepared_prompt = prepared
            job.state = "awaiting_confirmation"
            job.phase = "awaiting_confirmation"
            job.message = _printable_preview_message(job, "Review the prepared prompt before generation.")
            job.progress = 15
    except JobStopped:
        _mark_stopped(job)
    except OpenAIPreprocessorError as exc:
        if not _preprocess_fallback_enabled():
            _fail_job(job, str(exc))
        else:
            with _JOBS_LOCK:
                job.prepared_prompt = _generation_prompt(prompt, job.palette)
                job.state = "awaiting_confirmation"
                job.phase = "awaiting_confirmation"
                job.message = "Preprocessing is unavailable; review the original prompt before generation."
                job.progress = 15
    except Exception:
        _fail_job(job, "Text preprocessing failed.")
    finally:
        _finish_deleted(job)


def _preprocess_image_job(job: Job, input_path: Path, instruction: str) -> None:
    raw_preview = job.directory / "style-preview-raw.png"
    preview = job.directory / "preview.png"
    try:
        _stop_boundary(job)
        preprocess_image(
            input_path,
            instruction,
            raw_preview,
            job.palette,
            job.style,
            str(job.print_settings.get("shadow_color", "blue")),
            job.palette_roles,
            job.custom_style,
        )
        job.raw_preview_path = raw_preview
        if job.palette:
            color_usage = _apply_printable_image_pipeline(job, raw_preview)
            preview = job.preview_path or preview
        else:
            shutil.copyfile(raw_preview, preview)
            job.preview_path = preview
            color_usage = {}
        (job.directory / "preview-colors.json").write_text(
            json.dumps(
                {
                    "style": job.style,
                    "palette_constrained": bool(job.palette),
                    "palette_pixels": color_usage,
                    "palette_roles": job.palette_roles,
                    "print": job.print_settings,
                    "metrics": job.image_metrics,
                },
                ensure_ascii=False,
                indent=2,
            ),
            encoding="utf-8",
        )
        _stop_boundary(job)
        try:
            size = preview.stat().st_size
            signature = preview.read_bytes()[:16]
        except OSError:
            raise OpenAIPreprocessorError("The prepared preview could not be read.") from None
        content_type = _image_type(signature)
        if not content_type or size <= 0 or size > MAX_IMAGE_BYTES:
            raise OpenAIPreprocessorError("The prepared preview is not a valid PNG or JPEG image.")
        with _JOBS_LOCK:
            if job.stop_event.is_set():
                raise JobStopped()
            job.preview_path = preview
            job.preview_content_type = content_type
            job.state = "awaiting_confirmation"
            job.phase = "awaiting_confirmation"
            job.message = _printable_preview_message(job, "Review the prepared image before generation.")
            job.progress = 15
    except JobStopped:
        _mark_stopped(job)
    except OpenAIPreprocessorError as exc:
        _fail_job(job, str(exc))
    except Exception:
        _fail_job(job, "Image preprocessing failed.")
    finally:
        _finish_deleted(job)


def _progress_callback(job: Job, start: int, end: int) -> Callable[[int | float | None], None]:
    def update(value: int | float | None) -> None:
        try:
            fraction = max(0.0, min(float(value), 100.0)) / 100.0
        except (TypeError, ValueError):
            return
        with _JOBS_LOCK:
            if not job.stop_event.is_set():
                job.progress = start + int((end - start) * fraction)
                _persist_job(job)

    return update


def _safe_package_path(root: Path, name: str) -> Path:
    normalized = name.replace("\\", "/")
    if not normalized or normalized.startswith("/") or re.match(r"^[A-Za-z]:", normalized):
        raise TripoError("The generated OBJ package contains an unsafe path.")
    parts = normalized.split("/")
    if any(part in {"", ".", ".."} for part in parts):
        raise TripoError("The generated OBJ package contains an unsafe path.")
    destination = root.joinpath(*parts).resolve()
    try:
        destination.relative_to(root.resolve())
    except ValueError:
        raise TripoError("The generated OBJ package contains an unsafe path.") from None
    return destination


def _extract_obj_package(archive: Path, package_dir: Path) -> Path:
    try:
        with zipfile.ZipFile(archive) as bundle:
            members = bundle.infolist()
            if not members or len(members) > MAX_ARCHIVE_FILES:
                raise TripoError("The generated OBJ package contains an invalid number of files.")
            total_size = 0
            destinations: set[Path] = set()
            for member in members:
                mode = member.external_attr >> 16
                if stat.S_ISLNK(mode):
                    raise TripoError("The generated OBJ package contains a symbolic link.")
                file_type = stat.S_IFMT(mode)
                if file_type not in {0, stat.S_IFREG, stat.S_IFDIR}:
                    raise TripoError("The generated OBJ package contains an unsupported file type.")
                if member.flag_bits & 0x1:
                    raise TripoError("The generated OBJ package must not be encrypted.")
                destination = _safe_package_path(package_dir, member.filename)
                if destination in destinations:
                    raise TripoError("The generated OBJ package contains duplicate paths.")
                destinations.add(destination)
                if not member.is_dir():
                    total_size += member.file_size
                    if total_size > MAX_UNPACKED_BYTES:
                        raise TripoError("The generated OBJ package is too large after extraction.")

            package_dir.mkdir(parents=True, exist_ok=False)
            extracted_size = 0
            for member in members:
                destination = _safe_package_path(package_dir, member.filename)
                if member.is_dir():
                    destination.mkdir(parents=True, exist_ok=True)
                    continue
                destination.parent.mkdir(parents=True, exist_ok=True)
                try:
                    source = bundle.open(member)
                    target = destination.open("xb")
                except (OSError, RuntimeError, zipfile.BadZipFile):
                    raise TripoError("The generated OBJ package could not be extracted.") from None
                with source, target:
                    while True:
                        chunk = source.read(1024 * 1024)
                        if not chunk:
                            break
                        extracted_size += len(chunk)
                        if extracted_size > MAX_UNPACKED_BYTES:
                            raise TripoError("The generated OBJ package is too large after extraction.")
                        target.write(chunk)
    except TripoError:
        raise
    except (OSError, RuntimeError, zipfile.BadZipFile, zipfile.LargeZipFile):
        raise TripoError("Tripo returned an invalid OBJ package.") from None

    objects = [path for path in package_dir.rglob("*") if path.is_file() and path.suffix.lower() == ".obj"]
    if len(objects) != 1:
        raise TripoError("The generated OBJ package must contain exactly one OBJ model.")
    return objects[0]


def _obj_dependency_path(package_dir: Path, parent: Path, value: str, kind: str) -> Path:
    value = value.strip().strip('"')
    if not value:
        raise TripoError(f"The generated OBJ has an invalid {kind} reference.")
    normalized = value.replace("\\", "/")
    if normalized.startswith("/") or re.match(r"^[A-Za-z]:", normalized):
        raise TripoError(f"The generated OBJ has an unsafe {kind} reference.")
    destination = parent.joinpath(*normalized.split("/")).resolve()
    try:
        destination.relative_to(package_dir.resolve())
    except ValueError:
        raise TripoError(f"The generated OBJ has an unsafe {kind} reference.") from None
    if not destination.is_file():
        raise TripoError(f"The generated OBJ is missing its {kind} file.")
    return destination


def _read_obj_geometry(obj_path: Path) -> tuple[array, array, list[str]]:
    positions = array("d")
    texcoords = array("d")
    material_libraries: list[str] = []
    try:
        with obj_path.open("r", encoding="utf-8", errors="strict") as stream:
            for line in stream:
                stripped = line.strip()
                if not stripped or stripped.startswith("#"):
                    continue
                fields = stripped.split()
                keyword = fields[0].lower()
                if keyword == "v":
                    if len(fields) < 4:
                        raise TripoError("The generated OBJ has an invalid vertex.")
                    try:
                        values = [float(value) for value in fields[1:4]]
                    except ValueError:
                        raise TripoError("The generated OBJ has an invalid vertex.") from None
                    if not all(math.isfinite(value) for value in values):
                        raise TripoError("The generated OBJ has an invalid vertex.")
                    positions.extend(values)
                elif keyword == "vt":
                    if len(fields) < 3:
                        raise TripoError("The generated OBJ has an invalid texture coordinate.")
                    try:
                        values = [float(value) for value in fields[1:3]]
                    except ValueError:
                        raise TripoError("The generated OBJ has an invalid texture coordinate.") from None
                    if not all(math.isfinite(value) for value in values):
                        raise TripoError("The generated OBJ has an invalid texture coordinate.")
                    texcoords.extend(values)
                elif keyword == "mtllib":
                    reference = stripped[len(fields[0]) :].strip()
                    if reference:
                        material_libraries.append(reference)
    except UnicodeDecodeError:
        raise TripoError("The generated OBJ is not valid UTF-8 text.") from None
    except OSError:
        raise TripoError("The generated OBJ could not be read.") from None
    if not positions or not texcoords:
        raise TripoError("The generated OBJ does not contain textured geometry.")
    if not material_libraries:
        raise TripoError("The generated OBJ is missing its material library.")
    return positions, texcoords, material_libraries


def _read_material_textures(obj_path: Path, package_dir: Path, references: list[str]) -> dict[str, Path]:
    textures: dict[str, Path] = {}
    for reference in references:
        material_path = _obj_dependency_path(package_dir, obj_path.parent, reference, "material")
        current_material = ""
        try:
            with material_path.open("r", encoding="utf-8", errors="strict") as stream:
                for line in stream:
                    stripped = line.strip()
                    if not stripped or stripped.startswith("#"):
                        continue
                    fields = stripped.split(maxsplit=1)
                    keyword = fields[0].lower()
                    value = fields[1].strip() if len(fields) > 1 else ""
                    if keyword == "newmtl":
                        current_material = value
                    elif keyword == "map_kd" and current_material:
                        # Tripo emits a plain filename. Taking the final token also
                        # tolerates standard map_Kd options such as -s or -o.
                        texture_reference = value.strip().strip('"')
                        direct = material_path.parent / texture_reference
                        if not direct.is_file():
                            texture_reference = value.split()[-1].strip('"') if value.split() else ""
                        textures[current_material] = _obj_dependency_path(
                            package_dir, material_path.parent, texture_reference, "base-color texture"
                        )
        except UnicodeDecodeError:
            raise TripoError("The generated material library is not valid UTF-8 text.") from None
        except OSError:
            raise TripoError("The generated material library could not be read.") from None
    if not textures:
        raise TripoError("The generated OBJ is missing its base-color texture.")
    return textures


def _resolve_obj_index(value: str, count: int, kind: str) -> int:
    try:
        index = int(value)
    except ValueError:
        raise TripoError(f"The generated OBJ has an invalid {kind} index.") from None
    if index == 0:
        raise TripoError(f"The generated OBJ has an invalid {kind} index.")
    resolved = index - 1 if index > 0 else count + index
    if resolved < 0 or resolved >= count:
        raise TripoError(f"The generated OBJ references a missing {kind}.")
    return resolved


def _srgb_to_lab(color: tuple[int, int, int]) -> tuple[float, float, float]:
    linear = []
    for channel in color:
        value = channel / 255.0
        linear.append(value / 12.92 if value <= 0.04045 else ((value + 0.055) / 1.055) ** 2.4)
    red, green, blue = linear
    x = (0.4124564 * red + 0.3575761 * green + 0.1804375 * blue) / 0.95047
    y = 0.2126729 * red + 0.7151522 * green + 0.0721750 * blue
    z = (0.0193339 * red + 0.1191920 * green + 0.9503041 * blue) / 1.08883

    def transform(value: float) -> float:
        return value ** (1.0 / 3.0) if value > 0.008856 else 7.787 * value + 16.0 / 116.0

    fx, fy, fz = transform(x), transform(y), transform(z)
    return 116.0 * fy - 16.0, 500.0 * (fx - fy), 200.0 * (fy - fz)


def _palette_data(palette: tuple[str, ...]) -> tuple[list[tuple[int, int, int]], list[tuple[float, float, float]]]:
    colors = [tuple(int(color[index : index + 2], 16) for index in (1, 3, 5)) for color in palette]
    return colors, [_srgb_to_lab(color) for color in colors]


def _vertex_color_data(
    palette: tuple[str, ...],
) -> tuple[list[tuple[int, int, int]], list[tuple[float, float, float]]]:
    return _palette_data(palette) if palette else ([], [])


def _nearest_palette_index(
    color: tuple[int, int, int],
    palette_lab: list[tuple[float, float, float]],
    cache: dict[tuple[int, int, int], int],
) -> int:
    cached = cache.get(color)
    if cached is not None:
        return cached
    lab = _srgb_to_lab(color)
    index = min(
        range(len(palette_lab)),
        key=lambda item: sum((lab[channel] - palette_lab[item][channel]) ** 2 for channel in range(3)),
    )
    cache[color] = index
    return index


def _bake_obj_texture_to_vertex_colors(
    obj_path: Path,
    package_dir: Path,
    destination: Path,
    palette: tuple[str, ...],
) -> None:
    try:
        from PIL import Image, UnidentifiedImageError
    except ImportError:
        raise TripoError("Pillow is required to convert OBJ textures into printable vertex colors.") from None

    positions, texcoords, material_references = _read_obj_geometry(obj_path)
    texture_paths = _read_material_textures(obj_path, package_dir, material_references)
    palette_rgb, palette_lab = _vertex_color_data(palette)
    nearest_cache: dict[tuple[int, int, int], int] = {}
    images: dict[str, Any] = {}
    try:
        for material, texture_path in texture_paths.items():
            try:
                image = Image.open(texture_path)
                if image.width <= 0 or image.height <= 0 or image.width * image.height > MAX_TEXTURE_PIXELS:
                    image.close()
                    raise TripoError("The generated base-color texture has an invalid size.")
                image.load()
                images[material] = image.convert("RGB")
                image.close()
            except (OSError, UnidentifiedImageError, Image.DecompressionBombError):
                raise TripoError("The generated base-color texture could not be decoded.") from None

        face_path = destination.with_suffix(".faces.tmp")
        output_sources = array("I")
        output_color_counts: list[list[int]] = []
        output_color_sums = array("Q")
        output_sample_counts = array("I")
        output_vertices: dict[int, int] = {}
        face_vertices = array("I")
        face_count = 0
        current_material = ""
        try:
            source_stream = obj_path.open("r", encoding="utf-8", errors="strict")
            face_stream = face_path.open("w", encoding="ascii", newline="\n")
        except OSError:
            raise TripoError("The generated OBJ could not be converted.") from None
        with source_stream, face_stream:
            for line in source_stream:
                stripped = line.strip()
                if not stripped or stripped.startswith("#"):
                    continue
                fields = stripped.split()
                keyword = fields[0].lower()
                if keyword == "usemtl":
                    current_material = stripped[len(fields[0]) :].strip()
                    safe_material = re.sub(r"[^A-Za-z0-9_.-]+", "_", current_material).strip("_") or "material"
                    face_stream.write("g material_" + safe_material + "\n")
                    continue
                if keyword in {"o", "g"}:
                    name = stripped[len(fields[0]) :].strip()
                    safe_name = re.sub(r"[^A-Za-z0-9_.-]+", "_", name).strip("_") or keyword
                    face_stream.write(keyword + " " + safe_name + "\n")
                    continue
                if keyword != "f":
                    continue
                if len(fields) != 4:
                    raise TripoError("The generated OBJ must contain only triangular faces.")
                material = current_material
                if not material and len(images) == 1:
                    material = next(iter(images))
                image = images.get(material)
                if image is None:
                    raise TripoError("The generated OBJ face is missing a base-color material.")
                pixels = image.load()
                face_indices: list[str] = []
                for field in fields[1:]:
                    indices = field.split("/")
                    if len(indices) < 2 or not indices[0] or not indices[1]:
                        raise TripoError("The generated OBJ face is missing texture coordinates.")
                    vertex_index = _resolve_obj_index(indices[0], len(positions) // 3, "vertex")
                    texcoord_index = _resolve_obj_index(indices[1], len(texcoords) // 2, "texture coordinate")
                    output_index = output_vertices.get(vertex_index)
                    if output_index is None:
                        output_sources.append(vertex_index)
                        output_index = len(output_sources)
                        output_vertices[vertex_index] = output_index
                        output_color_counts.append([0] * len(palette_rgb))
                        output_color_sums.extend((0, 0, 0))
                        output_sample_counts.append(0)
                    u = max(0.0, min(1.0, texcoords[texcoord_index * 2]))
                    v = max(0.0, min(1.0, texcoords[texcoord_index * 2 + 1]))
                    x = min(image.width - 1, max(0, round(u * (image.width - 1))))
                    y = min(image.height - 1, max(0, round((1.0 - v) * (image.height - 1))))
                    sampled = tuple(pixels[x, y])
                    if palette_rgb:
                        palette_index = _nearest_palette_index(sampled, palette_lab, nearest_cache)
                        output_color_counts[output_index - 1][palette_index] += 1
                    else:
                        color_offset = (output_index - 1) * 3
                        for channel in range(3):
                            output_color_sums[color_offset + channel] += sampled[channel]
                        output_sample_counts[output_index - 1] += 1
                    face_indices.append(str(output_index))
                    face_vertices.append(output_index - 1)
                face_stream.write("f " + " ".join(face_indices) + "\n")
                face_count += 1

        if not output_sources or not face_count:
            raise TripoError("The generated OBJ does not contain textured faces.")
        output_palette_indices: list[int] = []
        if palette_rgb:
            output_palette_indices = [
                max(range(len(counts)), key=lambda item: (counts[item], -item))
                for counts in output_color_counts
            ]
            # Orca intentionally uses two-color triangles to encode an MMU boundary. Three-color triangles are much harder to
            # print predictably. Relabel the highest palette index in each offending face to one of its two lower labels. Every
            # edit is monotonic, so adjacent faces cannot oscillate forever; shared vertices and watertight topology stay intact.
            vertex_faces: list[list[int]] = [[] for _ in output_palette_indices]
            pending: deque[int] = deque()
            queued = bytearray(face_count)
            for face_index, offset in enumerate(range(0, len(face_vertices), 3)):
                vertices = face_vertices[offset:offset + 3]
                for vertex in vertices:
                    vertex_faces[vertex].append(face_index)
                if len({output_palette_indices[index] for index in vertices}) == 3:
                    pending.append(face_index)
                    queued[face_index] = 1
            changes = 0
            max_changes = len(output_palette_indices) * max(1, len(palette_rgb) - 1)
            while pending:
                face_index = pending.popleft()
                queued[face_index] = 0
                offset = face_index * 3
                vertices = face_vertices[offset:offset + 3]
                labels = [output_palette_indices[index] for index in vertices]
                if len(set(labels)) < 3:
                    continue
                current = max(labels)
                corner = labels.index(current)
                vertex = vertices[corner]
                counts = output_color_counts[vertex]
                targets = [label for label in labels if label < current]
                target = max(targets, key=lambda label: (counts[label], -label))
                output_palette_indices[vertex] = target
                changes += 1
                if changes > max_changes:
                    raise TripoError("The printable vertex-color pass did not converge.")
                for adjacent_face in vertex_faces[vertex]:
                    if not queued[adjacent_face]:
                        pending.append(adjacent_face)
                        queued[adjacent_face] = 1
            remaining_three_color_faces = sum(
                len({output_palette_indices[index] for index in face_vertices[offset:offset + 3]}) == 3
                for offset in range(0, len(face_vertices), 3)
            )
            if remaining_three_color_faces:
                raise TripoError(
                    f"The printable vertex-color pass left {remaining_three_color_faces} three-color triangles."
                )
        try:
            with destination.open("w", encoding="ascii", newline="\n") as output:
                output.write("# OrcaSlicer AI vertex-color OBJ\n")
                output.write(f"# Source package: {obj_path.name}\n")
                for output_index, source_index in enumerate(output_sources):
                    offset = source_index * 3
                    if palette_rgb:
                        palette_index = output_palette_indices[output_index]
                        red, green, blue = palette_rgb[palette_index]
                    else:
                        samples = max(1, output_sample_counts[output_index])
                        color_offset = output_index * 3
                        red, green, blue = (
                            round(output_color_sums[color_offset + channel] / samples) for channel in range(3)
                        )
                    output.write(
                        "v {:.9g} {:.9g} {:.9g} {:.6f} {:.6f} {:.6f}\n".format(
                            positions[offset], positions[offset + 1], positions[offset + 2],
                            red / 255.0,
                            green / 255.0,
                            blue / 255.0,
                        )
                    )
                with face_path.open("r", encoding="ascii") as faces:
                    shutil.copyfileobj(faces, output, length=1024 * 1024)
        except OSError:
            raise TripoError("The vertex-color OBJ could not be saved.") from None
        finally:
            try:
                face_path.unlink(missing_ok=True)
            except OSError:
                pass
    finally:
        for image in images.values():
            image.close()


def _quantize_vertex_color_obj(source: Path, destination: Path, palette: tuple[str, ...]) -> None:
    if not palette:
        try:
            shutil.copyfile(source, destination)
        except OSError:
            raise TripoError("The generated OBJ could not be copied.") from None
        return
    palette_rgb, palette_lab = _vertex_color_data(palette)
    nearest_cache: dict[tuple[int, int, int], int] = {}
    try:
        with source.open("r", encoding="utf-8", errors="strict") as input_stream, destination.open(
            "w", encoding="ascii", newline="\n"
        ) as output:
            output.write(
                "# OrcaSlicer AI palette-constrained vertex-color OBJ\n"
                if palette
                else "# OrcaSlicer AI natural vertex-color OBJ\n"
            )
            for line in input_stream:
                fields = line.strip().split()
                if not fields or fields[0].startswith("#"):
                    continue
                keyword = fields[0].lower()
                if keyword == "v":
                    if len(fields) not in {7, 8}:
                        raise TripoError("The generated OBJ does not provide valid vertex colors.")
                    try:
                        values = [float(value) for value in fields[1:7]]
                    except ValueError:
                        raise TripoError("The generated OBJ has an invalid vertex.") from None
                    sampled = tuple(round(max(0.0, min(1.0, value)) * 255) for value in values[3:6])
                    red, green, blue = palette_rgb[_nearest_palette_index(sampled, palette_lab, nearest_cache)]
                    output.write(
                        "v {:.9g} {:.9g} {:.9g} {:.6f} {:.6f} {:.6f}\n".format(
                            values[0], values[1], values[2], red / 255.0, green / 255.0, blue / 255.0
                        )
                    )
                elif keyword == "f":
                    output.write("f " + " ".join(field.split("/", 1)[0] for field in fields[1:]) + "\n")
                elif keyword in {"o", "g"}:
                    output.write(" ".join(fields) + "\n")
    except UnicodeDecodeError:
        raise TripoError("The generated OBJ is not valid UTF-8 text.") from None
    except OSError:
        raise TripoError("The generated OBJ could not be color constrained.") from None


def _normalize_obj_for_orca(path: Path, target_size_mm: float = DEFAULT_MODEL_SIZE_MM) -> None:
    minimum = [math.inf, math.inf, math.inf]
    maximum = [-math.inf, -math.inf, -math.inf]
    try:
        with path.open("r", encoding="utf-8", errors="strict") as stream:
            for line in stream:
                fields = line.strip().split()
                if not fields or fields[0].lower() != "v" or len(fields) not in {7, 8}:
                    continue
                values = [float(value) for value in fields[1:4]]
                for axis, value in enumerate(values):
                    minimum[axis] = min(minimum[axis], value)
                    maximum[axis] = max(maximum[axis], value)
    except (OSError, UnicodeDecodeError, ValueError):
        raise TripoError("The generated OBJ could not be normalized for OrcaSlicer.") from None
    spans = [maximum[axis] - minimum[axis] for axis in range(3)]
    largest_span = max(spans)
    if not math.isfinite(largest_span) or largest_span <= 1e-9 or target_size_mm <= 0:
        raise TripoError("The generated OBJ has invalid dimensions.")

    scale = target_size_mm / largest_span
    center_x = (minimum[0] + maximum[0]) * 0.5
    center_z = (minimum[2] + maximum[2]) * 0.5
    temporary = path.with_name(path.name + ".normalized")
    try:
        with path.open("r", encoding="utf-8", errors="strict") as source, temporary.open(
            "w", encoding="ascii", newline="\n"
        ) as output:
            output.write("# OrcaSlicer AI normalized: Z-up, centered, on-bed, 100 mm maximum dimension\n")
            for line in source:
                fields = line.strip().split()
                if fields and fields[0].lower() == "v" and len(fields) in {7, 8}:
                    values = [float(value) for value in fields[1:]]
                    x = (values[0] - center_x) * scale
                    y = -(values[2] - center_z) * scale
                    z = (values[1] - minimum[1]) * scale
                    output.write(
                        "v {:.9g} {:.9g} {:.9g} {}\n".format(
                            x, y, z, " ".join("{:.6f}".format(value) for value in values[3:])
                        )
                    )
                elif fields and fields[0].lower() == "f":
                    output.write("f " + " ".join(fields[1:]) + "\n")
                elif fields and fields[0].lower() in {"o", "g"}:
                    output.write(" ".join(fields) + "\n")
        os.replace(temporary, path)
    except (OSError, UnicodeDecodeError, ValueError):
        raise TripoError("The generated OBJ could not be normalized for OrcaSlicer.") from None
    finally:
        try:
            temporary.unlink(missing_ok=True)
        except OSError:
            pass


def _prepare_obj_artifact(raw_download: Path, job_directory: Path, palette: tuple[str, ...]) -> Path:
    try:
        with raw_download.open("rb") as stream:
            signature = stream.read(4)
    except OSError:
        raise TripoError("The generated artifact could not be read.") from None
    destination = job_directory / "model-vertex-color.obj"
    if signature.startswith(b"PK\x03\x04"):
        archive = job_directory / "artifact-raw.zip"
        raw_download.replace(archive)
        obj_path = _extract_obj_package(archive, job_directory / "package")
        _bake_obj_texture_to_vertex_colors(obj_path, job_directory / "package", destination, palette)
    else:
        raw_obj = job_directory / "artifact-raw.obj"
        raw_download.replace(raw_obj)
        _validate_obj_vertex_colors(raw_obj)
        _quantize_vertex_color_obj(raw_obj, destination, palette)
    _normalize_obj_for_orca(destination)
    repair_report = _remove_small_detached_obj_components(destination, job_directory / "mesh-repair.json")
    _repair_small_obj_topology_defects(destination, job_directory / "mesh-repair.json", repair_report)
    if palette:
        _validate_obj_palette(destination, palette)
    else:
        _validate_obj_vertex_colors(destination)
    _write_obj_vertex_color_metrics(destination, job_directory / "vertex-color-metrics.json")
    _validate_artifact(destination, "obj", allow_repairable_obj=True)
    quality = analyze_printable_obj(destination, allow_repairable_topology=True)
    try:
        write_model_quality_report(quality, job_directory / MODEL_QUALITY_FILENAME)
    except ModelQualityError as exc:
        raise TripoError(str(exc)) from None
    if quality.get("status") == "reject":
        errors = ", ".join(str(code) for code in quality.get("errors", [])) or "unknown structural error"
        raise TripoError(f"The generated OBJ failed the structural quality gate: {errors}.")
    return destination


def _persist_attempts(job: Job) -> None:
    temporary = job.directory / "attempts.json.part"
    destination = job.directory / "attempts.json"
    try:
        temporary.write_text(json.dumps({"attempts": job.attempts}, ensure_ascii=False, indent=2), encoding="utf-8")
        os.replace(temporary, destination)
    except OSError:
        try:
            temporary.unlink(missing_ok=True)
        except OSError:
            pass


def _record_attempt(job: Job, attempt_number: int, **updates: Any) -> None:
    with _JOBS_LOCK:
        while len(job.attempts) < attempt_number:
            job.attempts.append({"attempt": len(job.attempts) + 1})
        job.attempts[attempt_number - 1].update(updates)
        _persist_attempts(job)
        _persist_job(job)


def _download_conversion(
    job: Job, generation_id: str, format_name: str, attempt_number: int = 1, resume: bool = False
) -> Path:
    with _JOBS_LOCK:
        job.state = "running"
        job.phase = "converting"
        job.message = f"Converting generated geometry to {format_name.upper()}."
        job.progress = 75
        _persist_job(job)
    existing = job.attempts[attempt_number - 1] if resume and len(job.attempts) >= attempt_number else {}
    conversion_id = existing.get("conversion_task_id", "")
    if not isinstance(conversion_id, str) or not conversion_id:
        conversion_id = create_conversion(generation_id, format_name)
        _record_attempt(job, attempt_number, conversion_task_id=conversion_id)
    attempt_directory = job.directory / f"attempt-{attempt_number:02d}"
    attempt_directory.mkdir(parents=False, exist_ok=True)
    if resume and format_name == "obj":
        candidates = sorted(attempt_directory.rglob("model-vertex-color.obj"), reverse=True)
        for candidate in candidates:
            try:
                candidate.resolve().relative_to(attempt_directory.resolve())
                _validate_artifact(candidate, "obj", allow_repairable_obj=True)
            except (OSError, TripoError, ValueError):
                continue
            return candidate
    _stop_boundary(job)
    result = wait_for_task(
        conversion_id,
        stop_event=job.stop_event,
        progress=_progress_callback(job, 75, 95),
    )
    _stop_boundary(job)
    with _JOBS_LOCK:
        job.phase = "downloading_artifact"
        job.message = "Preparing the generated artifact."
        job.progress = 95
        _persist_job(job)
    work_directory = attempt_directory
    if resume:
        recovery_number = 1
        while (attempt_directory / f"recovery-{recovery_number:02d}").exists():
            recovery_number += 1
        work_directory = attempt_directory / f"recovery-{recovery_number:02d}"
        work_directory.mkdir(parents=False, exist_ok=False)
    destination = work_directory / "artifact-raw.download"
    download_task_artifact(result, destination, MAX_ARTIFACT_BYTES)
    _stop_boundary(job)
    if format_name == "obj":
        return _prepare_obj_artifact(destination, work_directory, job.palette)
    _validate_artifact(destination, format_name)
    return destination


def _validate_obj_vertex_colors(path: Path) -> None:
    vertices: list[bool] = []
    referenced_vertices: set[int] = set()
    try:
        with path.open("r", encoding="utf-8", errors="strict") as stream:
            for line in stream:
                stripped = line.strip()
                if not stripped or stripped.startswith("#"):
                    continue
                fields = stripped.split()
                keyword = fields[0].lower()
                if keyword in {"mtllib", "usemtl", "vt", "vn", "map_kd"}:
                    raise TripoError("The generated OBJ depends on external materials or textures.")
                if keyword == "v":
                    if len(fields) not in {7, 8}:
                        vertices.append(False)
                        continue
                    try:
                        values = [float(value) for value in fields[1:]]
                    except ValueError:
                        vertices.append(False)
                        continue
                    if not all(math.isfinite(value) for value in values):
                        vertices.append(False)
                        continue
                    colors = values[3:]
                    vertices.append(all(0.0 <= value <= 1.0 for value in colors))
                elif keyword == "f":
                    if len(fields) < 4:
                        raise TripoError("The generated OBJ has an invalid face.")
                    for field in fields[1:]:
                        if "/" in field:
                            raise TripoError("The generated OBJ contains unsupported texture or normal references.")
                        try:
                            index = int(field)
                        except ValueError:
                            raise TripoError("The generated OBJ has an invalid vertex index.") from None
                        if index == 0:
                            raise TripoError("The generated OBJ has an invalid vertex index.")
                        resolved = index - 1 if index > 0 else len(vertices) + index
                        if resolved < 0 or resolved >= len(vertices):
                            raise TripoError("The generated OBJ references a missing vertex.")
                        referenced_vertices.add(resolved)
    except UnicodeDecodeError:
        raise TripoError("The generated OBJ is not valid UTF-8 text.") from None
    except OSError:
        raise TripoError("The generated OBJ could not be read.") from None
    if not referenced_vertices or any(not vertices[index] for index in referenced_vertices):
        raise TripoError("The generated OBJ does not provide valid vertex colors.")


def _validate_obj_palette(path: Path, palette: tuple[str, ...]) -> None:
    allowed = set(_palette_data(palette)[0])
    found = False
    try:
        with path.open("r", encoding="utf-8", errors="strict") as stream:
            for line in stream:
                fields = line.strip().split()
                if not fields or fields[0].lower() != "v":
                    continue
                if len(fields) not in {7, 8}:
                    raise TripoError("The generated OBJ does not provide valid vertex colors.")
                try:
                    color = tuple(round(float(value) * 255) for value in fields[4:7])
                except ValueError:
                    raise TripoError("The generated OBJ has an invalid vertex color.") from None
                if color not in allowed:
                    raise TripoError("The generated OBJ contains colors outside the printable filament palette.")
                found = True
    except UnicodeDecodeError:
        raise TripoError("The generated OBJ is not valid UTF-8 text.") from None
    except OSError:
        raise TripoError("The generated OBJ could not be read.") from None
    if not found:
        raise TripoError("The generated OBJ does not provide valid vertex colors.")


def _obj_vertex_color_metrics(path: Path) -> dict[str, Any]:
    colors: list[tuple[int, int, int]] = []
    faces: list[tuple[int, int, int]] = []
    try:
        with path.open("r", encoding="utf-8", errors="strict") as stream:
            for line in stream:
                fields = line.strip().split()
                if not fields or fields[0].startswith("#"):
                    continue
                if fields[0].lower() == "v":
                    if len(fields) not in {7, 8}:
                        raise TripoError("The generated OBJ does not provide valid vertex colors.")
                    colors.append(tuple(round(float(value) * 255) for value in fields[4:7]))
                elif fields[0].lower() == "f":
                    if len(fields) != 4:
                        raise TripoError("The generated OBJ must contain only triangular faces.")
                    faces.append(tuple(_resolve_obj_index(value, len(colors), "vertex") for value in fields[1:]))
    except (OSError, UnicodeDecodeError, ValueError):
        raise TripoError("The generated OBJ color metrics could not be calculated.") from None
    distribution = Counter(len({colors[index] for index in face}) for face in faces)
    vertex_usage = Counter(colors)
    total = max(1, len(faces))
    return {
        "vertex_count": len(colors),
        "face_count": len(faces),
        "vertex_color_count": len(vertex_usage),
        "uniform_faces": distribution[1],
        "two_color_faces": distribution[2],
        "three_color_faces": distribution[3],
        "two_color_face_ratio": round(distribution[2] / total, 6),
        "three_color_face_ratio": round(distribution[3] / total, 6),
        "vertex_color_usage": {
            "#{:02X}{:02X}{:02X}".format(*color): count for color, count in sorted(vertex_usage.items())
        },
    }


def _write_obj_vertex_color_metrics(path: Path, report_path: Path) -> dict[str, Any]:
    metrics = _obj_vertex_color_metrics(path)
    temporary = report_path.with_suffix(report_path.suffix + ".part")
    try:
        temporary.write_text(json.dumps(metrics, ensure_ascii=False, indent=2), encoding="utf-8")
        os.replace(temporary, report_path)
    except OSError:
        try:
            temporary.unlink(missing_ok=True)
        except OSError:
            pass
        raise TripoError("The generated OBJ color metrics could not be saved.") from None
    return metrics


def _write_mesh_repair_report(path: Path, report: dict[str, Any]) -> None:
    temporary = path.with_suffix(path.suffix + ".part")
    try:
        temporary.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
        os.replace(temporary, path)
    except OSError:
        try:
            temporary.unlink(missing_ok=True)
        except OSError:
            pass


def _remove_small_detached_obj_components(path: Path, report_path: Path) -> dict[str, Any]:
    vertex_lines: list[str] = []
    vertices: list[tuple[float, float, float]] = []
    faces: list[tuple[int, int, int]] = []
    try:
        with path.open("r", encoding="utf-8", errors="strict") as stream:
            for line in stream:
                fields = line.strip().split()
                if not fields or fields[0].startswith("#"):
                    continue
                keyword = fields[0].lower()
                if keyword == "v":
                    if len(fields) not in {7, 8}:
                        raise TripoError("The generated OBJ does not provide valid vertex colors.")
                    try:
                        position = tuple(float(value) for value in fields[1:4])
                    except ValueError:
                        raise TripoError("The generated OBJ has an invalid vertex position.") from None
                    if not all(math.isfinite(value) for value in position):
                        raise TripoError("The generated OBJ has an invalid vertex position.")
                    vertices.append(position)
                    vertex_lines.append(" ".join(fields))
                elif keyword == "f":
                    if len(fields) != 4:
                        raise TripoError("The generated OBJ must contain only triangular faces.")
                    face = tuple(_resolve_obj_index(value, len(vertices), "vertex") for value in fields[1:])
                    if len(set(face)) != 3:
                        raise TripoError("The generated OBJ contains a degenerate triangle.")
                    faces.append(face)
    except UnicodeDecodeError:
        raise TripoError("The generated OBJ is not valid UTF-8 text.") from None
    except OSError:
        raise TripoError("The generated OBJ could not be read.") from None

    if not vertices or not faces:
        raise TripoError("The generated OBJ does not contain usable geometry.")

    parent = list(range(len(vertices)))

    def find(index: int) -> int:
        while parent[index] != index:
            parent[index] = parent[parent[index]]
            index = parent[index]
        return index

    def unite(left: int, right: int) -> None:
        left_root, right_root = find(left), find(right)
        if left_root != right_root:
            parent[right_root] = left_root

    for face in faces:
        unite(face[0], face[1])
        unite(face[1], face[2])

    component_faces: dict[int, list[tuple[int, int, int]]] = {}
    component_vertices: dict[int, set[int]] = {}
    for face in faces:
        root = find(face[0])
        component_faces.setdefault(root, []).append(face)
        component_vertices.setdefault(root, set()).update(face)

    def diagonal(indices: set[int]) -> float:
        minimum = [min(vertices[index][axis] for index in indices) for axis in range(3)]
        maximum = [max(vertices[index][axis] for index in indices) for axis in range(3)]
        return math.sqrt(sum((maximum[axis] - minimum[axis]) ** 2 for axis in range(3)))

    components = sorted(
        component_faces,
        key=lambda root: (len(component_faces[root]), len(component_vertices[root])),
        reverse=True,
    )
    main = components[0]
    main_face_count = len(component_faces[main])
    report: dict[str, Any] = {
        "status": "not_needed" if len(components) == 1 else "preserved",
        "original_components": len(components),
        "kept_vertices": sum(len(indices) for indices in component_vertices.values()),
        "kept_faces": sum(len(items) for items in component_faces.values()),
        "removed_components": 0,
        "removed_vertices": 0,
        "removed_faces": 0,
        "largest_component_faces": main_face_count,
        "largest_component_diagonal": diagonal(component_vertices[main]),
    }

    _write_mesh_repair_report(report_path, report)
    return report


def _repair_small_obj_topology_defects(
    path: Path, report_path: Path, report: dict[str, Any] | None = None
) -> dict[str, Any]:
    report = dict(report or {})
    vertex_lines: list[str] = []
    positions: list[tuple[float, float, float]] = []
    colors: list[tuple[str, ...]] = []
    faces: list[tuple[int, int, int]] = []
    try:
        with path.open("r", encoding="utf-8", errors="strict") as stream:
            for line in stream:
                fields = line.strip().split()
                if not fields or fields[0].startswith("#"):
                    continue
                keyword = fields[0].lower()
                if keyword == "v":
                    if len(fields) not in {7, 8}:
                        raise TripoError("The generated OBJ does not provide valid vertex colors.")
                    try:
                        position = tuple(float(value) for value in fields[1:4])
                    except ValueError:
                        raise TripoError("The generated OBJ has an invalid vertex position.") from None
                    if not all(math.isfinite(value) for value in position):
                        raise TripoError("The generated OBJ has an invalid vertex position.")
                    vertex_lines.append(" ".join(fields))
                    positions.append(position)
                    colors.append(tuple(fields[4:]))
                elif keyword == "f":
                    if len(fields) != 4:
                        raise TripoError("The generated OBJ must contain only triangular faces.")
                    face = tuple(_resolve_obj_index(value, len(positions), "vertex") for value in fields[1:])
                    if len(set(face)) != 3:
                        raise TripoError("The generated OBJ contains a degenerate triangle.")
                    faces.append(face)
    except UnicodeDecodeError:
        raise TripoError("The generated OBJ is not valid UTF-8 text.") from None
    except OSError:
        raise TripoError("The generated OBJ could not be read.") from None

    def edge_usage(source_faces: list[tuple[int, int, int]]) -> dict[tuple[int, int], list[tuple[int, int, int]]]:
        usage: dict[tuple[int, int], list[tuple[int, int, int]]] = {}
        for face_index, face in enumerate(source_faces):
            for left, right in ((face[0], face[1]), (face[1], face[2]), (face[2], face[0])):
                edge = (left, right) if left < right else (right, left)
                usage.setdefault(edge, []).append((face_index, left, right))
        return usage

    original_usage = edge_usage(faces)
    original_boundary = sum(len(uses) == 1 for uses in original_usage.values())
    original_non_manifold = sum(len(uses) > 2 for uses in original_usage.values())
    original_inconsistent_winding = sum(
        len(uses) == 2 and uses[0][1:] == uses[1][1:]
        for uses in original_usage.values()
    )
    report.update(
        topology_status=(
            "not_needed"
            if not original_boundary and not original_non_manifold and not original_inconsistent_winding
            else "deferred"
        ),
        original_boundary_edges=original_boundary,
        original_non_manifold_edges=original_non_manifold,
        original_inconsistent_winding_edges=original_inconsistent_winding,
        removed_non_manifold_faces=0,
        flipped_winding_faces=0,
        removed_topology_vertices=0,
        filled_boundary_loops=0,
        added_vertices=0,
        added_faces=0,
        remaining_invalid_edges=original_boundary + original_non_manifold + original_inconsistent_winding,
    )
    if not original_boundary and not original_non_manifold and not original_inconsistent_winding:
        _write_mesh_repair_report(report_path, report)
        return report

    mesh_minimum = [min(position[axis] for position in positions) for axis in range(3)]
    mesh_maximum = [max(position[axis] for position in positions) for axis in range(3)]
    mesh_diagonal = math.sqrt(sum((mesh_maximum[axis] - mesh_minimum[axis]) ** 2 for axis in range(3)))
    if mesh_diagonal <= 0.0:
        _write_mesh_repair_report(report_path, report)
        return report

    working_faces = list(faces)
    removed_face_count = 0
    if original_non_manifold:
        remove_indices = {
            face_index
            for uses in original_usage.values()
            if len(uses) > 2
            for face_index, _left, _right in uses
        }
        max_removed_faces = max(64, int(len(faces) * MAX_LOCAL_REPAIR_FACE_RATIO))
        if len(remove_indices) > max_removed_faces:
            report["topology_deferred_reason"] = "too_many_non_manifold_faces"
            _write_mesh_repair_report(report_path, report)
            return report

        vertex_faces: dict[int, list[int]] = {}
        for face_index in remove_indices:
            for vertex in faces[face_index]:
                vertex_faces.setdefault(vertex, []).append(face_index)
        remaining_faces = set(remove_indices)
        defect_regions: list[set[int]] = []
        while remaining_faces:
            pending = [min(remaining_faces)]
            remaining_faces.remove(pending[0])
            region: set[int] = set()
            while pending:
                face_index = pending.pop()
                region.add(face_index)
                for vertex in faces[face_index]:
                    for adjacent in vertex_faces[vertex]:
                        if adjacent in remaining_faces:
                            remaining_faces.remove(adjacent)
                            pending.append(adjacent)
            defect_regions.append(region)

        max_region_diagonal_ratio = 0.0
        for region in defect_regions:
            defect_vertices = {vertex for face_index in region for vertex in faces[face_index]}
            defect_minimum = [min(positions[index][axis] for index in defect_vertices) for axis in range(3)]
            defect_maximum = [max(positions[index][axis] for index in defect_vertices) for axis in range(3)]
            defect_diagonal = math.sqrt(
                sum((defect_maximum[axis] - defect_minimum[axis]) ** 2 for axis in range(3))
            )
            max_region_diagonal_ratio = max(max_region_diagonal_ratio, defect_diagonal / mesh_diagonal)
        report.update(
            non_manifold_regions=len(defect_regions),
            max_non_manifold_region_diagonal_ratio=max_region_diagonal_ratio,
        )
        if max_region_diagonal_ratio > MAX_LOCAL_REPAIR_DIAGONAL_RATIO:
            report["topology_deferred_reason"] = "non_manifold_region_too_large"
            _write_mesh_repair_report(report_path, report)
            return report
        working_faces = [face for index, face in enumerate(faces) if index not in remove_indices]
        removed_face_count = len(remove_indices)

    def normalize_face_winding(
        source_faces: list[tuple[int, int, int]],
    ) -> tuple[list[tuple[int, int, int]] | None, int, int]:
        usage = edge_usage(source_faces)
        adjacency: list[list[tuple[int, bool]]] = [[] for _ in source_faces]
        inconsistent_before = 0
        for uses in usage.values():
            if len(uses) != 2:
                continue
            left_face, left_from, left_to = uses[0]
            right_face, right_from, right_to = uses[1]
            same_direction = left_from == right_from and left_to == right_to
            inconsistent_before += int(same_direction)
            adjacency[left_face].append((right_face, same_direction))
            adjacency[right_face].append((left_face, same_direction))

        flips: list[bool | None] = [None] * len(source_faces)
        for start in range(len(source_faces)):
            if flips[start] is not None:
                continue
            flips[start] = False
            pending = [start]
            component: list[int] = []
            while pending:
                face_index = pending.pop()
                component.append(face_index)
                for adjacent, must_differ in adjacency[face_index]:
                    expected = bool(flips[face_index]) ^ must_differ
                    if flips[adjacent] is None:
                        flips[adjacent] = expected
                        pending.append(adjacent)
                    elif flips[adjacent] != expected:
                        return None, 0, inconsistent_before
            if sum(bool(flips[index]) for index in component) > len(component) // 2:
                for index in component:
                    flips[index] = not bool(flips[index])

        oriented_faces = [
            (face[0], face[2], face[1]) if flips[index] else face
            for index, face in enumerate(source_faces)
        ]
        oriented_usage = edge_usage(oriented_faces)
        inconsistent_after = sum(
            len(uses) == 2 and uses[0][1:] == uses[1][1:]
            for uses in oriented_usage.values()
        )
        return oriented_faces, sum(bool(value) for value in flips), inconsistent_after

    oriented_faces, flipped_face_count, remaining_inconsistent_winding = normalize_face_winding(working_faces)
    report.update(
        flipped_winding_faces=flipped_face_count,
        remaining_inconsistent_winding_edges=remaining_inconsistent_winding,
    )
    if oriented_faces is None or remaining_inconsistent_winding:
        report["topology_deferred_reason"] = "non_orientable_face_winding"
        _write_mesh_repair_report(report_path, report)
        return report
    working_faces = oriented_faces

    usage = edge_usage(working_faces)
    if any(len(uses) > 2 for uses in usage.values()):
        _write_mesh_repair_report(report_path, report)
        return report
    boundary = {edge: uses[0][1:] for edge, uses in usage.items() if len(uses) == 1}

    outgoing: dict[int, list[tuple[int, tuple[int, int]]]] = {}
    incoming_count: Counter[int] = Counter()
    outgoing_count: Counter[int] = Counter()
    for edge, (left, right) in boundary.items():
        outgoing.setdefault(left, []).append((right, edge))
        outgoing_count[left] += 1
        incoming_count[right] += 1
    boundary_vertices = set(incoming_count) | set(outgoing_count)
    if any(
        incoming_count[index] != outgoing_count[index] or incoming_count[index] not in {1, 2}
        for index in boundary_vertices
    ):
        _write_mesh_repair_report(report_path, report)
        return report
    for entries in outgoing.values():
        entries.sort(reverse=True)

    unused = set(boundary)
    circuits: list[list[int]] = []
    while unused:
        start = min(left for edge, (left, _right) in boundary.items() if edge in unused)
        stack = [start]
        circuit: list[int] = []
        while stack:
            current = stack[-1]
            entries = outgoing.get(current, [])
            while entries and entries[-1][1] not in unused:
                entries.pop()
            if entries:
                right, edge = entries.pop()
                unused.remove(edge)
                stack.append(right)
            else:
                circuit.append(stack.pop())
        circuit.reverse()
        if len(circuit) < 4 or circuit[0] != circuit[-1]:
            _write_mesh_repair_report(report_path, report)
            return report
        circuits.append(circuit)

    cycles: list[list[int]] = []
    for circuit in circuits:
        remainder = circuit
        while len(remainder) > 1:
            seen: dict[int, int] = {}
            for index, vertex in enumerate(remainder):
                if vertex not in seen:
                    seen[vertex] = index
                    continue
                begin = seen[vertex]
                cycle = remainder[begin:index + 1]
                if len(cycle) < 4:
                    _write_mesh_repair_report(report_path, report)
                    return report
                cycles.append(cycle)
                remainder = remainder[:begin + 1] + remainder[index + 1:]
                break
            else:
                _write_mesh_repair_report(report_path, report)
                return report

    if sum(len(cycle) - 1 for cycle in cycles) != len(boundary):
        _write_mesh_repair_report(report_path, report)
        return report
    for cycle in cycles:
        cycle_vertices = cycle[:-1]
        cycle_minimum = [min(positions[index][axis] for index in cycle_vertices) for axis in range(3)]
        cycle_maximum = [max(positions[index][axis] for index in cycle_vertices) for axis in range(3)]
        cycle_diagonal = math.sqrt(
            sum((cycle_maximum[axis] - cycle_minimum[axis]) ** 2 for axis in range(3))
        )
        if len(cycle_vertices) > MAX_LOCAL_BOUNDARY_EDGES or cycle_diagonal > mesh_diagonal * MAX_LOCAL_REPAIR_DIAGONAL_RATIO:
            _write_mesh_repair_report(report_path, report)
            return report

    patched_faces = list(working_faces)
    for cycle in cycles:
        cycle_vertices = cycle[:-1]
        center = tuple(
            sum(positions[index][axis] for index in cycle_vertices) / len(cycle_vertices)
            for axis in range(3)
        )
        color = Counter(colors[index] for index in cycle_vertices).most_common(1)[0][0]
        center_index = len(positions)
        positions.append(center)
        colors.append(color)
        vertex_lines.append(
            "v " + " ".join(f"{value:.9g}" for value in center) + " " + " ".join(color)
        )
        patched_faces.extend(
            (cycle[index + 1], cycle[index], center_index)
            for index in range(len(cycle_vertices))
        )

    final_usage = edge_usage(patched_faces)
    remaining_invalid = sum(len(uses) != 2 for uses in final_usage.values())
    if remaining_invalid or len(patched_faces) > MAX_MODEL_FACES:
        _write_mesh_repair_report(report_path, report)
        return report

    referenced = sorted({vertex for face in patched_faces for vertex in face})
    remap = {old_index: new_index + 1 for new_index, old_index in enumerate(referenced)}
    temporary = path.with_suffix(path.suffix + ".part")
    try:
        with temporary.open("w", encoding="ascii", newline="\n") as output:
            output.write("# OrcaSlicer AI repaired small local mesh defects\n")
            for index in referenced:
                output.write(vertex_lines[index] + "\n")
            for face in patched_faces:
                output.write("f " + " ".join(str(remap[index]) for index in face) + "\n")
        os.replace(temporary, path)
    except OSError:
        try:
            temporary.unlink(missing_ok=True)
        except OSError:
            pass
        raise TripoError("The generated OBJ could not be rewritten after topology repair.") from None

    original_vertex_count = len(vertex_lines) - len(cycles)
    removed_vertex_count = original_vertex_count - sum(index < original_vertex_count for index in referenced)
    report.update(
        status="repaired",
        topology_status="repaired",
        kept_vertices=len(referenced),
        kept_faces=len(patched_faces),
        removed_non_manifold_faces=removed_face_count,
        removed_topology_vertices=removed_vertex_count,
        filled_boundary_loops=len(cycles),
        added_vertices=len(cycles),
        added_faces=sum(len(cycle) - 1 for cycle in cycles),
        remaining_inconsistent_winding_edges=0,
        remaining_invalid_edges=0,
    )
    report.pop("topology_deferred_reason", None)
    _write_mesh_repair_report(report_path, report)
    return report


def _validate_obj_topology(path: Path, allow_repairable: bool = False) -> tuple[int, int, int]:
    vertex_count = 0
    faces: list[tuple[int, int, int]] = []
    try:
        with path.open("r", encoding="utf-8", errors="strict") as stream:
            for line in stream:
                fields = line.strip().split()
                if not fields or fields[0].startswith("#"):
                    continue
                keyword = fields[0].lower()
                if keyword == "v":
                    vertex_count += 1
                elif keyword == "f":
                    if len(fields) != 4:
                        raise TripoError("The generated OBJ must contain only triangular faces.")
                    face = tuple(_resolve_obj_index(value, vertex_count, "vertex") for value in fields[1:])
                    if len(set(face)) != 3:
                        raise TripoError("The generated OBJ contains a degenerate triangle.")
                    faces.append(face)
                    if len(faces) > MAX_MODEL_FACES:
                        raise TripoError(f"The generated OBJ exceeds the {MAX_MODEL_FACES}-triangle limit.")
    except UnicodeDecodeError:
        raise TripoError("The generated OBJ is not valid UTF-8 text.") from None
    except OSError:
        raise TripoError("The generated OBJ could not be read.") from None
    if vertex_count == 0 or not faces:
        raise TripoError("The generated OBJ does not contain usable geometry.")

    parent = list(range(vertex_count))

    def find(index: int) -> int:
        while parent[index] != index:
            parent[index] = parent[parent[index]]
            index = parent[index]
        return index

    def unite(left: int, right: int) -> None:
        left_root, right_root = find(left), find(right)
        if left_root != right_root:
            parent[right_root] = left_root

    referenced: set[int] = set()
    edge_uses: dict[tuple[int, int], list[tuple[int, int]]] = {}
    for face in faces:
        referenced.update(face)
        unite(face[0], face[1])
        unite(face[1], face[2])
        for left, right in ((face[0], face[1]), (face[1], face[2]), (face[2], face[0])):
            edge = (left, right) if left < right else (right, left)
            edge_uses.setdefault(edge, []).append((left, right))

    component_count = len({find(index) for index in referenced})
    invalid_edges = sum(
        len(uses) != 2 or (len(uses) == 2 and uses[0] == uses[1])
        for uses in edge_uses.values()
    )
    repairable_edge_limit = max(64, len(faces) // 100)
    if invalid_edges and (
        not allow_repairable or len(faces) < 4 or invalid_edges > repairable_edge_limit
    ):
        raise TripoError(
            "Tripo generated a non-watertight, non-manifold, or inconsistently wound mesh. "
            "Regenerate before importing into OrcaSlicer."
        )
    return len(faces), component_count, invalid_edges


def _validate_artifact(path: Path, format_name: str, allow_repairable_obj: bool = False) -> int:
    try:
        size = path.stat().st_size
        with path.open("rb") as stream:
            signature = stream.read(84)
    except OSError:
        raise TripoError("The generated artifact could not be read.") from None
    if size <= 0 or size > MAX_ARTIFACT_BYTES:
        raise TripoError("The generated artifact has an invalid size.")
    if format_name == "obj":
        _validate_obj_vertex_colors(path)
        _validate_obj_topology(path, allow_repairable=allow_repairable_obj)
    if format_name == "3mf" and not signature.startswith(b"PK\x03\x04"):
        raise TripoError("Tripo returned an invalid 3MF artifact.")
    if format_name == "stl":
        ascii_stl = signature.lstrip().lower().startswith(b"solid")
        binary_stl = len(signature) >= 84 and int.from_bytes(signature[80:84], "little") > 0
        if not ascii_stl and not binary_stl:
            raise TripoError("Tripo returned an invalid STL artifact.")
    return size


def _generate_job(job: Job, prepared_prompt: str, resume: bool = False) -> None:
    try:
        artifact: Path | None = None
        last_quality_error: TripoError | None = None
        for attempt_number in range(1, MAX_GENERATION_ATTEMPTS + 1):
            _stop_boundary(job)
            with _JOBS_LOCK:
                job.state = "running"
                job.phase = "generating"
                job.message = f"Generating printable model (attempt {attempt_number} of {MAX_GENERATION_ATTEMPTS})."
                job.progress = 20
                _persist_job(job)
            existing = job.attempts[attempt_number - 1] if resume and len(job.attempts) >= attempt_number else {}
            generation_id = existing.get("generation_task_id", "")
            if not isinstance(generation_id, str) or not generation_id:
                if resume:
                    raise TripoError("The paid model task reference is unavailable; start a new generation manually.")
                if job.source == "text" and job.preview_path is None:
                    generation_id = create_text_task(prepared_prompt, job.face_limit)
                else:
                    preview = job.model_reference_path or job.preview_path
                    if preview is None:
                        raise RuntimeError("The prepared preview is unavailable.")
                    token = upload_image(preview)
                    _stop_boundary(job)
                    generation_id = create_image_task(token, job.face_limit)
                _record_attempt(job, attempt_number, generation_task_id=generation_id, status="running")
            _stop_boundary(job)
            wait_for_task(
                generation_id,
                stop_event=job.stop_event,
                progress=_progress_callback(job, 20, 70),
            )
            _stop_boundary(job)
            try:
                candidate = _download_conversion(job, generation_id, MODEL_ARTIFACT_FORMAT, attempt_number, True) if resume else \
                    _download_conversion(job, generation_id, MODEL_ARTIFACT_FORMAT, attempt_number)
                face_count, _, _ = _validate_obj_topology(candidate, allow_repairable=True)
                _validate_face_target(face_count, job.face_limit)
                artifact = job.directory / "model-vertex-color.obj"
                if candidate.resolve() != artifact.resolve():
                    shutil.copyfile(candidate, artifact)
                _record_attempt(job, attempt_number, status="accepted", artifact=str(candidate.name), error="")
                break
            except TripoError as exc:
                if _SHUT_DOWN:
                    raise SidecarRestart() from None
                message = str(exc)
                retryable_quality_error = any(
                    marker in message.lower()
                    for marker in ("triangle limit", "non-watertight", "non-manifold", "degenerate triangle")
                )
                _record_attempt(job, attempt_number, status="rejected", error=message)
                if not retryable_quality_error or attempt_number == MAX_GENERATION_ATTEMPTS:
                    raise
                last_quality_error = exc
        if artifact is None:
            raise last_quality_error or TripoError("No printable model passed validation.")

        with _JOBS_LOCK:
            if job.stop_event.is_set():
                raise JobStopped()
            job.artifact_path = artifact
            job.artifact_format = MODEL_ARTIFACT_FORMAT
            job.state = "ready"
            job.phase = "ready"
            job.message = "Generated model is ready."
            job.progress = 100
            _persist_job(job)
    except SidecarRestart:
        with _JOBS_LOCK:
            job.state = "queued"
            job.phase = "resuming"
            job.message = "The existing paid model task will resume when the sidecar restarts."
            _persist_job(job)
    except JobStopped:
        _mark_stopped(job)
    except TripoError as exc:
        if _SHUT_DOWN:
            with _JOBS_LOCK:
                job.state = "queued"
                job.phase = "resuming"
                job.message = "The existing paid model task will resume when the sidecar restarts."
                _persist_job(job)
        else:
            _fail_job(job, str(exc))
    except Exception:
        _fail_job(job, "Model generation failed.")
    finally:
        _finish_deleted(job)


def _submit(job: Job, worker: Callable[..., None], *args: Any) -> None:
    try:
        future = _EXECUTOR.submit(worker, job, *args)
    except RuntimeError:
        raise RequestError("service_unavailable", "The model job service is shutting down.", 503, True) from None
    with _JOBS_LOCK:
        job.future = future


def shutdown_sidecar() -> None:
    global _SHUT_DOWN
    with _SHUTDOWN_LOCK:
        if _SHUT_DOWN:
            return
        _SHUT_DOWN = True
    with _JOBS_LOCK:
        jobs = list(_JOBS.values())
        for job in jobs:
            job.stop_event.set()
    _EXECUTOR.shutdown(wait=True, cancel_futures=False)
    with _JOBS_LOCK:
        _JOBS.clear()


atexit.register(shutdown_sidecar)


class Handler(BaseHTTPRequestHandler):
    server_version = "OrcaAISidecar/1.0"

    def log_message(self, fmt: str, *args: Any) -> None:
        sys.stderr.write("[orca-ai] " + fmt % args + "\n")

    def send_json(self, status: int, payload: dict[str, Any]) -> None:
        encoded = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(encoded)))
        self.end_headers()
        self.wfile.write(encoded)

    def send_bytes(
        self,
        stream: BinaryIO,
        size: int,
        content_type: str,
        filename: str | None = None,
    ) -> None:
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(size))
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Cache-Control", "no-store")
        if filename is not None:
            self.send_header("Content-Disposition", f'attachment; filename="{filename}"')
        self.end_headers()
        try:
            while chunk := stream.read(64 * 1024):
                self.wfile.write(chunk)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def read_json(self) -> dict[str, Any]:
        length = int(self.headers.get("Content-Length", "0"))
        if length <= 0:
            return {}
        if length > MAX_REQUEST_BYTES:
            raise ValueError("request body too large")
        parsed = json.loads(self.rfile.read(length).decode("utf-8"))
        if not isinstance(parsed, dict):
            raise ValueError("request body must be a JSON object")
        return parsed

    def _read_body(self, limit: int) -> bytes:
        raw_length = self.headers.get("Content-Length")
        if raw_length is None:
            raise RequestError("invalid_request", "Content-Length is required.", 400)
        try:
            length = int(raw_length)
        except ValueError:
            raise RequestError("invalid_request", "Content-Length is invalid.", 400) from None
        if length < 0:
            raise RequestError("invalid_request", "Content-Length is invalid.", 400)
        if length > limit:
            raise RequestError("request_too_large", "Request body is too large.", 413)
        body = self.rfile.read(length)
        if len(body) != length:
            raise RequestError("invalid_request", "Request body is incomplete.", 400)
        return body

    def _read_model_json(self) -> dict[str, Any]:
        content_type = self.headers.get("Content-Type", "").split(";", 1)[0].strip().lower()
        if content_type != "application/json":
            raise RequestError("unsupported_media_type", "Content-Type must be application/json.", 415)
        body = self._read_body(MAX_REQUEST_BYTES)
        try:
            parsed = json.loads(body.decode("utf-8")) if body else {}
        except (UnicodeDecodeError, json.JSONDecodeError):
            raise RequestError("invalid_json", "Request body contains malformed JSON.", 400) from None
        if not isinstance(parsed, dict):
            raise RequestError("invalid_request", "Request body must be a JSON object.", 400)
        return parsed

    def _read_image_multipart(self) -> tuple[dict[str, str], bytes, str]:
        content_type = self.headers.get("Content-Type", "")
        if len(content_type) > 1024 or not content_type.lower().startswith("multipart/form-data;"):
            raise RequestError("unsupported_media_type", "Content-Type must be multipart/form-data.", 415)
        body = self._read_body(MAX_MULTIPART_BYTES)
        try:
            header = b"Content-Type: " + content_type.encode("latin-1") + b"\r\nMIME-Version: 1.0\r\n\r\n"
        except UnicodeEncodeError:
            raise RequestError("invalid_multipart", "Multipart Content-Type is invalid.", 400) from None
        message = BytesParser(policy=policy.default).parsebytes(header + body)
        if not message.is_multipart():
            raise RequestError("invalid_multipart", "Multipart request is malformed.", 400)

        fields: dict[str, str] = {}
        image: bytes | None = None
        image_content_type = ""
        seen: set[str] = set()
        for part in message.iter_parts():
            if part.is_multipart() or part.get_content_disposition() != "form-data":
                raise RequestError("invalid_multipart", "Nested or invalid multipart data is not supported.", 400)
            name = part.get_param("name", header="content-disposition")
            if name not in {"request_id", "instruction", "palette", "palette_roles", "style", "custom_style", "print", "image"} or name in seen:
                raise RequestError("invalid_multipart", "Multipart fields are unexpected or duplicated.", 400)
            seen.add(name)
            payload = part.get_payload(decode=True) or b""
            if name == "image":
                image = payload
                image_content_type = part.get_content_type().lower()
            else:
                if len(payload) > MAX_PROMPT_BYTES:
                    raise RequestError("invalid_request", f"{name} exceeds the 2000-byte limit.", 400)
                try:
                    fields[name] = payload.decode(part.get_content_charset() or "utf-8")
                except (LookupError, UnicodeDecodeError):
                    raise RequestError("invalid_request", f"{name} must be UTF-8 text.", 400) from None
        if image is None:
            raise RequestError("invalid_request", "image is required.", 400)
        return fields, image, image_content_type

    def _require_native_client(self) -> bool:
        if self.headers.get("X-OrcaSlicer-Client") != "native":
            self._model_error(401, "client_required", "X-OrcaSlicer-Client must be native.")
            return False
        return True

    def _model_error(
        self,
        status: int,
        code: str,
        message: str,
        retryable: bool = False,
    ) -> None:
        self.send_json(status, {"error": {"code": code, "message": message, "retryable": retryable}})

    @staticmethod
    def _job_route(path: str) -> tuple[str | None, str | None]:
        prefix = "/v1/orcaslicer/model-jobs/"
        if not path.startswith(prefix):
            return None, None
        parts = path[len(prefix) :].split("/")
        if len(parts) == 1 and parts[0]:
            action = "status"
        elif len(parts) == 2 and parts[0] and (
            parts[1] in {
                "raw-preview", "strict-preview", "preview", "heatmap", "metadata",
                "background-mask", "subject-mask", "generate", "stop", "artifact",
                "recheck", "visual-review", "model-view-sheet",
            }
            or re.fullmatch(r"mask-[a-z0-9_]+", parts[1])
        ):
            action = parts[1]
        else:
            return None, None
        try:
            parsed = uuid.UUID(parts[0])
        except ValueError:
            return None, None
        if str(parsed) != parts[0].lower():
            return None, None
        return parts[0].lower(), action

    def _get_job(self, job_id: str) -> Job | None:
        with _JOBS_LOCK:
            return _JOBS.get(job_id)

    def do_GET(self) -> None:
        if self.path == "/health":
            config = os.environ.get("OPENAI_API_KEY", "")
            generation_preprocessing = bool(config) or _preprocess_fallback_enabled()
            self.send_json(
                200,
                {
                    "ok": True,
                    "protocol_version": 1,
                    "sidecar_version": SIDECAR_VERSION,
                    "runtime": {
                        "openai_base_url": os.environ.get("OPENAI_BASE_URL", "https://api.openai.com/v1").rstrip("/"),
                    },
                    "capabilities": {
                        "config_proposal": {"available": bool(config)},
                        "model_generation": {
                            "available": generation_preprocessing and bool(os.environ.get("TRIPO_API_KEY", "")),
                            "sources": ["text", "image"],
                            "styles": list(STYLE_IDS),
                            "artifact_formats": [MODEL_ARTIFACT_FORMAT],
                            "face_limits": list(MODEL_FACE_LIMITS),
                            "default_face_limit": DEFAULT_MODEL_FACE_LIMIT,
                            "printable_image_pipeline": {
                                "available": True,
                                "print_modes": ["solid_regions"],
                                "color_distances": ["ciede2000", "delta_e76"],
                                "outputs": [
                                    "raw_preview", "strict_preview", "clean_preview", "model_reference",
                                    "heatmap", "masks", "metadata",
                                ],
                            },
                        },
                    },
                },
            )
            return

        if not self.path.startswith("/v1/orcaslicer/model-jobs"):
            self.send_json(404, {"error": "not found"})
            return
        if not self._require_native_client():
            return
        if self.path == "/v1/orcaslicer/model-jobs/latest":
            with _JOBS_LOCK:
                candidates = [
                    job for job in _JOBS.values()
                    if job.state in {"preprocessing", "awaiting_confirmation", "queued", "running", "stopping", "ready"}
                ]
                response = _public_job(max(candidates, key=lambda item: item.updated_at)) if candidates else None
            self.send_json(200, {"job": response})
            return
        job_id, action = self._job_route(self.path)
        downloadable = {
            "status", "input", "raw-preview", "strict-preview", "preview", "heatmap", "metadata",
            "background-mask", "subject-mask", "artifact", "model-view-sheet",
        }
        if not job_id or (action not in downloadable and not (action or "").startswith("mask-")):
            self._model_error(404, "not_found", "Model job route not found.")
            return
        job = self._get_job(job_id)
        if job is None:
            self._model_error(404, "job_not_found", "Model job not found.")
            return
        if action == "status":
            with _JOBS_LOCK:
                response = _public_job(job)
            self.send_json(200, {"job": response})
            return
        self._download_job_file(job, action)

    def do_POST(self) -> None:
        if self.path == "/v1/orcaslicer/config-proposal":
            try:
                request = self.read_json()
                if not str(request.get("user_message", "")).strip():
                    self.send_json(400, {"error": "user_message is required"})
                    return
                if not extract_allowed_keys(request):
                    self.send_json(400, {"error": "allowed_changes is required"})
                    return
                self.send_json(200, normalize_proposal(provider_request(request), request))
            except ValueError as exc:
                self.send_json(400, {"error": str(exc)})
            except Exception as exc:
                self.send_json(502, {"error": str(exc)})
            return

        if not self.path.startswith("/v1/orcaslicer/model-jobs"):
            self.send_json(404, {"error": "not found"})
            return
        if not self._require_native_client():
            return
        try:
            if self.path == "/v1/orcaslicer/model-jobs/text":
                self._create_text_job()
                return
            if self.path == "/v1/orcaslicer/model-jobs/image":
                self._create_image_job()
                return
            job_id, action = self._job_route(self.path)
            if not job_id or action not in {"generate", "stop", "recheck", "visual-review"}:
                self._model_error(404, "not_found", "Model job route not found.")
                return
            if action == "generate":
                self._generate(job_id)
            elif action == "recheck":
                self._recheck(job_id)
            elif action == "visual-review":
                self._visual_review(job_id)
            else:
                self._stop(job_id)
        except RequestError as exc:
            self._model_error(exc.status, exc.code, exc.message, exc.retryable)

    def do_DELETE(self) -> None:
        if not self.path.startswith("/v1/orcaslicer/model-jobs"):
            self.send_json(404, {"error": "not found"})
            return
        if not self._require_native_client():
            return
        job_id, action = self._job_route(self.path)
        if not job_id or action != "status":
            self._model_error(404, "not_found", "Model job route not found.")
            return
        with _JOBS_LOCK:
            job = _JOBS.get(job_id)
            if job is None:
                self._model_error(404, "job_not_found", "Model job not found.")
                return
            if job.state in {"preprocessing", "queued", "running", "stopping"}:
                job.delete_requested = True
                job.stop_event.set()
                job.state = "stopping"
                job.phase = "stopping"
                job.message = "Stopping model generation."
            else:
                _JOBS.pop(job_id)
                _remove_job_state(job)
        self.send_response(204)
        self.end_headers()

    def _create_text_job(self) -> None:
        if not os.environ.get("OPENAI_API_KEY", "") and not _preprocess_fallback_enabled():
            raise RequestError("feature_unavailable", "Text preprocessing is not configured.", 503)
        request = self._read_model_json()
        _text_field(request.get("request_id"), "request_id")
        prompt = _text_field(request.get("prompt"), "prompt")
        palette = _normalize_palette(request.get("palette"))
        palette_roles = _normalize_palette_roles(request.get("palette_roles"), palette)
        style = _normalize_style(request.get("style"))
        custom_style = _normalize_custom_style(request.get("custom_style"), style)
        print_settings = _normalize_print_settings(request.get("print"))
        job = _new_job("text", palette, palette_roles, style, custom_style, print_settings)
        job.user_prompt = prompt
        _persist_job(job)
        with _JOBS_LOCK:
            _JOBS[job.id] = job
        try:
            _submit(job, _preprocess_text_job, prompt)
        except RequestError:
            with _JOBS_LOCK:
                _JOBS.pop(job.id, None)
            _cleanup_job(job)
            raise
        with _JOBS_LOCK:
            response = _public_job(job)
        self.send_json(202, {"job": response})

    def _create_image_job(self) -> None:
        if not os.environ.get("OPENAI_API_KEY", ""):
            raise RequestError("feature_unavailable", "AI style preview generation is not configured.", 503)
        fields, image, declared_type = self._read_image_multipart()
        _text_field(fields.get("request_id"), "request_id")
        instruction = _normalize_image_instruction(fields.get("instruction"))
        palette = _multipart_palette(fields.get("palette"))
        palette_roles = _multipart_palette_roles(fields.get("palette_roles"), palette)
        style = _normalize_style(fields.get("style"))
        custom_style = _normalize_custom_style(fields.get("custom_style"), style)
        try:
            print_payload = json.loads(fields.get("print", "{}"))
        except json.JSONDecodeError:
            raise RequestError("invalid_print_settings", "print settings must be valid JSON", 400) from None
        print_settings = _normalize_print_settings(print_payload)
        if len(image) > MAX_IMAGE_BYTES:
            raise RequestError("image_too_large", "Image exceeds the 20 MB limit.", 413)
        detected_type = _image_type(image)
        if detected_type is None:
            raise RequestError("unsupported_image", "Image must be PNG or JPEG.", 415)
        if declared_type not in {"application/octet-stream", detected_type}:
            raise RequestError("unsupported_image", "Image Content-Type does not match its data.", 415)
        job = _new_job("image", palette, palette_roles, style, custom_style, print_settings)
        job.user_prompt = instruction
        suffix = ".png" if detected_type == "image/png" else ".jpg"
        input_path = job.directory / f"input-{uuid.uuid4().hex}{suffix}"
        try:
            input_path.write_bytes(image)
        except OSError:
            _cleanup_job(job)
            raise RequestError("service_unavailable", "The uploaded image could not be stored.", 503, True) from None
        job.input_path = input_path
        _persist_job(job)
        with _JOBS_LOCK:
            _JOBS[job.id] = job
        try:
            _submit(job, _preprocess_image_job, input_path, instruction)
        except RequestError:
            with _JOBS_LOCK:
                _JOBS.pop(job.id, None)
            _cleanup_job(job)
            raise
        with _JOBS_LOCK:
            response = _public_job(job)
        self.send_json(202, {"job": response})

    def _generate(self, job_id: str) -> None:
        request = self._read_model_json()
        if "prepared_prompt" not in request:
            raise RequestError("invalid_request", "prepared_prompt is required.", 400)
        raw_prompt = request.get("prepared_prompt")
        if not isinstance(raw_prompt, str):
            raise RequestError("invalid_request", "prepared_prompt must be a string.", 400)
        if len(raw_prompt.strip().encode("utf-8")) > MAX_PROMPT_BYTES:
            raise RequestError("invalid_request", "prepared_prompt exceeds the 2000-byte limit.", 400)
        palette = _normalize_palette(request.get("palette"))
        face_limit = _normalize_face_limit(request.get("face_limit", DEFAULT_MODEL_FACE_LIMIT))
        job = self._get_job(job_id)
        if job is None:
            raise RequestError("job_not_found", "Model job not found.", 404)
        with _JOBS_LOCK:
            if job.state != "awaiting_confirmation":
                raise RequestError("invalid_job_state", "Job is not awaiting confirmation.", 409)
            if palette != job.palette:
                raise RequestError(
                    "palette_changed",
                    "The filament palette changed after preview; create a new preview before generating 3D.",
                    409,
                )
            prepared_prompt = raw_prompt.strip()
            if job.source == "text" and not prepared_prompt:
                raise RequestError("invalid_request", "prepared_prompt is required for text generation.", 400)
            if not os.environ.get("TRIPO_API_KEY", ""):
                raise RequestError("feature_unavailable", "Model generation is not configured.", 503)
            job.prepared_prompt = prepared_prompt if job.source == "text" else ""
            job.face_limit = face_limit
            job.state = "queued"
            job.phase = "generating"
            job.message = "Generation queued."
            job.progress = 20
            job.artifact_path = None
            job.artifact_format = ""
            _persist_job(job)
        try:
            _submit(job, _generate_job, prepared_prompt)
        except RequestError:
            with _JOBS_LOCK:
                job.state = "awaiting_confirmation"
                job.phase = "awaiting_confirmation"
                job.message = "Review the prepared request before generation."
                job.progress = 15
                _persist_job(job)
            raise
        with _JOBS_LOCK:
            response = _public_job(job)
        self.send_json(200, {"job": response})

    def _stop(self, job_id: str) -> None:
        self._read_model_json()
        job = self._get_job(job_id)
        if job is None:
            raise RequestError("job_not_found", "Model job not found.", 404)
        with _JOBS_LOCK:
            if job.state in {"preprocessing", "queued", "running", "stopping"}:
                job.stop_event.set()
                job.state = "stopping"
                job.phase = "stopping"
                job.message = "Stopping model generation."
                _persist_job(job)
            elif job.state == "awaiting_confirmation":
                job.stop_event.set()
                job.state = "stopped"
                job.phase = "stopped"
                job.message = "Model generation stopped."
                job.progress = 0
                _persist_job(job)
            elif job.state != "stopped":
                raise RequestError("invalid_job_state", "Job cannot be stopped in its current state.", 409)
            response = _public_job(job)
        self.send_json(200, {"job": response})

    def _recheck(self, job_id: str) -> None:
        self._read_model_json()
        job = self._get_job(job_id)
        if job is None:
            job = _adopt_legacy_completed_job(job_id)
        if job is None:
            raise RequestError("job_not_found", "Model job not found.", 404)
        with _JOBS_LOCK:
            if job.state in {"preprocessing", "queued", "running", "stopping"}:
                raise RequestError("invalid_job_state", "Model quality cannot be checked while the job is running.", 409)
            artifact = job.artifact_path
            artifact_format = job.artifact_format
        if artifact is None or artifact_format != "obj":
            raise RequestError("artifact_not_ready", "The model OBJ is not available for quality checking.", 409)
        try:
            resolved_artifact = artifact.resolve(strict=True)
            resolved_artifact.relative_to(job.directory.resolve(strict=True))
        except (OSError, ValueError):
            raise RequestError("artifact_not_ready", "The registered model OBJ is unavailable.", 409) from None
        quality = analyze_printable_obj(resolved_artifact, allow_repairable_topology=True)
        try:
            write_model_quality_report(quality, job.directory / MODEL_QUALITY_FILENAME)
        except ModelQualityError as exc:
            raise RequestError("quality_report_unavailable", str(exc), 503, True) from None
        with _JOBS_LOCK:
            if _JOBS.get(job_id) is not job:
                raise RequestError("job_not_found", "Model job is no longer available.", 404)
            _persist_job(job)
            response = _public_job(job)
        self.send_json(200, {"job": response})

    def _visual_review(self, job_id: str) -> None:
        request = self._read_model_json()
        force = request.get("force", False)
        if not isinstance(force, bool):
            raise RequestError("invalid_request", "force must be a boolean.", 400)
        job = self._get_job(job_id)
        if job is None:
            job = _adopt_legacy_completed_job(job_id)
        if job is None:
            raise RequestError("job_not_found", "Model job not found.", 404)
        with _JOBS_LOCK:
            if job.state in {"preprocessing", "queued", "running", "stopping"}:
                raise RequestError("job_busy", "Model generation is still running.", 409)
            artifact = job.artifact_path
        if artifact is None or job.artifact_format != MODEL_ARTIFACT_FORMAT:
            raise RequestError("artifact_not_ready", "The model OBJ is not available for visual review.", 409)
        try:
            resolved_directory = job.directory.resolve(strict=True)
            resolved_artifact = artifact.resolve(strict=True)
            resolved_artifact.relative_to(resolved_directory)
        except (OSError, ValueError):
            raise RequestError("artifact_not_ready", "The registered model OBJ is unavailable.", 409) from None
        reference: Path | None = None
        if job.source == "image" and job.input_path is not None:
            try:
                candidate = job.input_path.resolve(strict=True)
                candidate.relative_to(resolved_directory)
                reference = candidate
            except (OSError, ValueError):
                reference = None
        review_model_visual_quality(
            resolved_artifact,
            resolved_directory,
            description=job.user_prompt,
            style=job.style,
            reference_path=reference,
            force=force,
        )
        with _JOBS_LOCK:
            if _JOBS.get(job_id) is not job:
                raise RequestError("job_not_found", "Model job is no longer available.", 404)
            _persist_job(job)
            response = _public_job(job)
        self.send_json(200, {"job": response})

    def _download_job_file(self, job: Job, kind: str) -> None:
        with _JOBS_LOCK:
            fixed_paths = {
                "input": job.input_path,
                "raw-preview": job.raw_preview_path,
                "strict-preview": job.strict_preview_path,
                "preview": job.preview_path,
                "model-reference": job.model_reference_path,
                "heatmap": job.heatmap_path,
                "metadata": job.metadata_path,
                "background-mask": job.background_mask_path,
                "subject-mask": job.subject_mask_path,
                "artifact": job.artifact_path,
                "model-view-sheet": job.directory / "model-view-sheet.png",
            }
            path = job.mask_paths.get(kind[5:]) if kind.startswith("mask-") else fixed_paths.get(kind)
            ready, size = _file_info(path)
            if not ready or path is None:
                self._model_error(409, f"{kind}_not_ready", f"Model job {kind} is not ready.", True)
                return
            image_kinds = {
                "input", "raw-preview", "strict-preview", "preview", "model-reference", "heatmap",
                "background-mask", "subject-mask", "model-view-sheet",
            }
            content_type = _stored_image_type(path) if kind in image_kinds or kind.startswith("mask-") else \
                "application/json; charset=utf-8" if kind == "metadata" else {
                "obj": "model/obj",
                "3mf": "application/vnd.ms-package.3dmanufacturing-3dmodel+xml",
                "stl": "model/stl",
            }.get(job.artifact_format, "application/octet-stream")
            filename = f"orcaslicer-model-{job.id}.{job.artifact_format}" if kind == "artifact" else None
            try:
                stream = path.open("rb")
            except OSError:
                self._model_error(503, "file_unavailable", "Model job file is unavailable.", True)
                return
        with stream:
            self.send_bytes(stream, size, content_type, filename)


class LoopbackServer(ThreadingHTTPServer):
    daemon_threads = False


def main() -> int:
    host = HOST.strip().lower()
    if host not in _LOOPBACK_HOSTS:
        print("ORCASLICER_AI_SIDECAR_HOST must be 127.0.0.1, localhost, or ::1.", file=sys.stderr)
        return 2
    if host == "::1":
        LoopbackServer.address_family = socket.AF_INET6
    _restore_jobs()
    server = LoopbackServer((HOST, PORT), Handler)
    print(f"OrcaSlicer AI sidecar listening on http://{HOST}:{PORT}")
    print("Config endpoint: POST /v1/orcaslicer/config-proposal")
    print("Model jobs: /v1/orcaslicer/model-jobs/*")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
        shutdown_sidecar()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
