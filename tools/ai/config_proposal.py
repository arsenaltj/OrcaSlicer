"""Configuration proposal prompts and allowlist validation; no transport."""
from __future__ import annotations

import json
import re
from model_contracts import MAX_CHANGES
from typing import Any

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
