from __future__ import annotations

import base64
import binascii
import ipaddress
import json
import os
import socket
import ssl
import urllib.error
import urllib.parse
import urllib.request
import uuid
from pathlib import Path
from typing import Any

_DEFAULT_BASE_URL = "https://api.openai.com/v1"
_MAX_JSON_BYTES = 32 * 1024 * 1024
_MAX_IMAGE_BYTES = 20 * 1024 * 1024
_TIMEOUT_SECONDS = 120


class OpenAIPreprocessorError(RuntimeError):
    """An OpenAI-compatible preprocessing request failed safely."""


def _config() -> tuple[str, str, str, str]:
    key = os.environ.get("OPENAI_API_KEY", "")
    base = os.environ.get("OPENAI_BASE_URL", _DEFAULT_BASE_URL).rstrip("/")
    if not key:
        raise OpenAIPreprocessorError("OPENAI_API_KEY is not configured.")
    parsed = urllib.parse.urlsplit(base)
    if parsed.scheme.lower() != "https" or not parsed.hostname or parsed.username or parsed.password:
        raise OpenAIPreprocessorError("OPENAI_BASE_URL must be a credential-free HTTPS URL.")
    return (
        base,
        key,
        os.environ.get("OPENAI_TEXT_MODEL", "gpt-5.4"),
        os.environ.get("OPENAI_IMAGE_MODEL", "gpt-image-2"),
    )


def _read_json_response(response: Any) -> dict[str, Any]:
    content_length = response.headers.get("Content-Length")
    if content_length:
        try:
            if int(content_length) > _MAX_JSON_BYTES:
                raise OpenAIPreprocessorError("The preprocessing service returned an oversized response.")
        except ValueError:
            pass
    raw = response.read(_MAX_JSON_BYTES + 1)
    if len(raw) > _MAX_JSON_BYTES:
        raise OpenAIPreprocessorError("The preprocessing service returned an oversized response.")
    try:
        value = json.loads(raw.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError):
        raise OpenAIPreprocessorError("The preprocessing service returned an invalid response.") from None
    if not isinstance(value, dict):
        raise OpenAIPreprocessorError("The preprocessing service returned an invalid response.")
    return value


def _provider_request(path: str, body: bytes, content_type: str) -> dict[str, Any]:
    base, key, _, _ = _config()
    request = urllib.request.Request(base + path, data=body, method="POST")
    request.add_header("Authorization", "Bearer " + key)
    request.add_header("Content-Type", content_type)
    request.add_header("Accept", "application/json")
    try:
        with urllib.request.build_opener(_RejectRedirects()).open(request, timeout=_TIMEOUT_SECONDS) as response:
            return _read_json_response(response)
    except OpenAIPreprocessorError:
        raise
    except urllib.error.HTTPError as exc:
        if exc.code == 429:
            message = "The preprocessing service is rate limiting requests; try again later."
        elif 400 <= exc.code < 500:
            message = "The preprocessing service rejected the request."
        else:
            message = "The preprocessing service is temporarily unavailable."
        raise OpenAIPreprocessorError(message) from None
    except (urllib.error.URLError, TimeoutError, OSError):
        raise OpenAIPreprocessorError("Could not connect to the preprocessing service.") from None


class _RejectRedirects(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req: Any, fp: Any, code: int, msg: str, headers: Any, newurl: str) -> None:
        return None


def _content_text(value: Any) -> str:
    if isinstance(value, str):
        return value
    if isinstance(value, list):
        return "".join(_content_text(part) for part in value)
    if isinstance(value, dict):
        for key in ("text", "output_text", "content"):
            if key in value:
                return _content_text(value[key])
    return ""


