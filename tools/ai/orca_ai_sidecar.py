#!/usr/bin/env python3
from __future__ import annotations

import atexit
import math
import json
import os
import re
import shutil
import socket
import sys
import tempfile
import threading
import uuid
from concurrent.futures import Future, ThreadPoolExecutor
from dataclasses import dataclass, field
from email import policy
from email.parser import BytesParser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any, BinaryIO, Callable

from openai_preprocessor import OpenAIPreprocessorError, complete_text, preprocess_image, preprocess_text
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
SIDECAR_VERSION = "orcaslicer-ai-sidecar-v2"
MAX_REQUEST_BYTES = 256 * 1024
MAX_CHANGES = 8
MAX_PROMPT_BYTES = 2000
MAX_IMAGE_BYTES = 20 * 1024 * 1024
MAX_MULTIPART_BYTES = MAX_IMAGE_BYTES + 256 * 1024
MAX_ARTIFACT_BYTES = 250 * 1024 * 1024
_LOOPBACK_HOSTS = {"127.0.0.1", "localhost", "::1"}
_TEMP_ROOT = Path(tempfile.mkdtemp(prefix="orcaslicer-ai-"))
_JOBS_LOCK = threading.RLock()
_JOBS: dict[str, "Job"] = {}
_EXECUTOR = ThreadPoolExecutor(max_workers=1, thread_name_prefix="orca-model-job")
_SHUTDOWN_LOCK = threading.Lock()
_SHUT_DOWN = False


@dataclass
class Job:
    id: str
    source: str
    directory: Path
    state: str = "preprocessing"
    phase: str = "preprocessing"
    message: str = "Preparing model generation request."
    progress: int = 5
    prepared_prompt: str = ""
    preview_path: Path | None = None
    preview_content_type: str = ""
    artifact_path: Path | None = None
    artifact_format: str = ""
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


def _image_type(data: bytes) -> str | None:
    if data.startswith(b"\x89PNG\r\n\x1a\n"):
        return "image/png"
    if data.startswith(b"\xff\xd8\xff"):
        return "image/jpeg"
    return None


def _new_job(source: str) -> Job:
    job_id = str(uuid.uuid4())
    directory = Path(tempfile.mkdtemp(prefix=uuid.uuid4().hex + "-", dir=_TEMP_ROOT))
    return Job(id=job_id, source=source, directory=directory)


def _file_info(path: Path | None) -> tuple[bool, int]:
    if path is None:
        return False, 0
    try:
        size = path.stat().st_size
    except OSError:
        return False, 0
    return size > 0, size


