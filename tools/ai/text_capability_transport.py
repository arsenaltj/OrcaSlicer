from __future__ import annotations

import json
from email import policy
from email.parser import BytesParser
from typing import Any

from model_contracts import MAX_IMAGE_BYTES, MAX_REQUEST_BYTES, RequestError


def parse_text_capability_multipart(
    content_type: str,
    body: bytes,
    *,
    max_request_bytes: int = MAX_REQUEST_BYTES,
    max_image_bytes: int = MAX_IMAGE_BYTES,
) -> tuple[dict[str, Any], bytes | None]:
    media_type = content_type.split(";", 1)[0].strip().lower()
    if len(content_type) > 1024 or media_type != "multipart/form-data":
        raise RequestError(
            "unsupported_media_type",
            "Content-Type must be application/json or multipart/form-data.",
            415,
        )
    try:
        header = b"Content-Type: " + content_type.encode("latin-1") + b"\r\nMIME-Version: 1.0\r\n\r\n"
    except UnicodeEncodeError:
        raise RequestError("invalid_multipart", "Multipart Content-Type is invalid.", 400) from None
    message = BytesParser(policy=policy.default).parsebytes(header + body)
    if not message.is_multipart():
        raise RequestError("invalid_multipart", "Multipart request is malformed.", 400)

    fields: dict[str, bytes] = {}
    for part in message.iter_parts():
        if part.is_multipart() or part.get_content_disposition() != "form-data":
            raise RequestError("invalid_multipart", "Nested or invalid multipart data is not supported.", 400)
        name = part.get_param("name", header="content-disposition")
        if name not in {"request", "image"} or name in fields:
            raise RequestError("invalid_multipart", "Multipart fields are unexpected or duplicated.", 400)
        fields[name] = part.get_payload(decode=True) or b""

    request_payload = fields.get("request")
    image = fields.get("image")
    if request_payload is None:
        raise RequestError("invalid_request", "request is required.", 400)
    if len(request_payload) > max_request_bytes:
        raise RequestError("request_too_large", "Text capability request is too large.", 413)
    if image is not None and len(image) > max_image_bytes:
        raise RequestError("request_too_large", "Reference image exceeds 20 MB.", 413)
    try:
        request = json.loads(request_payload.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError):
        raise RequestError("invalid_json", "Text capability request contains malformed JSON.", 400) from None
    if not isinstance(request, dict):
        raise RequestError("invalid_request", "Text capability request must be a JSON object.", 400)
    return request, image