def complete_text(system_prompt: str, user_content: str) -> str:
    if not isinstance(system_prompt, str) or not system_prompt.strip():
        raise OpenAIPreprocessorError("A system prompt is required.")
    if not isinstance(user_content, str) or not user_content.strip():
        raise OpenAIPreprocessorError("User content is required.")
    _, _, model, _ = _config()
    payload = {
        "model": model,
        "messages": [
            {"role": "system", "content": system_prompt},
            {"role": "user", "content": user_content},
        ],
    }
    result = _provider_request(
        "/chat/completions",
        json.dumps(payload, ensure_ascii=False, separators=(",", ":")).encode("utf-8"),
        "application/json",
    )
    choices = result.get("choices")
    content = ""
    if isinstance(choices, list) and choices and isinstance(choices[0], dict):
        message = choices[0].get("message")
        content = _content_text(message.get("content") if isinstance(message, dict) else message)
    if not content:
        content = _content_text(result.get("output_text")) or _content_text(result.get("output"))
    if not content.strip():
        raise OpenAIPreprocessorError("The preprocessing service returned an empty response.")
    return content.strip()


def preprocess_text(instruction: str) -> str:
    if not isinstance(instruction, str) or not instruction.strip():
        raise OpenAIPreprocessorError("A text instruction is required.")
    content = complete_text(
        (
            "Rewrite the user's request as one concise prompt for a text-to-3D model. "
            "Describe a single watertight, printable object with stable geometry, a flat base, "
            "adequate wall thickness, and no unsupported details. Preserve the requested subject "
            "and style. Return only the prompt, without markdown."
        ),
        instruction,
    )
    prompt = content.strip()
    if prompt.startswith("```") and prompt.endswith("```"):
        lines = prompt.splitlines()
        prompt = "\n".join(lines[1:-1]).strip()
    if not prompt:
        raise OpenAIPreprocessorError("The preprocessing service returned an empty prompt.")
    return prompt


def _image_kind(path: Path) -> tuple[str, str]:
    try:
        with path.open("rb") as stream:
            signature = stream.read(16)
    except OSError:
        raise OpenAIPreprocessorError("The input image could not be read.") from None
    if signature.startswith(b"\x89PNG\r\n\x1a\n"):
        return "image/png", "png"
    if signature.startswith(b"\xff\xd8\xff"):
        return "image/jpeg", "jpg"
    raise OpenAIPreprocessorError("The input image must be PNG or JPEG.")


def _multipart_image(path: Path, instruction: str, model: str) -> tuple[bytes, str]:
    mime_type, extension = _image_kind(path)
    try:
        if path.stat().st_size > _MAX_IMAGE_BYTES:
            raise OpenAIPreprocessorError("The input image exceeds the 20 MB limit.")
        image = path.read_bytes()
    except OpenAIPreprocessorError:
        raise
    except OSError:
        raise OpenAIPreprocessorError("The input image could not be read.") from None
    boundary = "----OrcaAI" + uuid.uuid4().hex
    chunks: list[bytes] = []

    def field(name: str, value: str) -> None:
        chunks.extend(
            [
                f"--{boundary}\r\n".encode("ascii"),
                f'Content-Disposition: form-data; name="{name}"\r\n\r\n'.encode("ascii"),
                value.encode("utf-8"),
                b"\r\n",
            ]
        )

    field("model", model)
    field("prompt", instruction)
    chunks.extend(
        [
            f"--{boundary}\r\n".encode("ascii"),
            f'Content-Disposition: form-data; name="image"; filename="input.{extension}"\r\n'.encode("ascii"),
            f"Content-Type: {mime_type}\r\n\r\n".encode("ascii"),
            image,
            b"\r\n",
            f"--{boundary}--\r\n".encode("ascii"),
        ]
    )
    return b"".join(chunks), "multipart/form-data; boundary=" + boundary


