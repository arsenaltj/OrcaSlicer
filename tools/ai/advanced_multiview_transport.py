"""Authenticated advanced HTTP adaptation; generation policy stays in Application."""
from __future__ import annotations

from email import policy
from email.parser import BytesParser
import json
from pathlib import Path
import re
import tempfile
from typing import Any, BinaryIO
from urllib.parse import parse_qs, urlsplit

from advanced_multiview_contracts import VIEWS, identity
from model_contracts import MAX_IMAGE_BYTES, MAX_REQUEST_BYTES, RequestError

PREFIX = "/v1/orcaslicer/advanced-multiview"
MAX_MULTIVIEW_MULTIPART_BYTES = 4 * MAX_IMAGE_BYTES + MAX_REQUEST_BYTES


class MultipartReader:
    """Bounded streaming parser; never duplicates the whole 80 MiB multipart envelope."""
    def __init__(self, stream: BinaryIO, length: int, boundary: bytes):
        self.stream, self.remaining = stream, length
        self.buffer = b""
        self.boundary = boundary

    def _fill(self) -> bool:
        if self.remaining <= 0:
            return False
        data = self.stream.read(min(64 * 1024, self.remaining))
        if not data:
            raise RequestError("invalid_multipart", "The multipart request is incomplete.", 400)
        self.remaining -= len(data)
        self.buffer += data
        return True

    def line(self, maximum: int = 16 * 1024) -> bytes:
        while b"\r\n" not in self.buffer:
            if len(self.buffer) > maximum or not self._fill():
                raise RequestError("invalid_multipart", "Malformed multipart headers.", 400)
        index = self.buffer.index(b"\r\n")
        if index > maximum:
            raise RequestError("invalid_multipart", "Multipart headers exceed their limit.", 400)
        line, self.buffer = self.buffer[:index], self.buffer[index + 2:]
        return line

    def part(self, maximum: int) -> tuple[bytes, bool]:
        delimiter = b"\r\n--" + self.boundary
        size = 0
        with tempfile.SpooledTemporaryFile(max_size=1024 * 1024) as output:
            while True:
                index = self.buffer.find(delimiter)
                if index >= 0:
                    # Wait for the suffix; boundary-like bytes inside images are payload.
                    required = index + len(delimiter) + 2
                    if len(self.buffer) < required:
                        if not self._fill():
                            raise RequestError("invalid_multipart", "Incomplete multipart boundary.", 400)
                        continue
                    suffix = self.buffer[index + len(delimiter):required]
                    if suffix in {b"--", b"\r\n"}:
                        output.write(self.buffer[:index])
                        size += index
                        self.buffer = self.buffer[required:]
                        if size > maximum:
                            raise RequestError("request_too_large", "An upload field exceeds its limit.", 413)
                        output.seek(0)
                        return output.read(), suffix == b"--"
                    # Not a delimiter: consume enough bytes to make forward progress.
                    take = index + 2
                else:
                    take = max(0, len(self.buffer) - len(delimiter) - 2)
                if take:
                    output.write(self.buffer[:take])
                    self.buffer = self.buffer[take:]
                    size += take
                    if size > maximum:
                        raise RequestError("request_too_large", "An upload field exceeds its limit.", 413)
                if not self._fill():
                    raise RequestError("invalid_multipart", "The final multipart boundary is missing.", 400)


