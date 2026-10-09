"""Offline, fail-closed portrait residual proposals.

This module deliberately does not assign materials and never contacts a
provider.  It turns an R9 audit into a reviewable proposal.  The host may
apply a proposal only after validating the same identity again.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import re
import tempfile
import sys
from pathlib import Path
from typing import Any, Iterable


SCHEMA = "orca.portrait-residual-proposal/v1"
REQUEST_SCHEMA = "orca.portrait-residual-request/v1"
CACHE_SCHEMA = "orca.portrait-residual-cache/v1"
POLICY_VERSION = "portrait-residual-local-v2-verified-support"
PROPOSAL_TYPES = frozenset({"skin", "hair", "cloth", "preserve", "subdivide"})
STATUSES = frozenset({"READY", "PROTECTED_R9", "UNAVAILABLE", "CANCELLED", "APPLIED"})
HARD_REASONS = frozenset(
    {
        "FROZEN_SHAPE_LOCK",
        "CROSS_SUBJECT",
        "CROSS_EYE_SIDE",
        "CROSS_PARENT",
        "UNBOUND_PARENT",
        "MIXED_FACE_UNCONFIRMED",
        "IDENTITY_DRIFT",
        "SOURCE_MAPPING_INVALID",
        "FACE_OUT_OF_RANGE",
    }
)
IDENTITY_FIELDS = (
    "source_sha256",
    "geometry_id",
    "face_count",
    "evidence_sha256",
    "shape_lock_sha256",
    "runtime_sha256",
    "policy_sha256",
)
_SHA = re.compile(r"^[0-9a-f]{64}$")
_ID = re.compile(r"^[A-Za-z0-9_.:-]{1,128}$")
_PARENTS = frozenset({"face", "skin", "hair", "cloth", "neck", "body", "arm", "ear",
                      "le", "re", "lb", "rb", "ulip", "llip", "imouth", "iris"})
_EYE_LABELS = frozenset({"le", "re", "iris", "eye_white"})


class ProposalError(ValueError):
    """A stable, user-safe validation error."""


def _fail(message: str) -> None:
    raise ProposalError(message)


def _sha(value: Any, name: str) -> str:
    if not isinstance(value, str) or not _SHA.fullmatch(value):
        _fail(f"invalid_{name}")
    return value


def _identifier(value: Any, name: str) -> str:
    if not isinstance(value, str) or not _ID.fullmatch(value):
        _fail(f"invalid_{name}")
    return value


def _face_list(value: Any, face_count: int, name: str) -> list[int]:
    if not isinstance(value, list) or value != sorted(set(value)):
        _fail(f"invalid_{name}")
    if any(type(face) is not int or face < 0 or face >= face_count for face in value):
        _fail(f"invalid_{name}")
    return list(value)


def canonical_json(value: Any) -> bytes:
    return json.dumps(value, ensure_ascii=True, sort_keys=True, separators=(",", ":")).encode("utf-8")


def sha256_json(value: Any) -> str:
    return hashlib.sha256(canonical_json(value)).hexdigest()


def identity_from(value: dict[str, Any]) -> dict[str, Any]:
    if not isinstance(value, dict):
        _fail("invalid_identity")
    result: dict[str, Any] = {}
    for field in IDENTITY_FIELDS:
        if field == "face_count":
            count = value.get(field)
            if type(count) is not int or count <= 0 or count > 2_000_000:
                _fail("invalid_face_count")
            result[field] = count
        elif field == "geometry_id":
            result[field] = _sha(value.get(field), field)
        else:
            result[field] = _sha(value.get(field), field)
    return result


def _validate_no_network(value: Any) -> None:
    if isinstance(value, dict):
        for key, child in value.items():
            if key.lower() in {"url", "endpoint", "api_key", "apikey", "provider", "remote"}:
                _fail("online_provider_not_allowed")
            _validate_no_network(child)
    elif isinstance(value, list):
        for child in value:
            _validate_no_network(child)


def _validate_leaf_keys(value: Any, face_count: int, name: str) -> list[list[int]]:
    if value is None:
        return []
    if not isinstance(value, list):
        _fail(f"invalid_{name}")
    result: list[list[int]] = []
    previous: tuple[int, int, int] | None = None
    for item in value:
        if not isinstance(item, list) or len(item) != 3 or any(type(v) is not int for v in item):
            _fail(f"invalid_{name}")
        face, depth, path = item
        if face < 0 or face >= face_count or depth < 0 or depth > 4 or path < 0 or path >= 4**depth:
            _fail(f"invalid_{name}")
        current = (face, depth, path)
        if previous is not None and current <= previous:
            _fail(f"invalid_{name}")
        previous = current
        result.append([face, depth, path])
    encoded = {(face, depth, path) for face, depth, path in result}
    for face, depth, path in result:
        for ancestor in range(depth):
            if (face, ancestor, path >> (2 * (depth - ancestor))) in encoded:
                _fail("overlapping_leaf_ancestry")
    return result


def _unit_reason(unit: dict[str, Any], frozen: set[int], face_count: int) -> str | None:
    face_ids = _face_list(unit.get("face_ids", []), face_count, "unit_faces")
    if not face_ids:
        return "FACE_OUT_OF_RANGE"
    if frozen.intersection(face_ids):
        return "FROZEN_SHAPE_LOCK"
    subject = unit.get("subject_id")
    parent = unit.get("parent_region")
    if not isinstance(subject, str) or not subject:
        return "CROSS_SUBJECT"
    if parent not in _PARENTS:
        return "UNBOUND_PARENT"
    if unit.get("cross_subject"):
        return "CROSS_SUBJECT"
    if unit.get("cross_eye_side"):
        return "CROSS_EYE_SIDE"
    if unit.get("cross_parent"):
        return "CROSS_PARENT"
    if unit.get("mapping_valid") is False:
        return "SOURCE_MAPPING_INVALID"
    if unit.get("mixed") and not unit.get("leaf_keys"):
        return "MIXED_FACE_UNCONFIRMED"
    if parent in _EYE_LABELS and unit.get("eye_side") not in {"left", "right"}:
        return "CROSS_EYE_SIDE"
    return None


def _classify(unit: dict[str, Any]) -> str:
    proposed = unit.get("proposal") or unit.get("suggested_type")
    if proposed in PROPOSAL_TYPES:
        return str(proposed)
    parent = unit.get("parent_region")
    if parent in {"skin", "face", "neck", "body", "arm"}:
        return "skin"
    if parent in {"hair", "ear"}:
        return "hair"
    if parent == "cloth":
        return "cloth"
    return "preserve"


def _validate_views(unit: dict[str, Any]) -> int:
    if unit.get("evidence_source") == "verified-local-semantics":
        count, pixels = unit.get("view_support"), unit.get("pixel_support")
        if type(count) is not int or not 0 <= count <= 16 or type(pixels) is not int or pixels < count:
            _fail("invalid_verified_support")
        return count
    views = unit.get("views", [])
    if not isinstance(views, list):
        _fail("invalid_views")
    identifiers: set[str] = set()
    total = 0
    for view in views:
        if not isinstance(view, dict):
            _fail("invalid_views")
        identifier = _identifier(view.get("id"), "view_id")
        if identifier in identifiers:
            _fail("duplicate_view")
        identifiers.add(identifier)
        pixels = view.get("pixels", 0)
        if type(pixels) is not int or pixels < 0 or pixels > 16_777_216:
            _fail("invalid_view_pixels")
        total += pixels
    return sum(view.get("pixels", 0) > 0 or view.get("analytic_coverage") is True for view in views)


def _proposal_record(unit: dict[str, Any], kind: str, identity: dict[str, Any], frozen: set[int], budget: int) -> dict[str, Any]:
    unit_id = _identifier(unit.get("unit_id"), "unit_id")
    faces = _face_list(unit.get("face_ids", []), identity["face_count"], "unit_faces")
    leaves = _validate_leaf_keys(unit.get("leaf_keys"), identity["face_count"], "leaf_keys")
    if leaves and any(item[0] not in faces for item in leaves):
        _fail("leaf_outside_unit")
    rejected = _face_list(unit.get("rejected_faces", []), identity["face_count"], "rejected_faces")
    if set(faces).intersection(rejected):
        _fail("accepted_rejected_overlap")
    views = _validate_views(unit)
    reasons = [str(reason) for reason in unit.get("risk_reasons", [])]
    if any(not _ID.fullmatch(reason) for reason in reasons):
        _fail("invalid_risk_reason")
    hard = _unit_reason(unit, frozen, identity["face_count"])
    if any(reason in HARD_REASONS for reason in reasons):
        hard = next(reason for reason in reasons if reason in HARD_REASONS)
    compatible = {"skin": {"face", "skin", "neck", "body", "arm"}, "hair": {"hair"}, "cloth": {"cloth"}}
    if not hard and kind in compatible and unit.get("parent_region") not in compatible[kind]:
        hard = "CROSS_PARENT"
    if hard:
        return {
            "unit_id": unit_id,
            "proposal": "preserve",
            "subject_id": str(unit.get("subject_id", "")),
            "parent_region": str(unit.get("parent_region", "")),
            "confidence": 0.0,
            "view_support": views,
            "source_faces": faces,
            "leaf_keys": leaves,
            "rejected_faces": rejected,
            "reasons": [hard],
            "applied": False,
            "status": "REJECTED",
        }
    target_rgb = unit.get("target_rgb")
    if target_rgb is not None:
        if not isinstance(target_rgb, list) or len(target_rgb) != 3 or any(
                not isinstance(channel, (int, float)) or not 0 <= float(channel) <= 1 for channel in target_rgb):
            _fail("invalid_target_rgb")
        target_rgb = [float(channel) for channel in target_rgb]
    confidence = unit.get("confidence", 0.0)
    if type(confidence) not in (int, float) or not math.isfinite(confidence) or not 0 <= float(confidence) <= 1:
        _fail("invalid_confidence")
    if kind != "preserve" and views < 2:
        reasons.append("INSUFFICIENT_VIEW_SUPPORT")
        kind = "preserve"
    if kind != "preserve" and confidence < .85:
        reasons.append("LOW_CONFIDENCE")
        kind = "preserve"
    if kind == "subdivide":
        needed = unit.get("additional_triangles", len(faces))
        if type(needed) is not int or needed < 1:
            _fail("invalid_subdivide_budget")
        if needed > budget:
            reasons.append("SUBDIVIDE_BUDGET_UNAVAILABLE")
            return {
                "unit_id": unit_id, "proposal": "subdivide", "confidence": float(confidence),
                "subject_id": str(unit.get("subject_id", "")),
                "parent_region": str(unit.get("parent_region", "")),
                "view_support": views, "source_faces": faces, "leaf_keys": leaves,
                "rejected_faces": rejected, "reasons": reasons, "applied": False,
            "status": "UNAPPLIED_BUDGET", "additional_triangles": needed,
            **({"target_rgb": target_rgb} if target_rgb is not None else {}),
            }
        budget -= needed
    return {
        "unit_id": unit_id,
        "proposal": kind,
        "subject_id": str(unit.get("subject_id", "")),
        "parent_region": str(unit.get("parent_region", "")),
        "confidence": float(confidence),
        "view_support": views,
        "source_faces": faces,
        "leaf_keys": leaves,
        "rejected_faces": rejected,
        "reasons": reasons,
        "applied": False,
        "status": "PROPOSED",
        **({"target_slot": unit["target_slot"]} if "target_slot" in unit else {}),
        **({"additional_triangles": int(unit.get("additional_triangles", 0))} if kind == "subdivide" else {}),
        **({"target_rgb": target_rgb} if target_rgb is not None else {}),
    }


def validate_request(request: dict[str, Any]) -> tuple[dict[str, Any], list[dict[str, Any]]]:
    if not isinstance(request, dict) or request.get("schema") != REQUEST_SCHEMA:
        _fail("invalid_request_schema")
    _validate_no_network(request)
    identity = identity_from(request)
    units = request.get("units", [])
    if not isinstance(units, list) or len(units) > 200_000:
        _fail("invalid_units")
    seen: set[str] = set()
    clean: list[dict[str, Any]] = []
    for unit in units:
        if not isinstance(unit, dict):
            _fail("invalid_unit")
        unit_id = _identifier(unit.get("unit_id"), "unit_id")
        if unit_id in seen:
            _fail("duplicate_unit")
        seen.add(unit_id)
        clean.append(unit)
    return identity, clean


def build_proposal(request: dict[str, Any], cancelled: bool = False) -> dict[str, Any]:
    identity, units = validate_request(request)
    if cancelled:
        return {
            "schema": SCHEMA, "policy_version": POLICY_VERSION, "status": "CANCELLED",
            **identity, "request_sha256": sha256_json(request), "proposals": [], "rejected": [], "statistics": {"units": len(units), "proposed": 0, "rejected": 0},
        }
    frozen = set(_face_list(request.get("frozen_faces", []), identity["face_count"], "frozen_faces"))
    budget = request.get("remaining_triangle_budget", 0)
    if type(budget) is not int or budget < 0 or budget > 20_000:
        _fail("invalid_remaining_triangle_budget")
    records: list[dict[str, Any]] = []
    rejected: list[dict[str, Any]] = []
    for unit in units:
        record = _proposal_record(unit, _classify(unit), identity, frozen, budget)
        if record["status"] == "REJECTED":
            rejected.append(record)
        else:
            records.append(record)
            if record["proposal"] == "subdivide" and record["status"] != "UNAPPLIED_BUDGET":
                budget -= record.get("additional_triangles", 0)
    status = "READY" if records else "PROTECTED_R9"
    return {
        "schema": SCHEMA,
        "policy_version": POLICY_VERSION,
        "status": status,
        **identity,
        "request_sha256": sha256_json(request),
        "remaining_triangle_budget": budget,
        "proposals": records,
        "rejected": rejected,
        "statistics": {
            "units": len(units), "proposed": sum(r["status"] == "PROPOSED" for r in records),
            "unapplied_budget": sum(r["status"] == "UNAPPLIED_BUDGET" for r in records),
            "rejected": len(rejected), "frozen_faces": len(frozen),
            "total_source_faces": identity["face_count"],
            "unresolved_source_faces": identity["face_count"] - len({f for r in records if r["proposal"] in {"skin", "hair", "cloth"} for f in r["source_faces"]}),
        },
    }


def validate_proposal(proposal: dict[str, Any], expected: dict[str, Any] | None = None) -> dict[str, Any]:
    if not isinstance(proposal, dict) or proposal.get("schema") != SCHEMA:
        _fail("invalid_proposal_schema")
    identity = identity_from(proposal)
    _sha(proposal.get("request_sha256"), "request_sha256")
    if expected is not None:
        expected_identity = identity_from(expected)
        if identity != expected_identity:
            _fail("identity_drift")
    if proposal.get("policy_version") != POLICY_VERSION or proposal.get("status") not in STATUSES:
        _fail("invalid_proposal_status")
    if not isinstance(proposal.get("proposals"), list) or not isinstance(proposal.get("rejected"), list):
        _fail("invalid_proposals")
    seen: set[str] = set()
    claimed: set[int] = set()
    for record in proposal["proposals"] + proposal["rejected"]:
        if not isinstance(record, dict):
            _fail("invalid_proposal_record")
        unit_id = _identifier(record.get("unit_id"), "unit_id")
        if unit_id in seen:
            _fail("duplicate_proposal")
        seen.add(unit_id)
        kind = record.get("proposal")
        if kind not in PROPOSAL_TYPES:
            _fail("invalid_proposal_type")
        faces = _face_list(record.get("source_faces", []), identity["face_count"], "source_faces")
        if claimed.intersection(faces) and kind != "preserve":
            _fail("overlapping_proposals")
        if kind != "preserve":
            claimed.update(faces)
        leaves = _validate_leaf_keys(record.get("leaf_keys", []), identity["face_count"], "leaf_keys")
        if any(leaf[0] not in faces for leaf in leaves):
            _fail("leaf_outside_unit")
        rejected = _face_list(record.get("rejected_faces", []), identity["face_count"], "rejected_faces")
        if set(faces).intersection(rejected):
            _fail("accepted_rejected_overlap")
        confidence, support = record.get("confidence"), record.get("view_support")
        if type(confidence) not in (int, float) or not math.isfinite(confidence) or not 0 <= confidence <= 1:
            _fail("invalid_confidence")
        if type(support) is not int or not 0 <= support <= 16:
            _fail("invalid_view_support")
        if "target_rgb" in record:
            target = record["target_rgb"]
            if not isinstance(target, list) or len(target) != 3 or any(
                    not isinstance(channel, (int, float)) or not 0 <= float(channel) <= 1 for channel in target):
                _fail("invalid_target_rgb")
        if record.get("status") not in {"PROPOSED", "REJECTED", "UNAPPLIED_BUDGET"}:
            _fail("invalid_proposal_record_status")
        if not isinstance(record.get("reasons", []), list):
            _fail("invalid_proposal_reasons")
        if any(not isinstance(reason, str) or not _ID.fullmatch(reason) for reason in record.get("reasons", [])):
            _fail("invalid_proposal_reasons")
        if record in proposal["rejected"] and record["status"] != "REJECTED":
            _fail("invalid_rejected_status")
        if kind in {"skin", "hair", "cloth"} and record["status"] == "PROPOSED":
            compatible = {"skin": {"face", "skin", "neck", "body", "arm"}, "hair": {"hair"}, "cloth": {"cloth"}}
            if record.get("parent_region") not in compatible[kind] or not record.get("subject_id") or support < 2 or confidence < .85:
                _fail("unsafe_actionable_proposal")
            if any(reason in HARD_REASONS for reason in record.get("reasons", [])):
                _fail("unsafe_actionable_proposal")
    return proposal


def apply_proposal(proposal: dict[str, Any], expected: dict[str, Any], cancelled: bool = False) -> dict[str, Any]:
    validate_proposal(proposal, expected)
    if cancelled:
        return {"status": "CANCELLED", "applied": [], "preserved": "R9"}
    if proposal["status"] not in {"READY", "PROTECTED_R9"}:
        _fail("proposal_not_ready")
    applied: list[dict[str, Any]] = []
    for record in proposal["proposals"]:
        if record["status"] != "PROPOSED" or record["proposal"] in {"preserve", "subdivide"}:
            continue
        applied.append({"unit_id": record["unit_id"], "proposal": record["proposal"],
                        "source_faces": record["source_faces"], "leaf_keys": record["leaf_keys"],
                        **({"target_rgb": record["target_rgb"]} if "target_rgb" in record else {})})
    return {"status": "APPLIED", "applied": applied, "preserved": "R9", "material_tree_changed": False,
            "production_enabled": False, "provider_called": False}


def cache_key(proposal: dict[str, Any]) -> str:
    validate_proposal(proposal)
    return sha256_json({k: v for k, v in proposal.items() if k != "cache_path"})


def write_cache(root: Path, proposal: dict[str, Any]) -> Path:
    key = cache_key(proposal)
    root = root.resolve()
    root.mkdir(parents=True, exist_ok=True)
    destination = root / "portrait_residuals" / key / "proposal.json"
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.exists():
        existing = json.loads(destination.read_text(encoding="utf-8"))
        if existing != proposal:
            _fail("cache_identity_collision")
        return destination
    fd, temporary = tempfile.mkstemp(prefix="proposal-", suffix=".partial", dir=str(destination.parent))
    try:
        with os.fdopen(fd, "wb") as stream:
            stream.write(canonical_json(proposal))
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, destination)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)
    return destination


def _main(argv: Iterable[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--request", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--cache-root", type=Path)
    parser.add_argument("--cancel-file", type=Path)
    args = parser.parse_args(argv)
    try:
        def deny_network(event, _args):
            if event in {"socket.connect", "socket.getaddrinfo", "socket.bind", "subprocess.Popen"}:
                raise PermissionError("offline_only")
        sys.addaudithook(deny_network)
        sys.path.insert(0,str(Path(__file__).resolve().parent))
        from bundled_portrait_runtime import verify
        verify(Path(sys.executable).resolve().parent.parent,Path(__file__).resolve().parent)
        request_bytes = args.request.read_bytes()
        request = json.loads(request_bytes)
        cancelled = bool(args.cancel_file and args.cancel_file.exists())
        result = build_proposal(request, cancelled=cancelled)
        result["request_sha256"] = hashlib.sha256(request_bytes).hexdigest()
        validate_proposal(result, request)
        if args.cache_root and result["status"] in {"READY", "PROTECTED_R9"}:
            result["cache_path"] = str(write_cache(args.cache_root, result).relative_to(args.cache_root.resolve()).as_posix())
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_bytes(canonical_json(result))
        return 0
    except Exception:
        fallback = {"schema": SCHEMA, "policy_version": POLICY_VERSION, "status": "UNAVAILABLE",
                    "error": "local_residual_proposal_unavailable"}
        try:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_bytes(canonical_json(fallback))
        except OSError:
            return 2
        return 2


if __name__ == "__main__":
    raise SystemExit(_main())
