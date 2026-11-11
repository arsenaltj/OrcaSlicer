"""Validated four-view source identity for version-2 model jobs."""
from __future__ import annotations

import hashlib
from pathlib import Path
from typing import Any

from advanced_multiview_contracts import CREATION_KIND, VIEWS, fingerprint, identity
from model_contracts import RequestError

SNAPSHOT_SCHEMA = "approved-multiview-input-v1"


def approved_paths(snapshot: Any, directory: Path, *, verify_files: bool = True) -> dict[str, Path]:
    try:
        if (not isinstance(snapshot, dict) or snapshot.get("schema") != SNAPSHOT_SCHEMA
                or set(snapshot.get("views", {})) != set(VIEWS)):
            raise ValueError()
        identity(snapshot["group_id"])
        identity(snapshot["snapshot_id"])
        content = {key: value for key, value in snapshot.items() if key != "fingerprint"}
        if fingerprint(content) != snapshot["fingerprint"]:
            raise ValueError()
        paths = {}
        root = directory.resolve()
        for view in VIEWS:
            record = snapshot["views"][view]
            identity(record["version_id"])
            if record["path"] != f"inputs/{view}.png":
                raise ValueError()
            digest = record["sha256"]
            if not isinstance(digest, str) or len(digest) != 64 or any(c not in "0123456789abcdef" for c in digest):
                raise ValueError()
            path = (directory / record["path"]).resolve()
            path.relative_to(root)
            if verify_files and (not path.is_file() or path.stat().st_size > 20 * 1024 * 1024
                                 or hashlib.sha256(path.read_bytes()).hexdigest() != digest):
                raise ValueError()
            paths[view] = path
        return paths
    except (ValueError, KeyError, TypeError, OSError, RequestError):
        raise RequestError("invalid_multiview_snapshot", "The approved four-view model inputs are unavailable or changed.", 409) from None