def _public_job(job: Job) -> dict[str, Any]:
    preview_ready, preview_size = _file_info(job.preview_path)
    artifact_ready, artifact_size = _file_info(job.artifact_path)
    artifact_filename = ""
    if artifact_ready:
        artifact_filename = f"orcaslicer-model-{job.id}.{job.artifact_format}"
    return {
        "id": job.id,
        "source": job.source,
        "state": job.state,
        "phase": job.phase,
        "message": job.message,
        "progress": job.progress,
        "prepared_prompt": job.prepared_prompt if job.source == "text" else "",
        "preview": {
            "ready": preview_ready,
            "content_type": job.preview_content_type if preview_ready else "",
            "size_bytes": preview_size if preview_ready else 0,
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
    try:
        shutil.rmtree(job.directory, ignore_errors=True)
    except OSError:
        pass


def _finish_deleted(job: Job) -> None:
    cleanup = False
    with _JOBS_LOCK:
        if job.delete_requested:
            _JOBS.pop(job.id, None)
            cleanup = True
    if cleanup:
        _cleanup_job(job)


def _mark_stopped(job: Job) -> None:
    with _JOBS_LOCK:
        job.state = "stopped"
        job.phase = "stopped"
        job.message = "Model generation stopped."
        job.progress = 0
        job.artifact_path = None
        job.artifact_format = ""


def _stop_boundary(job: Job) -> None:
    if job.stop_event.is_set():
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


def _preprocess_text_job(job: Job, prompt: str) -> None:
    try:
        _stop_boundary(job)
        prepared = preprocess_text(prompt).strip()
        if not prepared or len(prepared.encode("utf-8")) > MAX_PROMPT_BYTES:
            raise OpenAIPreprocessorError("The prepared prompt is empty or exceeds the 2000-byte limit.")
        with _JOBS_LOCK:
            if job.stop_event.is_set():
                raise JobStopped()
            job.prepared_prompt = prepared
            job.state = "awaiting_confirmation"
            job.phase = "awaiting_confirmation"
            job.message = "Review the prepared prompt before generation."
            job.progress = 15
    except JobStopped:
        _mark_stopped(job)
    except OpenAIPreprocessorError as exc:
        _fail_job(job, str(exc))
    except Exception:
        _fail_job(job, "Text preprocessing failed.")
    finally:
        _finish_deleted(job)


def _preprocess_image_job(job: Job, input_path: Path, instruction: str) -> None:
    preview = job.directory / "preview.png"
    try:
        _stop_boundary(job)
        preprocess_image(input_path, instruction, preview)
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
            job.message = "Review the prepared image before generation."
            job.progress = 15
    except JobStopped:
        _mark_stopped(job)
    except OpenAIPreprocessorError as exc:
        _fail_job(job, str(exc))
    except Exception:
        _fail_job(job, "Image preprocessing failed.")
    finally:
        try:
            input_path.unlink(missing_ok=True)
        except OSError:
            pass
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

    return update


def _download_conversion(job: Job, generation_id: str, format_name: str) -> Path:
    with _JOBS_LOCK:
        job.state = "running"
        job.phase = "converting"
        job.message = f"Converting generated geometry to {format_name.upper()}."
        job.progress = 75
    conversion_id = create_conversion(generation_id, format_name)
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
    destination = job.directory / f"artifact.{format_name}"
    download_task_artifact(result, destination, MAX_ARTIFACT_BYTES)
    _stop_boundary(job)
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


def _validate_artifact(path: Path, format_name: str) -> int:
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
    if format_name == "3mf" and not signature.startswith(b"PK\x03\x04"):
        raise TripoError("Tripo returned an invalid 3MF artifact.")
    if format_name == "stl":
        ascii_stl = signature.lstrip().lower().startswith(b"solid")
        binary_stl = len(signature) >= 84 and int.from_bytes(signature[80:84], "little") > 0
        if not ascii_stl and not binary_stl:
            raise TripoError("Tripo returned an invalid STL artifact.")
    return size


def _generate_job(job: Job, prepared_prompt: str) -> None:
    try:
        _stop_boundary(job)
        with _JOBS_LOCK:
            job.state = "running"
            job.phase = "generating"
            job.message = "Generating model geometry."
            job.progress = 20
        if job.source == "text":
            generation_id = create_text_task(prepared_prompt)
        else:
            preview = job.preview_path
            if preview is None:
                raise RuntimeError("The prepared preview is unavailable.")
            token = upload_image(preview)
            _stop_boundary(job)
            generation_id = create_image_task(token)
        _stop_boundary(job)
        wait_for_task(
            generation_id,
            stop_event=job.stop_event,
            progress=_progress_callback(job, 20, 70),
        )
        _stop_boundary(job)

        artifact = None
        artifact_format = ""
        last_error: TripoError | None = None
        for candidate in ("obj", "3mf", "stl"):
            try:
                artifact = _download_conversion(job, generation_id, candidate)
                artifact_format = candidate
                break
            except TripoError as exc:
                if job.stop_event.is_set():
                    raise JobStopped()
                last_error = exc
        if artifact is None:
            raise last_error or TripoError("No supported generated artifact was available.")

        with _JOBS_LOCK:
            if job.stop_event.is_set():
                raise JobStopped()
            job.artifact_path = artifact
            job.artifact_format = artifact_format
            job.state = "ready"
            job.phase = "ready"
            job.message = "Generated model is ready."
            job.progress = 100
    except JobStopped:
        _mark_stopped(job)
    except TripoError as exc:
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
    shutil.rmtree(_TEMP_ROOT, ignore_errors=True)


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
            if name not in {"request_id", "instruction", "image"} or name in seen:
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
        elif len(parts) == 2 and parts[0] and parts[1] in {"preview", "generate", "stop", "artifact"}:
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
            self.send_json(
                200,
                {
                    "ok": True,
                    "protocol_version": 1,
                    "sidecar_version": SIDECAR_VERSION,
                    "capabilities": {
                        "config_proposal": {"available": bool(config)},
                        "model_generation": {
                            "available": bool(config) and bool(os.environ.get("TRIPO_API_KEY", "")),
                            "sources": ["text", "image"],
                            "artifact_formats": ["obj", "3mf", "stl"],
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
        job_id, action = self._job_route(self.path)
        if not job_id or action not in {"status", "preview", "artifact"}:
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
            if not job_id or action not in {"generate", "stop"}:
                self._model_error(404, "not_found", "Model job route not found.")
                return
            if action == "generate":
                self._generate(job_id)
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
        cleanup: Job | None = None
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
                cleanup = _JOBS.pop(job_id)
        if cleanup is not None:
            _cleanup_job(cleanup)
        self.send_response(204)
        self.end_headers()

    def _create_text_job(self) -> None:
        if not os.environ.get("OPENAI_API_KEY", ""):
            raise RequestError("feature_unavailable", "Text preprocessing is not configured.", 503)
        request = self._read_model_json()
        _text_field(request.get("request_id"), "request_id")
        prompt = _text_field(request.get("prompt"), "prompt")
        job = _new_job("text")
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
            raise RequestError("feature_unavailable", "Image preprocessing is not configured.", 503)
        fields, image, declared_type = self._read_image_multipart()
        _text_field(fields.get("request_id"), "request_id")
        instruction = _text_field(fields.get("instruction"), "instruction")
        if len(image) > MAX_IMAGE_BYTES:
            raise RequestError("image_too_large", "Image exceeds the 20 MB limit.", 413)
        detected_type = _image_type(image)
        if detected_type is None:
            raise RequestError("unsupported_image", "Image must be PNG or JPEG.", 415)
        if declared_type not in {"application/octet-stream", detected_type}:
            raise RequestError("unsupported_image", "Image Content-Type does not match its data.", 415)
        job = _new_job("image")
        suffix = ".png" if detected_type == "image/png" else ".jpg"
        input_path = job.directory / f"input-{uuid.uuid4().hex}{suffix}"
        try:
            input_path.write_bytes(image)
        except OSError:
            _cleanup_job(job)
            raise RequestError("service_unavailable", "The uploaded image could not be stored.", 503, True) from None
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
        job = self._get_job(job_id)
        if job is None:
            raise RequestError("job_not_found", "Model job not found.", 404)
        with _JOBS_LOCK:
            if job.state != "awaiting_confirmation":
                raise RequestError("invalid_job_state", "Job is not awaiting confirmation.", 409)
            prepared_prompt = raw_prompt.strip()
            if job.source == "text" and not prepared_prompt:
                raise RequestError("invalid_request", "prepared_prompt is required for text generation.", 400)
            if not os.environ.get("TRIPO_API_KEY", ""):
                raise RequestError("feature_unavailable", "Model generation is not configured.", 503)
            job.prepared_prompt = prepared_prompt if job.source == "text" else ""
            job.state = "queued"
            job.phase = "generating"
            job.message = "Generation queued."
            job.progress = 20
            job.artifact_path = None
            job.artifact_format = ""
        try:
            _submit(job, _generate_job, prepared_prompt)
        except RequestError:
            with _JOBS_LOCK:
                job.state = "awaiting_confirmation"
                job.phase = "awaiting_confirmation"
                job.message = "Review the prepared request before generation."
                job.progress = 15
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
            elif job.state == "awaiting_confirmation":
                job.stop_event.set()
                job.state = "stopped"
                job.phase = "stopped"
                job.message = "Model generation stopped."
                job.progress = 0
            elif job.state != "stopped":
                raise RequestError("invalid_job_state", "Job cannot be stopped in its current state.", 409)
            response = _public_job(job)
        self.send_json(200, {"job": response})

    def _download_job_file(self, job: Job, kind: str) -> None:
        with _JOBS_LOCK:
            path = job.preview_path if kind == "preview" else job.artifact_path
            ready, size = _file_info(path)
            if not ready or path is None:
                self._model_error(409, f"{kind}_not_ready", f"Model job {kind} is not ready.", True)
                return
            content_type = job.preview_content_type if kind == "preview" else {
                "obj": "model/obj",
                "3mf": "application/vnd.ms-package.3dmanufacturing-3dmodel+xml",
                "stl": "model/stl",
            }.get(job.artifact_format, "application/octet-stream")
            filename = None if kind == "preview" else f"orcaslicer-model-{job.id}.{job.artifact_format}"
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