def parse_upload(stream: BinaryIO, content_type: str, length: int) -> tuple[dict[str, Any], dict[str, bytes]]:
    if len(content_type) > 1024 or length < 0:
        raise RequestError("invalid_multipart", "Invalid multipart metadata.", 400)
    if length > MAX_MULTIVIEW_MULTIPART_BYTES:
        raise RequestError("request_too_large", "The four-view upload exceeds its limit.", 413)
    try:
        header = BytesParser(policy=policy.default).parsebytes(("Content-Type: " + content_type + "\r\n\r\n").encode("ascii"))
        boundary = header.get_boundary()
        if header.get_content_type() != "multipart/form-data" or not boundary or not re.fullmatch(r"[A-Za-z0-9'()+_,./:=?-]{1,70}", boundary):
            raise ValueError()
        boundary_bytes = boundary.encode("ascii")
    except (ValueError, UnicodeError):
        raise RequestError("invalid_multipart", "A valid multipart boundary is required.", 400) from None
    reader = MultipartReader(stream, length, boundary_bytes)
    if reader.line() != b"--" + boundary_bytes:
        raise RequestError("invalid_multipart", "The initial upload boundary is invalid.", 400)
    fields: dict[str, Any] = {}
    images: dict[str, bytes] = {}
    allowed = {"request_id", "expected_group_revision", "source", "prompt", "style", "custom_style",
               "design_provider", "design_model"}
    header_bytes = 0
    field_bytes = 0
    seen = set()
    while True:
        headers = bytearray()
        while True:
            line = reader.line()
            header_bytes += len(line) + 2
            if header_bytes > 16 * 1024:
                raise RequestError("invalid_multipart", "Multipart headers exceed their limit.", 400)
            if not line:
                break
            headers.extend(line + b"\r\n")
        part = BytesParser(policy=policy.default).parsebytes(bytes(headers) + b"\r\n")
        name = part.get_param("name", header="Content-Disposition")
        if part.get_content_disposition() != "form-data" or not isinstance(name, str) or name in seen:
            raise RequestError("invalid_multipart", "Duplicate or invalid upload field.", 400)
        seen.add(name)
        view = name[:-6] if name.endswith("_image") else None
        if view not in VIEWS and name not in allowed:
            raise RequestError("invalid_multipart", "An unsupported upload field was supplied.", 400)
        payload, final = reader.part(MAX_IMAGE_BYTES if view in VIEWS else MAX_REQUEST_BYTES)
        if view in VIEWS:
            if part.get_filename() is None:
                raise RequestError("invalid_multipart", "Image slots require file parts.", 400)
            images[view] = payload
        else:
            field_bytes += len(payload)
            if field_bytes > MAX_REQUEST_BYTES or part.get_filename() is not None:
                raise RequestError("invalid_multipart", "Upload metadata exceeds its limit.", 400)
            try:
                fields[name] = payload.decode("utf-8")
            except UnicodeError:
                raise RequestError("invalid_multipart", "Upload text must be UTF-8.", 400) from None
        if final:
            while reader._fill():
                if len(reader.buffer) > 2:
                    raise RequestError("invalid_multipart", "Unexpected multipart epilogue.", 400)
            if reader.buffer not in {b"", b"\r\n"}:
                raise RequestError("invalid_multipart", "Unexpected multipart epilogue.", 400)
            return fields, images


