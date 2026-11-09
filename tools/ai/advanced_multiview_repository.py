"""Single-writer group storage; immutable images and attempts never become legacy Jobs."""
from __future__ import annotations

from contextlib import contextmanager
from copy import deepcopy
import hashlib
from io import BytesIO
import json
import math
import os
from pathlib import Path
import threading
import time
from typing import Any, Iterator, Mapping

from PIL import Image, ImageOps

from advanced_multiview_contracts import (
    CREATION_KIND, GROUP_SCHEMA, MAX_GROUP_BYTES, MAX_RECORD_BYTES, VERSION_SCHEMA,
    VIEWS, display_name, draft_fields, fingerprint, identity, new_id,
)
from model_contracts import MAX_IMAGE_BYTES, MIN_SOURCE_IMAGE_EDGE, RequestError
from model_request import _validate_image_data


class AdvancedMultiviewRepository:
    def __init__(self, models_root: Path):
        self.root = Path(models_root).resolve() / "advanced-multiview"
        self._lock = threading.RLock()
        self._depth = threading.local()

    @contextmanager
    def transaction(self) -> Iterator[None]:
        """One shared repository per sidecar; an OS lock survives no crashed writer."""
        with self._lock:
            depth = getattr(self._depth, "value", 0)
            if depth:
                self._depth.value = depth + 1
                try:
                    yield
                finally:
                    self._depth.value -= 1
                return
            self.root.mkdir(parents=True, exist_ok=True)
            lock_path = self.root / ".writer.lock"
            descriptor = None
            try:
                descriptor = lock_path.open("a+b")
                if descriptor.tell() == 0:
                    descriptor.write(b"0")
                    descriptor.flush()
                descriptor.seek(0)
                if os.name == "nt":
                    import msvcrt
                    msvcrt.locking(descriptor.fileno(), msvcrt.LK_NBLCK, 1)
                else:
                    import fcntl
                    fcntl.flock(descriptor.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
            except OSError:
                if descriptor is not None:
                    descriptor.close()
                raise RequestError("storage_busy", "The multiview store is locked or not writable.", 409) from None
            self._depth.value = 1
            try:
                yield
            finally:
                self._depth.value = 0
                descriptor.seek(0)
                if os.name == "nt":
                    import msvcrt
                    msvcrt.locking(descriptor.fileno(), msvcrt.LK_UNLCK, 1)
                else:
                    import fcntl
                    fcntl.flock(descriptor.fileno(), fcntl.LOCK_UN)
                descriptor.close()

    def directory(self, group_id: str) -> Path:
        directory = self.root / identity(group_id, "group_id")
        if directory.resolve() != directory:
            raise RequestError("invalid_asset", "A linked group directory is not allowed.", 409)
        return directory

    def path(self, group_id: str, relative: str, *, must_exist: bool = True) -> Path:
        directory = self.directory(group_id)
        if not isinstance(relative, str) or not relative or Path(relative).is_absolute():
            raise RequestError("invalid_asset", "An in-group asset reference is required.", 409)
        try:
            result = (directory / relative).resolve(strict=must_exist)
            result.relative_to(directory)
            if must_exist and not result.is_file():
                raise ValueError()
            return result
        except (OSError, ValueError):
            raise RequestError("snapshot_unavailable", "The referenced group asset is unavailable.", 409) from None

    @staticmethod
    def atomic_json(path: Path, value: Any, maximum: int = MAX_RECORD_BYTES) -> None:
        try:
            encoded = json.dumps(value, ensure_ascii=False, sort_keys=True, allow_nan=False).encode("utf-8")
            if len(encoded) > maximum:
                raise ValueError("size")
            path.parent.mkdir(parents=True, exist_ok=True)
            temporary = path.with_name(path.name + "." + new_id() + ".part")
            try:
                with temporary.open("xb") as output:
                    output.write(encoded)
                    output.flush()
                    os.fsync(output.fileno())
                os.replace(temporary, path)
            finally:
                temporary.unlink(missing_ok=True)
        except (OSError, ValueError, TypeError):
            raise RequestError("persistence_failed", "The multiview record could not be saved.", 503) from None

    @staticmethod
    def read_json(path: Path, maximum: int = MAX_RECORD_BYTES) -> dict[str, Any]:
        try:
            with path.open("rb") as source:
                encoded = source.read(maximum + 1)
            if len(encoded) > maximum:
                raise ValueError()
            result = json.loads(encoded, parse_constant=lambda _: None)
            if not isinstance(result, dict):
                raise ValueError()
            return result
        except (OSError, UnicodeError, ValueError):
            raise RequestError("snapshot_unavailable", "The saved multiview record is unreadable.", 409) from None

    def load(self, group_id: str) -> dict[str, Any]:
        path = self.directory(group_id) / "group.json"
        if not path.exists():
            raise RequestError("group_not_found", "The multiview group was not found.", 404)
        group = self.read_json(path, MAX_GROUP_BYTES)
        # These fields were added without changing the on-disk schema so existing
        # advanced groups remain readable and acquire no work until edited.
        group.setdefault("pending_views", [])
        group.setdefault("changed_upload_views", [])
        if (group.get("schema") != GROUP_SCHEMA or group.get("id") != group_id
                or group.get("workflow_kind") != CREATION_KIND
                or not isinstance(group.get("revision"), int) or isinstance(group.get("revision"), bool)
                or not isinstance(group.get("views"), dict) or set(group["views"]) != set(VIEWS)
                or not isinstance(group.get("uploads"), dict) or set(group["uploads"]) != set(VIEWS)):
            raise RequestError("snapshot_unavailable", "The multiview group schema is not supported.", 409)
        try:
            identity(group["input_epoch"])
            if group["active_operation_id"] is not None:
                identity(group["active_operation_id"])
            for key in ("initial_input", "saved_draft"):
                draft_fields(group[key])
            display_name(group.get("display_name", ""))
            if (group["revision"] < 0 or group["source"] not in {"text", "image"}
                    or any(type(group[key]) is not bool for key in ("published", "stop_intent"))
                    or not isinstance(group["model_links"], list)
                    or not isinstance(group["pending_views"], list)
                    or not isinstance(group["changed_upload_views"], list)
                    or any(view not in VIEWS for view in group["pending_views"])
                    or any(view not in VIEWS for view in group["changed_upload_views"])
                    or len(group["pending_views"]) != len(set(group["pending_views"]))
                    or len(group["changed_upload_views"]) != len(set(group["changed_upload_views"]))
                    or not set(group["changed_upload_views"]).issubset(group["pending_views"])
                    or any(type(group[key]) not in (int, float) or not math.isfinite(group[key])
                           for key in ("created_at", "updated_at"))):
                raise ValueError()
            for view in VIEWS:
                state = group["views"][view]
                if state["current_version_id"] is not None:
                    identity(state["current_version_id"])
                attempt = state["latest_attempt"]
                if attempt is not None:
                    identity(attempt["operation_id"])
                    identity(attempt["attempt_id"])
                    if not isinstance(attempt["state"], str):
                        raise ValueError()
                upload = group["uploads"][view]
                if upload is not None:
                    identity(upload["asset_id"])
                    if (upload["view"] != view or not isinstance(upload["path"], str)
                            or not isinstance(upload["normalized_path"], str)
                            or any(not isinstance(upload[key], str) or len(upload[key]) != 64
                                   for key in ("sha256", "normalized_sha256"))):
                        raise ValueError()
        except (RequestError, ValueError, TypeError, KeyError):
            raise RequestError("snapshot_unavailable", "The multiview group record is incomplete or invalid.", 409) from None
        return group

    def save(self, group: dict[str, Any]) -> dict[str, Any]:
        """Commit first; return a new revision only after successful durable replacement."""
        with self.transaction():
            previous = self.load(group["id"])
            if previous["revision"] != group["revision"]:
                raise RequestError("revision_conflict", "The multiview group changed.", 409)
            updated = deepcopy(group)
            updated["revision"] += 1
            updated["updated_at"] = time.time()
            self.atomic_json(self.directory(group["id"]) / "group.json", updated, MAX_GROUP_BYTES)
            return updated

    @staticmethod
    def _validate_images(images: Mapping[str, bytes]) -> dict[str, Any]:
        if not isinstance(images, Mapping) or set(images) - set(VIEWS):
            raise RequestError("invalid_request", "Unsupported upload view.", 400)
        validated = {}
        for view, data in images.items():
            try:
                if not isinstance(data, bytes):
                    raise ValueError("Image bytes are required.")
                info = _validate_image_data(data, minimum_edge=MIN_SOURCE_IMAGE_EDGE)
                if min(info.width, info.height) < MIN_SOURCE_IMAGE_EDGE:
                    raise ValueError("Source images must be at least 64 pixels on each edge.")
                with Image.open(BytesIO(data)) as opened:
                    if getattr(opened, "is_animated", False):
                        raise ValueError("Only static images are supported.")
                validated[view] = info
            except ValueError as error:
                raise RequestError("invalid_image", str(error), 400) from None
        return validated

    def _store_upload(self, group_id: str, view: str, data: bytes, info: Any) -> dict[str, Any]:
        directory = self.directory(group_id)
        asset_id = new_id()
        extension = {"image/png": "png", "image/jpeg": "jpg", "image/webp": "webp"}[info.content_type]
        relative = f"uploads/{asset_id}/original.{extension}"
        original = self.path(group_id, relative, must_exist=False)
        original.parent.mkdir(parents=True, exist_ok=True)
        try:
            with original.open("xb") as output:
                output.write(data)
            normalized = original.with_name("normalized.png")
            with Image.open(BytesIO(data)) as opened:
                ImageOps.exif_transpose(opened).convert("RGBA").save(normalized)
        except (OSError, ValueError):
            raise RequestError("persistence_failed", "The uploaded reference could not be preserved.", 503) from None
        return {"asset_id": asset_id, "view": view, "path": relative,
                "normalized_path": normalized.relative_to(directory).as_posix(),
                "normalized_sha256": hashlib.sha256(normalized.read_bytes()).hexdigest(),
                "sha256": hashlib.sha256(data).hexdigest(), "content_type": info.content_type,
                "width": info.width, "height": info.height, "bytes": len(data)}

    def replace_inputs(self, group: dict[str, Any], draft: Mapping[str, Any],
                       images: Mapping[str, bytes]) -> dict[str, Any]:
        """Replace current upload pointers while preserving every prior asset and design version."""
        validated = self._validate_images(images)
        if images and "front" not in images:
            raise RequestError("missing_front", "A front reference is required when uploading images.", 400)
        if not images and not draft["prompt"]:
            raise RequestError("invalid_request", "An image or description is required.", 400)

        uploads: dict[str, Any] = dict.fromkeys(VIEWS)
        changed = []
        for view in VIEWS:
            data = images.get(view)
            previous = group["uploads"].get(view)
            if data is None:
                if previous is not None:
                    changed.append(view)
                continue
            digest = hashlib.sha256(data).hexdigest()
            if previous is not None and previous["sha256"] == digest:
                uploads[view] = previous
            else:
                uploads[view] = self._store_upload(group["id"], view, data, validated[view])
                changed.append(view)

        style_changed = (draft["style"], draft["custom_style"]) != (
            group["saved_draft"]["style"], group["saved_draft"]["custom_style"])
        draft_changed = draft != group["saved_draft"]
        if not changed and not draft_changed:
            return group

        group["source"] = "image" if images else "text"
        group["uploads"] = uploads
        group["saved_draft"] = dict(draft)
        if changed or style_changed:
            group["input_epoch"] = new_id()
            pending = set(group["pending_views"])
            pending.update(changed)
            if style_changed:
                pending.update(VIEWS)
            changed_uploads = set(group["changed_upload_views"])
            changed_uploads.update(changed)
            group["pending_views"] = [view for view in VIEWS if view in pending]
            group["changed_upload_views"] = [view for view in VIEWS if view in changed_uploads]
        group["input_fingerprint"] = fingerprint({
            "draft": draft, "source": group["source"],
            "uploads": {view: uploads[view]["sha256"] for view in VIEWS if uploads[view]},
        })
        return self.save(group)

    def create(self, request_id: str, fields: Mapping[str, Any],
               images: Mapping[str, bytes]) -> dict[str, Any]:
        group_id = identity(request_id, "request_id")
        draft = draft_fields(fields)
        validated = self._validate_images(images)
        if images and "front" not in images:
            raise RequestError("missing_front", "A front reference is required when uploading images.", 400)
        if not images and not draft["prompt"]:
            raise RequestError("invalid_request", "An image or description is required.", 400)
        source = "image" if images else "text"
        if fields.get("source", source) != source:
            raise RequestError("invalid_request", "The source does not match the uploaded references.", 400)
        input_hash = fingerprint({"draft": draft, "source": source,
                                  "uploads": {v: hashlib.sha256(d).hexdigest() for v, d in images.items()}})
        with self.transaction():
            directory = self.directory(group_id)
            if (directory / "group.json").exists():
                existing = self.load(group_id)
                if existing["input_fingerprint"] != input_hash:
                    raise RequestError("idempotency_conflict", "The request ID belongs to other inputs.", 409)
                return existing
            directory.mkdir(exist_ok=True)
            uploads: dict[str, Any] = dict.fromkeys(VIEWS)
            for view, data in images.items():
                uploads[view] = self._store_upload(group_id, view, data, validated[view])
            now = time.time()
            group = {"schema": GROUP_SCHEMA, "workflow_kind": CREATION_KIND, "id": group_id,
                     "revision": 0, "input_epoch": new_id(), "input_fingerprint": input_hash,
                     "display_name": "",
                     "source": source, "uploads": uploads, "initial_input": draft,
                     "saved_draft": draft, "latest_submitted_draft": None,
                     "pending_views": [], "changed_upload_views": [],
                     "views": {v: {"current_version_id": None, "latest_attempt": None} for v in VIEWS},
                     "active_operation_id": None, "stop_intent": False, "model_links": [],
                     "created_at": now, "updated_at": now, "published": False}
            self.atomic_json(directory / "group.json", group, MAX_GROUP_BYTES)
            return group

    def write_record(self, group_id: str, relative: str, record: dict[str, Any]) -> None:
        with self.transaction():
            self.atomic_json(self.path(group_id, relative, must_exist=False), record)

    def read_record(self, group_id: str, relative: str) -> dict[str, Any]:
        return self.read_json(self.path(group_id, relative))

    def version(self, group_id: str, view: str, version_id: str) -> dict[str, Any]:
        if view not in VIEWS:
            raise RequestError("invalid_request", "Unsupported view.", 400)
        identity(version_id, "version_id")
        record = self.read_record(group_id, f"designs/{view}/{version_id}/version.json")
        if record.get("schema") != VERSION_SCHEMA or record.get("view") != view or record.get("id") != version_id:
            raise RequestError("snapshot_unavailable", "The design version identity is invalid.", 409)
        image = self.path(group_id, record.get("path"))
        if image.stat().st_size > MAX_IMAGE_BYTES or hashlib.sha256(image.read_bytes()).hexdigest() != record.get("sha256"):
            raise RequestError("snapshot_unavailable", "The design image content changed.", 409)
        return record

    def snapshot(self, group_id: str) -> dict[str, Any]:
        with self._lock:
            group = self.load(group_id)
            for view in VIEWS:
                state = group["views"][view]
                version_id = state["current_version_id"]
                state["current"] = None
                state["unavailable_reason"] = ""
                if version_id:
                    try:
                        state["current"] = self.version(group_id, view, version_id)
                    except RequestError as error:
                        state["unavailable_reason"] = error.code
            group["all_views_available"] = all(group["views"][v]["current"] for v in VIEWS)
            return group

    def list_groups(self, *, before: float | None = None, limit: int = 24) -> list[dict[str, Any]]:
        if isinstance(limit, bool) or not isinstance(limit, int) or not 1 <= limit <= 100:
            raise RequestError("invalid_request", "The page limit must be between 1 and 100.", 400)
        if before is not None and (type(before) not in (float, int) or not math.isfinite(before)):
            raise RequestError("invalid_request", "The history cursor must be finite.", 400)
        entries = []
        if not self.root.exists():
            return entries
        with self._lock:
            for directory in self.root.iterdir():
                if not directory.is_dir():
                    continue
                try:
                    group = self.load(directory.name)
                    if group["published"] and (before is None or group["created_at"] < before):
                        entry = {key: group[key] for key in
                                 ("id", "revision", "created_at", "updated_at", "saved_draft", "views")}
                        entry["display_name"] = group.get("display_name", "")
                        entries.append(entry)
                except RequestError:
                    continue  # Keep damaged originals; opening by ID reports the precise error.
        return sorted(entries, key=lambda g: (g["created_at"], g["id"]), reverse=True)[:limit]
