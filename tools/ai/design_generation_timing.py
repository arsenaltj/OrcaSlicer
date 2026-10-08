"""Local, bounded timing samples; never submit or retry a provider request."""
from __future__ import annotations

import hashlib
import json
import math
import os
import statistics
import threading
import time
from pathlib import Path
from typing import Any

_LOCK = threading.Lock()
_MAX_SECONDS = 7200
_MAX_AGE = 30 * 86400
_MAX_SAMPLES = 20
_MIN_SAMPLES = 3
_MAX_GROUPS = 64
_MAX_BYTES = 256 * 1024


def timing_key(provider: dict[str, Any], model: str, quality: str, source: str, style: str) -> str:
    if not provider.get("available"):
        return ""
    # Store only a fingerprint, never a service URL, credential or user prompt.
    values = [provider.get("source", ""), provider.get("base_url", ""), model, quality, source, style]
    return hashlib.sha256(json.dumps(values, ensure_ascii=True).encode()).hexdigest()


def seconds(value: Any) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return 0.0
    return float(value) if 0 < value <= _MAX_SECONDS and math.isfinite(value) else 0.0


class DesignTimingHistory:
    def __init__(self, root: Path):
        self.path = root / "design-generation-times.json"

    def _read(self, now: float) -> dict[str, list[list[float]]]:
        try:
            if self.path.stat().st_size > _MAX_BYTES:
                return {}
            payload = json.loads(self.path.read_text(encoding="utf-8"))
            if not isinstance(payload, dict) or payload.get("version") != 1:
                return {}
            groups = payload.get("groups")
            if not isinstance(groups, dict):
                return {}
            valid = {}
            for key, rows in groups.items():
                if not isinstance(key, str) or len(key) != 64 or any(c not in "0123456789abcdef" for c in key):
                    continue
                if not isinstance(rows, list):
                    continue
                samples = []
                for row in rows[-_MAX_SAMPLES:]:
                    if not isinstance(row, list) or len(row) != 2:
                        continue
                    stamp, duration = row
                    if (isinstance(stamp, (int, float)) and not isinstance(stamp, bool)
                            and now - _MAX_AGE <= stamp <= now and math.isfinite(stamp)
                            and seconds(duration)):
                        samples.append([float(stamp), float(duration)])
                if samples:
                    valid[key] = samples
            return dict(sorted(valid.items(), key=lambda item: item[1][-1][0])[-_MAX_GROUPS:])
        except (OSError, ValueError, TypeError):
            return {}

    def estimate(self, key: str, now: float | None = None) -> int:
        if not key:
            return 0
        with _LOCK:
            rows = self._read(time.time() if now is None else now).get(key, [])
        if len(rows) < _MIN_SAMPLES:
            return 0
        return math.ceil(statistics.median(row[1] for row in rows))

    def record(self, key: str, duration: float, now: float | None = None) -> bool:
        if len(key) != 64 or any(c not in "0123456789abcdef" for c in key) or not seconds(duration):
            return False
        now = time.time() if now is None else now
        with _LOCK:
            groups = self._read(now)
            groups[key] = (groups.get(key, []) + [[now, duration]])[-_MAX_SAMPLES:]
            groups = dict(sorted(groups.items(), key=lambda item: item[1][-1][0])[-_MAX_GROUPS:])
            temporary = self.path.with_suffix(".json.part")
            try:
                self.path.parent.mkdir(parents=True, exist_ok=True)
                temporary.write_text(json.dumps({"version": 1, "groups": groups}), encoding="utf-8")
                os.replace(temporary, self.path)
                return True
            except OSError:
                # Optional statistics must never turn a successful design into a failure.
                try:
                    temporary.unlink(missing_ok=True)
                except OSError:
                    pass
                return False
