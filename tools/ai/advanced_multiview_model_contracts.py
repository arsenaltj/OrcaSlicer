"""Validated named input identity for advanced version-2 model jobs."""
from __future__ import annotations

import hashlib
from pathlib import Path
from typing import Any

from advanced_multiview_contracts import CREATION_KIND, VIEWS, fingerprint, identity
from model_contracts import RequestError

LEGACY_SNAPSHOT_SCHEMA = "approved-multiview-input-v1"
SNAPSHOT_SCHEMA = "approved-multiview-input-v2"


def supports_model_input(capability: dict[str, Any], count: int) -> bool:
    if not capability.get("available"):
        return False
    counts = capability.get("supported_image_counts")
    if isinstance(counts, (list, tuple)):
        return count in counts
    # An old four-view capability does not prove single/two/three-view support.
    return count == 4 and capability.get("named_four_view_model") == "supported"


def approved_paths(snapshot: Any, directory: Path, *, verify_files: bool = True) -> dict[str, Path]:
    try:
        if not isinstance(snapshot, dict) or not isinstance(snapshot.get("views"), dict):
            raise ValueError()
        views = tuple(view for view in VIEWS if view in snapshot["views"])
        if snapshot.get("schema") == LEGACY_SNAPSHOT_SCHEMA:
            if set(snapshot["views"]) != set(VIEWS):
                raise ValueError()
        elif snapshot.get("schema") == SNAPSHOT_SCHEMA:
            if (not views or views[0] != "front" or set(views) != set(snapshot["views"])
                    or snapshot.get("selected_views") != list(views)
                    or snapshot.get("input_mode") != ("image" if len(views) == 1 else "multiview")):
                raise ValueError()
            identity(snapshot["input_epoch"])
        else:
            raise ValueError()
        identity(snapshot["group_id"])
        identity(snapshot["snapshot_id"])
        content = {key: value for key, value in snapshot.items() if key != "fingerprint"}
        if fingerprint(content) != snapshot["fingerprint"]:
            raise ValueError()
        paths = {}
        root = directory.resolve()
        for view in views:
            record = snapshot["views"][view]
            identity(record["version_id"])
            if record["path"] != f"inputs/{view}.png":
                raise ValueError()
            digest = record["sha256"]
            if not isinstance(digest, str) or len(digest) != 64 or any(c not in "0123456789abcdef" for c in digest):
                raise ValueError()
            path = (directory / record["path"]).resolve()
            path.relative_to(root)
            if verify_files and (not path.is_file() or not 0 < path.stat().st_size <= 20 * 1024 * 1024
                                 or hashlib.sha256(path.read_bytes()).hexdigest() != digest):
                raise ValueError()
            paths[view] = path
        return paths
    except (ValueError, KeyError, TypeError, OSError, RequestError):
        raise RequestError("invalid_multiview_snapshot", "The approved model inputs are unavailable or changed.", 409) from None