def _validate_artifact_url(url: str) -> None:
    parsed = urllib.parse.urlsplit(url)
    if parsed.scheme.lower() != "https" or not parsed.hostname or parsed.username or parsed.password:
        raise OpenAIPreprocessorError("The preprocessing service returned an unsafe image location.")
    try:
        addresses = socket.getaddrinfo(parsed.hostname, parsed.port or 443, type=socket.SOCK_STREAM)
    except socket.gaierror:
        raise OpenAIPreprocessorError("The result image host could not be resolved.") from None
    if not addresses:
        raise OpenAIPreprocessorError("The result image host could not be resolved.")
    for address in addresses:
        try:
            ip = ipaddress.ip_address(address[4][0].split("%", 1)[0])
        except ValueError:
            raise OpenAIPreprocessorError("The preprocessing service returned an unsafe image location.") from None
        if not ip.is_global:
            raise OpenAIPreprocessorError("The preprocessing service returned an unsafe image location.")


class _SafeArtifactRedirects(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req: Any, fp: Any, code: int, msg: str, headers: Any, newurl: str) -> Any:
        _validate_artifact_url(newurl)
        return urllib.request.Request(newurl, headers={"Accept": "image/*"}, method="GET")


def _atomic_write(path: Path, data: bytes) -> Path:
    if len(data) > _MAX_IMAGE_BYTES:
        raise OpenAIPreprocessorError("The result image exceeds the 20 MB limit.")
    part = path.with_name(path.name + ".part")
    try:
        with part.open("wb") as stream:
            stream.write(data)
        os.replace(part, path)
    except OSError:
        try:
            part.unlink(missing_ok=True)
        except OSError:
            pass
        raise OpenAIPreprocessorError("The result image could not be written.") from None
    return path


def _download_image(url: str, output_path: Path) -> Path:
    _validate_artifact_url(url)
    request = urllib.request.Request(url, headers={"Accept": "image/*"}, method="GET")
    part = output_path.with_name(output_path.name + ".part")
    try:
        with urllib.request.build_opener(_SafeArtifactRedirects()).open(request, timeout=_TIMEOUT_SECONDS) as response:
            content_length = response.headers.get("Content-Length")
            if content_length:
                try:
                    if int(content_length) > _MAX_IMAGE_BYTES:
                        raise OpenAIPreprocessorError("The result image exceeds the 20 MB limit.")
                except ValueError:
                    pass
            total = 0
            with part.open("wb") as stream:
                while chunk := response.read(min(64 * 1024, _MAX_IMAGE_BYTES + 1 - total)):
                    total += len(chunk)
                    if total > _MAX_IMAGE_BYTES:
                        raise OpenAIPreprocessorError("The result image exceeds the 20 MB limit.")
                    stream.write(chunk)
        os.replace(part, output_path)
        return output_path
    except OpenAIPreprocessorError:
        try:
            part.unlink(missing_ok=True)
        except OSError:
            pass
        raise
    except (urllib.error.HTTPError, urllib.error.URLError, TimeoutError, OSError):
        try:
            part.unlink(missing_ok=True)
        except OSError:
            pass
        raise OpenAIPreprocessorError("The result image could not be downloaded.") from None


def preprocess_image(input_path: str | os.PathLike[str], instruction: str, output_path: str | os.PathLike[str]) -> Path:
    if not isinstance(instruction, str) or not instruction.strip():
        raise OpenAIPreprocessorError("An image-edit instruction is required.")
    source = Path(input_path)
    destination = Path(output_path)
    _, _, _, model = _config()
    body, content_type = _multipart_image(source, instruction, model)
    result = _provider_request("/images/edits", body, content_type)
    data = result.get("data")
    item = data[0] if isinstance(data, list) and data and isinstance(data[0], dict) else None
    if not item:
        raise OpenAIPreprocessorError("The preprocessing service returned no image.")
    encoded = item.get("b64_json")
    if isinstance(encoded, str) and encoded:
        try:
            image = base64.b64decode(encoded, validate=True)
        except (ValueError, binascii.Error):
            raise OpenAIPreprocessorError("The preprocessing service returned invalid image data.") from None
        return _atomic_write(destination, image)
    url = item.get("url")
    if isinstance(url, str) and url:
        return _download_image(url, destination)
    raise OpenAIPreprocessorError("The preprocessing service returned no usable image.")