def dispatch(handler: Any, runtime: Any, method: str) -> bool:
    route = urlsplit(handler.path)
    if not (route.path == PREFIX or route.path.startswith(PREFIX + "/")):
        return False
    if not handler._require_native_client():
        return True
    try:
        if callable(runtime):
            runtime = runtime()
        parts = route.path[len(PREFIX):].strip("/").split("/")
        application, repository = runtime.application, runtime.repository
        if method == "GET" and parts == ["capabilities"]:
            handler.send_json(200, runtime.capabilities())
        elif parts == ["groups"] and method == "POST":
            content_type = handler.headers.get("Content-Type", "")
            if content_type.startswith("multipart/form-data"):
                try:
                    length = int(handler.headers.get("Content-Length", "-1"))
                except ValueError:
                    raise RequestError("invalid_request", "Content-Length is invalid.", 400) from None
                fields, images = parse_upload(handler.rfile, content_type, length)
            else:
                fields, images = handler._read_model_json(), {}
            group = repository.create(fields.get("request_id"), fields, images)
            handler.send_json(201, {"group": group})
        elif parts == ["groups"] and method == "GET":
            query = parse_qs(route.query)
            try:
                before = float(query["before"][0]) if "before" in query else None
                limit = int(query.get("limit", ["24"])[0])
            except ValueError:
                raise RequestError("invalid_request", "Invalid history page parameters.", 400) from None
            handler.send_json(200, {"groups": repository.list_groups(before=before, limit=limit)})
        elif len(parts) >= 2 and parts[0] == "groups":
            group_id = identity(parts[1], "group_id")
            if len(parts) == 2 and method == "GET":
                handler.send_json(200, {"group": runtime.snapshot(group_id)})
            elif len(parts) == 3 and parts[2] == "inputs" and method == "POST":
                content_type = handler.headers.get("Content-Type", "")
                if not content_type.startswith("multipart/form-data"):
                    raise RequestError("invalid_multipart", "Updated multiview inputs require multipart data.", 400)
                try:
                    length = int(handler.headers.get("Content-Length", "-1"))
                except ValueError:
                    raise RequestError("invalid_request", "Content-Length is invalid.", 400) from None
                fields, images = parse_upload(handler.rfile, content_type, length)
                try:
                    fields["expected_group_revision"] = int(fields.get("expected_group_revision", ""))
                except ValueError:
                    raise RequestError("invalid_request", "expected_group_revision is invalid.", 400) from None
                application.update_inputs(group_id, fields, images)
                handler.send_json(200, {"group": runtime.snapshot(group_id)})
            elif len(parts) == 3 and parts[2] == "model-plans" and method == "POST" and runtime.models:
                handler.send_json(200, {"plan": runtime.models.prepare(group_id, handler._read_model_json())})
            elif len(parts) == 3 and parts[2] == "model-jobs" and method == "POST" and runtime.models:
                job = runtime.models.execute(group_id, handler._read_model_json())
                handler.send_json(202, {"job": runtime.models.ports.present(job)})
            elif len(parts) == 5 and parts[2] == "model-jobs" and parts[4] == "resume" and method == "POST" and runtime.models:
                handler._read_model_json()
                job = runtime.models.resume(group_id, parts[3])
                handler.send_json(202, {"job": runtime.models.ports.present(job)})
            elif len(parts) == 5 and parts[2] == "model-jobs" and parts[4] == "reconcile" and method == "POST" and runtime.models:
                handler._read_model_json()
                response = runtime.models.reconcile(group_id, parts[3])
                response["group"] = runtime.snapshot(group_id)
                handler.send_json(200, response)
            elif len(parts) == 5 and parts[2] == "model-jobs" and parts[4] == "stop" and method == "POST" and runtime.models:
                handler._read_model_json()
                job = runtime.models.stop(group_id, parts[3])
                handler.send_json(200, {"job": runtime.models.ports.present(job)})
            elif len(parts) == 3 and parts[2] == "draft" and method == "PUT":
                application.save_draft(group_id, handler._read_model_json())
                handler.send_json(200, {"group": runtime.snapshot(group_id)})
            elif len(parts) == 3 and parts[2] == "name" and method == "PUT":
                application.rename_group(group_id, handler._read_model_json())
                handler.send_json(200, {"group": runtime.snapshot(group_id)})
            elif len(parts) == 3 and parts[2] == "plans" and method == "POST":
                handler.send_json(200, {"plan": application.prepare(group_id, handler._read_model_json())})
            elif len(parts) == 3 and parts[2] == "operations" and method == "POST":
                handler.send_json(202, {"operation": application.execute(group_id, handler._read_model_json())})
            elif len(parts) == 4 and parts[2] == "operations" and method == "GET":
                handler.send_json(200, {"operation": application.operation(group_id, parts[3])})
            elif len(parts) == 5 and parts[2] == "operations" and parts[4] == "stop" and method == "POST":
                handler._read_model_json()
                handler.send_json(200, {"operation": application.stop(group_id, parts[3])})
            elif len(parts) == 5 and parts[2] == "operations" and parts[4] == "recover" and method == "POST":
                handler._read_model_json()
                handler.send_json(200, {"operation": runtime.recover(group_id, parts[3])})
            elif len(parts) == 5 and parts[2] == "operations" and parts[4] == "reconcile" and method == "POST":
                handler._read_model_json()
                response = runtime.reconcile(group_id, parts[3])
                response["group"] = runtime.snapshot(group_id)
                handler.send_json(200, response)
            elif len(parts) == 4 and parts[2] == "assets" and method == "GET":
                asset_id = identity(parts[3], "asset_id")
                group = repository.snapshot(group_id)
                record = next((item for item in group["uploads"].values() if item and item["asset_id"] == asset_id), None)
                if record is None:
                    record = next((item["current"] for item in group["views"].values()
                                   if item["current"] and item["current"]["id"] == asset_id), None)
                if record is None:
                    raise RequestError("asset_not_found", "The group asset was not found.", 404)
                path = repository.path(group_id, record["path"])
                with path.open("rb") as stream:
                    handler.send_bytes(stream, path.stat().st_size, record.get("content_type", "image/png"))
            else:
                raise RequestError("not_found", "Advanced route not found.", 404)
        else:
            raise RequestError("not_found", "Advanced route not found.", 404)
    except RequestError as error:
        handler._model_error(error.status, error.code, error.message, error.retryable)
    return True
