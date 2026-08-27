#!/usr/bin/env python3
"""Validate the pinned OrcaSlicer AI integration contract.

The lock file is intentionally machine-readable and contains only public Git
identities and runtime metadata. It must never contain provider credentials.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys
from typing import Any, Iterable, Sequence


SCHEMA = "orcaslicer.ai-integration-lock/v2"
EXPECTED_INTEGRATION_BRANCH = "codex/orca-integration-v2"
EXPECTED_UPSTREAM = {
    "remote": "upstream",
    "branch": "main",
    "ref": "upstream/main",
    "sha": "6fdd4945c19348cc5fc9ed9ae2f26f22a778786b",
}
EXPECTED_FEATURE_SOURCES = {
    "model_generation": {
        "branch": "codex/model-generation",
        "sha": "db81edc2b028c37822c997e99eb88c0465089f0c",
    },
    "smart_slicing": {
        "branch": "codex/smart-slicing",
        "sha": "1c163d68906e287b946b40a975feb3bfd9aab68d",
    },
}
EXPECTED_INTEGRATION_RECEIPTS = {
    "model_generation": {
        "source_sha": "db81edc2b028c37822c997e99eb88c0465089f0c",
        "integration_commit": "c1cdfece8752d24637ec9d62edc70f2101455b1f",
        "method": "verified_git_object_subset",
        "verified_git_objects": {
            "src/slic3r/AI/ModelGeneration": "f12bb0e8d725bf428498639ae47f19abd0527e95",
            "src/slic3r/GUI/AIModelGenerationClient.cpp": "a8a67cc4ffc4d7ae1097801fdf534c7257d32810",
            "src/slic3r/GUI/AIModelGenerationClient.hpp": "50f3c9b8749199ca396fa2e00ccd407881c0f90b",
            "src/slic3r/GUI/ModelGenerationPanel.hpp": "d3b3a42cb4a6c32f7a121ad8a4cb71d2129af5ec",
        },
    },
    "smart_slicing": {
        "source_sha": "1c163d68906e287b946b40a975feb3bfd9aab68d",
        "integration_commit": "c1cdfece8752d24637ec9d62edc70f2101455b1f",
        "method": "verified_git_object_subset",
        "verified_git_objects": {
            "src/slic3r/AI/SmartSlicing": "1dce995947edfaf834388f25d278148dfe9f1cf9",
            "src/slic3r/GUI/AI/SmartSlicing": "c6afa49cba2e5b107fa1430c249cebd9e9371d1a",
        },
    },
}
EXPECTED_PROTOCOL_VERSION = 2
EXPECTED_SIDECAR_VERSION = "orcaslicer-ai-sidecar-v8"
EXPECTED_PRODUCT_PORT = 18764
EXPECTED_DEVELOPMENT_PORTS = {
    "model_generation": 18765,
    "smart_slicing": 18766,
    "integration": 18767,
}
EXPECTED_BOUNDARY_FLAGS = (
    "integration_consumes_exact_accepted_sha",
    "feature_branches_receive_no_reverse_integration",
    "preserve_orca_defaults",
    "preserve_3mf_and_profile_formats",
    "paid_api_requires_explicit_authorization",
)
REQUIRED_INTEGRATION_PATHS = {
    ".github",
    "CMakeLists.txt",
    "build_release_vs.bat",
    "docs/architecture/ai-integration-lock.json",
    "scripts/package_internal_fast.ps1",
    "scripts/verify_ai_integration.py",
    "src/CMakeLists.txt",
    "src/slic3r/CMakeLists.txt",
    "src/slic3r/GUI/AI/Orca",
    "src/slic3r/GUI/MainFrame.cpp",
    "src/slic3r/GUI/MainFrame.hpp",
    "src/slic3r/GUI/Plater.cpp",
    "src/slic3r/GUI/Plater.hpp",
}
REQUIRED_SHARED_RUNTIME_PATHS = {
    "src/slic3r/GUI/AIServiceManager.cpp",
    "src/slic3r/GUI/AIServiceManager.hpp",
    "src/slic3r/GUI/AISidecarClient.cpp",
    "src/slic3r/GUI/AISidecarClient.hpp",
    "src/slic3r/Utils/Http.cpp",
    "src/slic3r/Utils/Http.hpp",
    "tools/ai/ai_diagnostics.py",
    "tools/ai/network_policy.py",
    "tools/ai/orca_ai_build_info.json.in",
    "tools/ai/orca_ai_installed_bootstrap.py",
    "tools/ai/orca_ai_runtime_dependencies.json.in",
    "tools/ai/verify_bundled_runtime.py",
}
REQUIRED_CONTRACT_PATHS = {
    "src/slic3r/AI/ModelGeneration/GeneratedModelArtifact.hpp",
    "src/slic3r/AI/ModelGeneration/IPrintablePaletteProvider.hpp",
    "src/slic3r/AI/SmartSlicing/IModelArtifactConsumer.hpp",
    "src/slic3r/GUI/AI/Orca/OrcaWorkspaceAdapter.hpp",
}
SHA_PATTERN = re.compile(r"^[0-9a-f]{40}$")
FORBIDDEN_TRACKED_SECRET_NAMES = {
    "orca_ai_internal_defaults.json",
}
SECRET_SCAN_SUFFIXES = {
    ".bat", ".cmake", ".cpp", ".h", ".hpp", ".json", ".md", ".ps1", ".py", ".txt", ".yaml", ".yml",
}
DIRECT_OPENAI_KEY_PATTERN = re.compile(r"\bsk-(?:proj-)?[A-Za-z0-9_-]{30,}\b")
LITERAL_PROVIDER_KEY_PATTERN = re.compile(
    r'''(?i)["']?(OPENAI_API_KEY|TRIPO_API_KEY)["']?\s*[:=]\s*["']([^"'\r\n]{16,})["']'''
)
PRIVATE_KEY_PATTERN = re.compile(r"-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----")


def _issue(code: str, message: str) -> dict[str, str]:
    return {"code": code, "message": message}


def _expect_object(value: Any, name: str, errors: list[dict[str, str]]) -> dict[str, Any]:
    if not isinstance(value, dict):
        errors.append(_issue("schema.type", f"{name} must be an object"))
        return {}
    return value


def _expect_exact_keys(
    value: dict[str, Any], expected: Iterable[str], name: str, errors: list[dict[str, str]]
) -> None:
    expected_keys = set(expected)
    actual_keys = set(value)
    missing = sorted(expected_keys - actual_keys)
    unknown = sorted(actual_keys - expected_keys)
    if missing:
        errors.append(_issue("schema.missing", f"{name} is missing: {', '.join(missing)}"))
    if unknown:
        errors.append(_issue("schema.unknown", f"{name} has unknown fields: {', '.join(unknown)}"))


def _expect_constant(
    actual: Any, expected: Any, name: str, errors: list[dict[str, str]]
) -> None:
    if actual != expected:
        errors.append(_issue("contract.constant", f"{name} must be {expected!r}, got {actual!r}"))


def _validate_sha(value: Any, name: str, errors: list[dict[str, str]]) -> None:
    if not isinstance(value, str) or SHA_PATTERN.fullmatch(value) is None:
        errors.append(_issue("schema.sha", f"{name} must be a lowercase full 40-character Git SHA"))


def _validate_path_list(value: Any, name: str, errors: list[dict[str, str]]) -> set[str]:
    if not isinstance(value, list) or not value:
        errors.append(_issue("boundary.paths", f"{name} must be a non-empty array"))
        return set()

    valid_paths: set[str] = set()
    for index, item in enumerate(value):
        if not isinstance(item, str) or not item:
            errors.append(_issue("boundary.path", f"{name}[{index}] must be a non-empty string"))
            continue
        path = PurePosixPath(item)
        if item.startswith("/") or "\\" in item or ".." in path.parts or str(path) != item:
            errors.append(
                _issue("boundary.path", f"{name}[{index}] must be a normalized repository-relative POSIX path")
            )
            continue
        if item in valid_paths:
            errors.append(_issue("boundary.duplicate", f"{name} contains duplicate path {item!r}"))
            continue
        valid_paths.add(item)
    return valid_paths


def _overlapping_paths(first: Iterable[str], second: Iterable[str]) -> list[tuple[str, str]]:
    """Return exact or parent/child overlaps between two ownership sets."""
    overlaps: list[tuple[str, str]] = []
    for left in sorted(first):
        left_parts = PurePosixPath(left).parts
        for right in sorted(second):
            right_parts = PurePosixPath(right).parts
            common_length = min(len(left_parts), len(right_parts))
            if left_parts[:common_length] == right_parts[:common_length]:
                overlaps.append((left, right))
    return overlaps


def validate_document(document: Any) -> list[dict[str, str]]:
    """Validate schema, pinned identities and ownership boundary declarations."""
    errors: list[dict[str, str]] = []
    root = _expect_object(document, "document", errors)
    _expect_exact_keys(
        root,
        (
            "schema",
            "integration_branch",
            "upstream",
            "feature_sources",
            "integration_receipts",
            "runtime_contract",
            "boundaries",
        ),
        "document",
        errors,
    )
    _expect_constant(root.get("schema"), SCHEMA, "schema", errors)
    _expect_constant(root.get("integration_branch"), EXPECTED_INTEGRATION_BRANCH, "integration_branch", errors)

    upstream = _expect_object(root.get("upstream"), "upstream", errors)
    _expect_exact_keys(upstream, EXPECTED_UPSTREAM, "upstream", errors)
    for key, expected in EXPECTED_UPSTREAM.items():
        _expect_constant(upstream.get(key), expected, f"upstream.{key}", errors)
    _validate_sha(upstream.get("sha"), "upstream.sha", errors)

    sources = _expect_object(root.get("feature_sources"), "feature_sources", errors)
    _expect_exact_keys(sources, EXPECTED_FEATURE_SOURCES, "feature_sources", errors)
    for source_name, expected_source in EXPECTED_FEATURE_SOURCES.items():
        source = _expect_object(sources.get(source_name), f"feature_sources.{source_name}", errors)
        _expect_exact_keys(source, expected_source, f"feature_sources.{source_name}", errors)
        for key, expected in expected_source.items():
            _expect_constant(source.get(key), expected, f"feature_sources.{source_name}.{key}", errors)
        _validate_sha(source.get("sha"), f"feature_sources.{source_name}.sha", errors)

    receipts = _expect_object(root.get("integration_receipts"), "integration_receipts", errors)
    _expect_exact_keys(receipts, EXPECTED_INTEGRATION_RECEIPTS, "integration_receipts", errors)
    for feature_name, expected_receipt in EXPECTED_INTEGRATION_RECEIPTS.items():
        receipt = _expect_object(receipts.get(feature_name), f"integration_receipts.{feature_name}", errors)
        _expect_exact_keys(receipt, expected_receipt, f"integration_receipts.{feature_name}", errors)
        for key in ("source_sha", "integration_commit", "method"):
            _expect_constant(
                receipt.get(key),
                expected_receipt[key],
                f"integration_receipts.{feature_name}.{key}",
                errors,
            )
        _validate_sha(receipt.get("source_sha"), f"integration_receipts.{feature_name}.source_sha", errors)
        _validate_sha(
            receipt.get("integration_commit"),
            f"integration_receipts.{feature_name}.integration_commit",
            errors,
        )
        objects = _expect_object(
            receipt.get("verified_git_objects"),
            f"integration_receipts.{feature_name}.verified_git_objects",
            errors,
        )
        expected_objects = expected_receipt["verified_git_objects"]
        _expect_exact_keys(
            objects,
            expected_objects,
            f"integration_receipts.{feature_name}.verified_git_objects",
            errors,
        )
        for path, expected_object in expected_objects.items():
            _expect_constant(
                objects.get(path),
                expected_object,
                f"integration_receipts.{feature_name}.verified_git_objects.{path}",
                errors,
            )
            _validate_sha(
                objects.get(path),
                f"integration_receipts.{feature_name}.verified_git_objects.{path}",
                errors,
            )

    runtime = _expect_object(root.get("runtime_contract"), "runtime_contract", errors)
    _expect_exact_keys(
        runtime,
        ("protocol_version", "sidecar_version", "product_port", "development_ports", "development_port_policy"),
        "runtime_contract",
        errors,
    )
    _expect_constant(runtime.get("protocol_version"), EXPECTED_PROTOCOL_VERSION, "runtime_contract.protocol_version", errors)
    _expect_constant(runtime.get("sidecar_version"), EXPECTED_SIDECAR_VERSION, "runtime_contract.sidecar_version", errors)
    _expect_constant(runtime.get("product_port"), EXPECTED_PRODUCT_PORT, "runtime_contract.product_port", errors)
    development_ports = _expect_object(runtime.get("development_ports"), "runtime_contract.development_ports", errors)
    _expect_exact_keys(development_ports, EXPECTED_DEVELOPMENT_PORTS, "runtime_contract.development_ports", errors)
    for role, expected_port in EXPECTED_DEVELOPMENT_PORTS.items():
        _expect_constant(development_ports.get(role), expected_port, f"runtime_contract.development_ports.{role}", errors)
    development_port_values = list(development_ports.values())
    if not all(type(value) is int and 1 <= value <= 65535 for value in development_port_values):
        errors.append(_issue("runtime.ports", "development ports must be integers between 1 and 65535"))
    else:
        if len(set(development_port_values)) != len(development_port_values):
            errors.append(_issue("runtime.ports", "development ports must be unique per development role"))
        if runtime.get("product_port") in development_port_values:
            errors.append(_issue("runtime.product_port", "product_port must not be assigned to a development role"))
    policy = runtime.get("development_port_policy")
    if not isinstance(policy, str) or "explicit runtime override" not in policy or "product_port" not in policy:
        errors.append(
            _issue("runtime.port_policy", "development_port_policy must distinguish explicit overrides from product_port")
        )

    boundaries = _expect_object(root.get("boundaries"), "boundaries", errors)
    _expect_exact_keys(
        boundaries,
        (
            *EXPECTED_BOUNDARY_FLAGS,
            "integration_owned_paths",
            "shared_runtime_owned_paths",
            "model_generation_owned_paths",
            "smart_slicing_owned_paths",
            "allowed_cross_feature_contracts",
        ),
        "boundaries",
        errors,
    )
    for flag in EXPECTED_BOUNDARY_FLAGS:
        if boundaries.get(flag) is not True:
            errors.append(_issue("boundary.rule", f"boundaries.{flag} must be true"))

    integration_paths = _validate_path_list(
        boundaries.get("integration_owned_paths"), "boundaries.integration_owned_paths", errors
    )
    shared_runtime_paths = _validate_path_list(
        boundaries.get("shared_runtime_owned_paths"), "boundaries.shared_runtime_owned_paths", errors
    )
    model_paths = _validate_path_list(
        boundaries.get("model_generation_owned_paths"), "boundaries.model_generation_owned_paths", errors
    )
    slicing_paths = _validate_path_list(
        boundaries.get("smart_slicing_owned_paths"), "boundaries.smart_slicing_owned_paths", errors
    )
    contract_paths = _validate_path_list(
        boundaries.get("allowed_cross_feature_contracts"), "boundaries.allowed_cross_feature_contracts", errors
    )
    missing_integration = sorted(REQUIRED_INTEGRATION_PATHS - integration_paths)
    if missing_integration:
        errors.append(
            _issue("boundary.integration", f"integration ownership is missing: {', '.join(missing_integration)}")
        )
    missing_shared_runtime = sorted(REQUIRED_SHARED_RUNTIME_PATHS - shared_runtime_paths)
    if missing_shared_runtime:
        errors.append(
            _issue(
                "boundary.shared_runtime",
                f"shared runtime ownership is missing: {', '.join(missing_shared_runtime)}",
            )
        )
    missing_contracts = sorted(REQUIRED_CONTRACT_PATHS - contract_paths)
    if missing_contracts:
        errors.append(_issue("boundary.contract", f"allowed contracts are missing: {', '.join(missing_contracts)}"))
    ownership_groups = {
        "integration": integration_paths,
        "shared_runtime": shared_runtime_paths,
        "model_generation": model_paths,
        "smart_slicing": slicing_paths,
    }
    group_names = tuple(ownership_groups)
    for first_index, first_name in enumerate(group_names):
        for second_name in group_names[first_index + 1 :]:
            overlaps = _overlapping_paths(ownership_groups[first_name], ownership_groups[second_name])
            if overlaps:
                rendered = ", ".join(f"{left} <> {right}" for left, right in overlaps)
                errors.append(
                    _issue(
                        "boundary.overlap",
                        f"{first_name} and {second_name} ownership overlap: {rendered}",
                    )
                )
    return errors


def _run_git(repo_root: Path, arguments: Sequence[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["git", "-C", str(repo_root), *arguments],
        check=False,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
    )


def _git_ref_exists(repo_root: Path, ref: str) -> bool:
    return _run_git(repo_root, ["show-ref", "--verify", "--quiet", ref]).returncode == 0


def _git_object_at_path(repo_root: Path, commit: str, path: str) -> str | None:
    result = _run_git(repo_root, ["rev-parse", f"{commit}:{path}"])
    if result.returncode != 0:
        return None
    value = result.stdout.strip()
    return value if SHA_PATTERN.fullmatch(value) else None


def validate_tracked_paths(paths: Iterable[str]) -> list[dict[str, str]]:
    """Reject package-only credential payloads from Git."""
    errors: list[dict[str, str]] = []
    for value in paths:
        normalized = value.replace("\\", "/")
        name = PurePosixPath(normalized).name.lower()
        if name in FORBIDDEN_TRACKED_SECRET_NAMES or name == ".env" or name.startswith(".env."):
            errors.append(_issue("security.tracked_secret", f"secret-bearing file must not be tracked: {normalized}"))
    return errors


def _placeholder_secret(value: str) -> bool:
    normalized = value.strip().lower()
    return (
        normalized.startswith(("$", "<", "{", "test", "fake", "example", "placeholder", "packaged"))
        or normalized in {"configured", "missing", "redacted", "secret", "your-key-here"}
        or "<redacted>" in normalized
    )


def _secret_finding(line: str) -> str:
    if PRIVATE_KEY_PATTERN.search(line):
        return "private-key material"
    if DIRECT_OPENAI_KEY_PATTERN.search(line):
        return "OpenAI-style credential"
    assignment = LITERAL_PROVIDER_KEY_PATTERN.search(line)
    if assignment and not _placeholder_secret(assignment.group(2)):
        return f"literal {assignment.group(1).upper()} credential"
    return ""


def validate_tracked_secret_content(repo_root: Path, paths: Iterable[str]) -> list[dict[str, str]]:
    """Scan relevant tracked source/config files without ever echoing a matched value."""
    errors: list[dict[str, str]] = []
    for relative_path in paths:
        normalized = relative_path.replace("\\", "/")
        path = repo_root / normalized
        if path.suffix.lower() not in SECRET_SCAN_SUFFIXES:
            continue
        try:
            if path.stat().st_size > 2 * 1024 * 1024:
                continue
            content = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        for line_number, line in enumerate(content.splitlines(), 1):
            finding = _secret_finding(line)
            if finding:
                errors.append(
                    _issue("security.secret_content", f"{finding} must not be tracked: {normalized}:{line_number}")
                )
    return errors


def validate_git_secret_content(repo_root: Path) -> list[dict[str, str]]:
    """Use Git's index scan to avoid opening every tracked Orca source file on Windows."""
    pathspecs = [f"*{suffix}" for suffix in sorted(SECRET_SCAN_SUFFIXES)]
    result = _run_git(
        repo_root,
        [
            "grep",
            "-n",
            "-I",
            "-E",
            "-e",
            "OPENAI_API_KEY|TRIPO_API_KEY|sk-|PRIVATE KEY",
            "--",
            *pathspecs,
        ],
    )
    if result.returncode == 1:
        return []
    if result.returncode != 0:
        return [_issue("security.scan_failed", "tracked secret content scan could not be completed")]

    errors: list[dict[str, str]] = []
    for record in result.stdout.splitlines():
        relative_path, separator, remainder = record.partition(":")
        line_text, line_separator, line = remainder.partition(":")
        if not separator or not line_separator or not line_text.isdigit():
            continue
        finding = _secret_finding(line)
        if finding:
            errors.append(
                _issue(
                    "security.secret_content",
                    f"{finding} must not be tracked: {relative_path.replace(chr(92), '/')}:{line_text}",
                )
            )
    return errors


def validate_integration_receipts(
    document: dict[str, Any], repo_root: Path, head: str
) -> tuple[list[dict[str, str]], dict[str, Any]]:
    """Verify historical snapshot-port receipts without scanning the whole worktree."""
    errors: list[dict[str, str]] = []
    receipt_details: dict[str, Any] = {}
    for feature_name, receipt in document["integration_receipts"].items():
        source_sha = receipt["source_sha"]
        integration_commit = receipt["integration_commit"]
        feature_details: dict[str, Any] = {
            "source_sha": source_sha,
            "integration_commit": integration_commit,
            "verified_paths": [],
        }
        receipt_details[feature_name] = feature_details
        if source_sha != document["feature_sources"][feature_name]["sha"]:
            errors.append(
                _issue(
                    "git.receipt_source",
                    f"{feature_name} receipt source does not match the accepted feature source",
                )
            )
            continue
        if _run_git(repo_root, ["cat-file", "-e", f"{integration_commit}^{{commit}}"]).returncode != 0:
            errors.append(
                _issue(
                    "git.receipt_commit",
                    f"{feature_name} integration receipt commit is unavailable: {integration_commit}",
                )
            )
            continue
        if _run_git(repo_root, ["merge-base", "--is-ancestor", integration_commit, head]).returncode != 0:
            errors.append(
                _issue(
                    "git.receipt_ancestor",
                    f"{feature_name} integration receipt {integration_commit} is not an ancestor of HEAD {head}",
                )
            )
            continue
        for path, expected_object in receipt["verified_git_objects"].items():
            source_object = _git_object_at_path(repo_root, source_sha, path)
            integration_object = _git_object_at_path(repo_root, integration_commit, path)
            if source_object is None or integration_object is None:
                errors.append(
                    _issue(
                        "git.receipt_path",
                        f"{feature_name} receipt path is missing from its source or integration commit: {path}",
                    )
                )
                continue
            if source_object != expected_object or integration_object != expected_object:
                errors.append(
                    _issue(
                        "git.receipt_object",
                        f"{feature_name} receipt object mismatch for {path}",
                    )
                )
                continue
            feature_details["verified_paths"].append(path)
    return errors, receipt_details


def validate_git(document: dict[str, Any], repo_root: Path) -> tuple[list[dict[str, str]], dict[str, Any]]:
    """Validate that locked commits exist and remain tied to the intended histories."""
    errors: list[dict[str, str]] = []
    details: dict[str, Any] = {}
    head_result = _run_git(repo_root, ["rev-parse", "HEAD"])
    if head_result.returncode != 0:
        return [_issue("git.repository", "repository HEAD could not be resolved")], details
    head = head_result.stdout.strip()
    details["head"] = head

    tracked_result = _run_git(repo_root, ["ls-files"])
    if tracked_result.returncode != 0:
        errors.append(_issue("git.tracked_files", "tracked files could not be enumerated"))
    else:
        tracked_paths = tracked_result.stdout.splitlines()
        errors.extend(validate_tracked_paths(tracked_paths))
        errors.extend(validate_git_secret_content(repo_root))

    shas = {"upstream": document["upstream"]["sha"]}
    shas.update({name: source["sha"] for name, source in document["feature_sources"].items()})
    for name, sha in shas.items():
        if _run_git(repo_root, ["cat-file", "-e", f"{sha}^{{commit}}"]).returncode != 0:
            errors.append(_issue("git.missing_commit", f"locked {name} commit is not available: {sha}"))

    upstream_sha = document["upstream"]["sha"]
    if _run_git(repo_root, ["merge-base", "--is-ancestor", upstream_sha, head]).returncode != 0:
        errors.append(_issue("git.upstream_ancestor", f"locked upstream {upstream_sha} is not an ancestor of HEAD {head}"))

    for name, source in document["feature_sources"].items():
        branch = source["branch"]
        sha = source["sha"]
        candidates = (f"refs/remotes/origin/{branch}", f"refs/heads/{branch}")
        available = [ref for ref in candidates if _git_ref_exists(repo_root, ref)]
        if not available:
            errors.append(_issue("git.missing_source_ref", f"no local origin or branch ref is available for {branch}"))
            continue
        if not any(_run_git(repo_root, ["merge-base", "--is-ancestor", sha, ref]).returncode == 0 for ref in available):
            errors.append(_issue("git.source_ancestry", f"locked {name} commit {sha} is not in {branch} history"))

    receipt_errors, receipt_details = validate_integration_receipts(document, repo_root, head)
    errors.extend(receipt_errors)
    details["integration_receipts"] = receipt_details
    return errors, details


def _source_requirements(document: dict[str, Any]) -> tuple[tuple[str, str, str], ...]:
    runtime = document["runtime_contract"]
    version = re.escape(runtime["sidecar_version"])
    port = runtime["product_port"]
    protocol = runtime["protocol_version"]
    return (
        (
            "tools/ai/orca_ai_sidecar.py",
            rf'^PORT\s*=\s*int\(os\.environ\.get\("ORCASLICER_AI_SIDECAR_PORT",\s*"{port}"\)\)',
            "Sidecar default port",
        ),
        (
            "tools/ai/orca_ai_sidecar.py",
            rf'^SIDECAR_VERSION\s*=\s*"{version}"',
            "Sidecar version",
        ),
        (
            "tools/ai/orca_ai_sidecar.py",
            rf'"protocol_version"\s*:\s*{protocol}\b',
            "Sidecar protocol",
        ),
        (
            "src/slic3r/GUI/AIServiceManager.cpp",
            rf'DEFAULT_LOCAL_ENDPOINT\s*=\s*"http://127\.0\.0\.1:{port}"',
            "service-manager endpoint",
        ),
        (
            "src/slic3r/GUI/AIServiceManager.cpp",
            rf'EXPECTED_PROTOCOL_VERSION\s*=\s*{protocol}\b',
            "service-manager protocol",
        ),
        (
            "src/slic3r/GUI/AIServiceManager.cpp",
            rf'EXPECTED_SIDECAR_VERSION\s*=\s*"{version}"',
            "service-manager Sidecar version",
        ),
        (
            "src/slic3r/GUI/AISidecarClient.cpp",
            rf'return\s+"http://127\.0\.0\.1:{port}"',
            "Sidecar client endpoint",
        ),
        (
            "src/slic3r/GUI/AISidecarClient.cpp",
            r'request\.local_only\(\);',
            "native Sidecar local-only transport",
        ),
        (
            "src/slic3r/GUI/AISidecarClient.cpp",
            r'X-OrcaSlicer-Session-Proof',
            "native Sidecar session proof",
        ),
        (
            "src/slic3r/Utils/Http.cpp",
            r'p->follow_redirects\s*=\s*false;',
            "local HTTP redirect denial",
        ),
        (
            "src/slic3r/Utils/Http.cpp",
            r'CURLOPT_NOPROXY,\s*"\*"',
            "local HTTP proxy bypass",
        ),
        (
            "src/slic3r/Utils/Http.cpp",
            r'CURLOPT_PROXY,\s*""',
            "local HTTP explicit proxy disable",
        ),
        (
            "tools/ai/orca_ai_sidecar.py",
            r'X-OrcaSlicer-Session-Proof',
            "Sidecar request proof validation",
        ),
        (
            "tools/ai/orca_ai_installed_bootstrap.py",
            r'os\.environ\["ORCASLICER_AI_REQUIRE_SESSION"\]\s*=\s*"1"',
            "installed Sidecar session requirement",
        ),
        (
            "tools/ai/orca_ai_installed_bootstrap.py",
            r'build_identity_invalid',
            "installed Sidecar build identity gate",
        ),
        (
            "tools/ai/orca_ai_sidecar.py",
            r'def\s+_monitor_parent\s*\(',
            "installed Sidecar parent lifetime monitor",
        ),
        (
            "src/slic3r/GUI/AIServiceManager.cpp",
            r'process::environment\s+child_environment\s*;',
            "Sidecar child environment allowlist",
        ),
        (
            "src/slic3r/GUI/AIServiceManager.cpp",
            r'ORCASLICER_AI_PARENT_PID',
            "Sidecar parent identity propagation",
        ),
        (
            "CMakeLists.txt",
            r'ORCA_AI_RUNTIME_DEPENDENCIES_FILE',
            "bundled runtime dependency manifest",
        ),
        (
            "src/CMakeLists.txt",
            r'verify_bundled_runtime\.py',
            "installed runtime verification",
        ),
        (
            "CMakeLists.txt",
            r'Commercial AI packages require an exact 40-character source commit',
            "commercial package source identity fail-closed gate",
        ),
        (
            ".github/workflows/build_all.yml",
            r"vars\.SELF_HOSTED\s*&&\s*github\.event_name\s*!=\s*'pull_request'",
            "pull-request self-hosted runner denial",
        ),
        (
            "tools/ai/orca_ai_sidecar.py",
            r'/v1/orcaslicer/shutdown',
            "authenticated Sidecar graceful shutdown",
        ),
        (
            "tools/ai/orca_ai_sidecar.py",
            r'_restore_jobs\(resume_jobs=False\)',
            "deferred remote-job recovery",
        ),
    )


FORBIDDEN_SOURCE_PATTERNS = (
    (
        "src/slic3r/GUI/AIServiceManager.cpp",
        r'boost::this_process::environment\s*\(',
        "Sidecar child must not inherit the complete desktop environment",
    ),
    (
        ".github/workflows/build_all.yml",
        r'secrets:\s*inherit',
        "general build workflow must not inherit repository secrets",
    ),
    (
        ".github/workflows/build_check_cache.yml",
        r'secrets:\s*inherit',
        "cache/build workflow must not forward repository secrets",
    ),
    (
        ".github/workflows/build_deps.yml",
        r'secrets:\s*inherit',
        "dependency/build workflow must not forward repository secrets",
    ),
    (
        ".github/workflows/build_orca.yml",
        r'ORCA_UPDATER_SIG_KEY',
        "general build job must not expose updater signing credentials",
    ),
)


def validate_source_constants(document: dict[str, Any], repo_root: Path) -> list[dict[str, str]]:
    errors: list[dict[str, str]] = []
    for relative_path, pattern, label in _source_requirements(document):
        path = repo_root / relative_path
        try:
            content = path.read_text(encoding="utf-8")
        except OSError as exc:
            errors.append(_issue("source.missing", f"cannot read {relative_path}: {exc}"))
            continue
        if re.search(pattern, content, flags=re.MULTILINE) is None:
            errors.append(_issue("source.constant", f"{label} does not match the lock in {relative_path}"))
    for relative_path, pattern, label in FORBIDDEN_SOURCE_PATTERNS:
        path = repo_root / relative_path
        try:
            content = path.read_text(encoding="utf-8")
        except OSError:
            continue
        if re.search(pattern, content, flags=re.MULTILINE) is not None:
            errors.append(_issue("source.forbidden", f"{label}: {relative_path}"))
    return errors


def _cpp_files(path: Path) -> Iterable[Path]:
    if path.is_file():
        if path.suffix.lower() in {".cc", ".cpp", ".h", ".hpp"}:
            yield path
        return
    if path.is_dir():
        for candidate in sorted(path.rglob("*")):
            if candidate.is_file() and candidate.suffix.lower() in {".cc", ".cpp", ".h", ".hpp"}:
                yield candidate


def _dependency_issue(repo_root: Path, path: Path, line_number: int, rule: str) -> dict[str, str]:
    relative_path = path.relative_to(repo_root).as_posix()
    return _issue("boundary.dependency", f"{relative_path}:{line_number} violates {rule}")


def validate_dependency_boundaries(repo_root: Path) -> list[dict[str, str]]:
    """Scan the dependency directions that keep the feature lanes independent."""
    errors: list[dict[str, str]] = []

    model_locations = (
        repo_root / "src/slic3r/AI/ModelGeneration",
        repo_root / "src/slic3r/GUI/AIModelGenerationClient.cpp",
        repo_root / "src/slic3r/GUI/AIModelGenerationClient.hpp",
        repo_root / "src/slic3r/GUI/ModelGenerationPanel.cpp",
        repo_root / "src/slic3r/GUI/ModelGenerationPanel.hpp",
    )
    for location in model_locations:
        for path in _cpp_files(location):
            content = path.read_text(encoding="utf-8", errors="replace")
            for line_number, line in enumerate(content.splitlines(), 1):
                normalized = line.replace("\\", "/")
                allowed_contract = "SmartSlicing/IModelArtifactConsumer.hpp" in normalized
                if ("SmartSlicing/" in normalized and not allowed_contract) or "SmartSlicing::" in normalized:
                    errors.append(
                        _dependency_issue(
                            repo_root,
                            path,
                            line_number,
                            "model-generation isolation (only IModelArtifactConsumer is allowed)",
                        )
                    )

    include_pattern = re.compile(r'^\s*#\s*include\s*[<"]([^">]+)[">]')
    forbidden_domain_include = re.compile(r"(^|/)(wx|gui)(/|$)|plater|provider|tripo|openai", re.IGNORECASE)
    for layer in ("Domain", "Application"):
        location = repo_root / "src/slic3r/AI/SmartSlicing" / layer
        for path in _cpp_files(location):
            content = path.read_text(encoding="utf-8", errors="replace")
            for line_number, line in enumerate(content.splitlines(), 1):
                match = include_pattern.match(line)
                if match and forbidden_domain_include.search(match.group(1).replace("\\", "/")):
                    errors.append(
                        _dependency_issue(
                            repo_root,
                            path,
                            line_number,
                            "SmartSlicing Domain/Application include isolation",
                        )
                    )

    smart_gui = repo_root / "src/slic3r/GUI/AI/SmartSlicing"
    forbidden_gui_reference = re.compile(
        r"ModelGenerationPanel|model_provider_gateway|openai_preprocessor|tripo_client|tools[/\\]ai",
        re.IGNORECASE,
    )
    for path in _cpp_files(smart_gui):
        content = path.read_text(encoding="utf-8", errors="replace")
        for line_number, line in enumerate(content.splitlines(), 1):
            if forbidden_gui_reference.search(line):
                errors.append(
                    _dependency_issue(repo_root, path, line_number, "SmartSlicing GUI provider/model-panel isolation")
                )
    return errors


def validate(lock_path: Path, repo_root: Path, skip_git: bool = False) -> dict[str, Any]:
    report: dict[str, Any] = {
        "ok": False,
        "schema": SCHEMA,
        "lock_file": str(lock_path),
        "git_checks_skipped": skip_git,
        "errors": [],
    }
    try:
        document = json.loads(lock_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        report["errors"] = [_issue("lock.read", f"cannot read lock file: {exc}")]
        return report

    errors = validate_document(document)
    if not errors:
        errors.extend(validate_source_constants(document, repo_root))
        errors.extend(validate_dependency_boundaries(repo_root))
        if not skip_git:
            git_errors, git_details = validate_git(document, repo_root)
            errors.extend(git_errors)
            report["git"] = git_details
    report["errors"] = errors
    report["ok"] = not errors
    return report


def _default_repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


def main(argv: Sequence[str] | None = None) -> int:
    repo_root = _default_repo_root()
    parser = argparse.ArgumentParser(description="Validate OrcaSlicer AI integration pins and boundaries.")
    parser.add_argument(
        "--lock-file",
        type=Path,
        default=repo_root / "docs" / "architecture" / "ai-integration-lock.json",
        help="Path to the integration lock JSON.",
    )
    parser.add_argument("--skip-git", action="store_true", help="Skip commit/object/ancestry checks.")
    parser.add_argument("--json", action="store_true", help="Emit a machine-readable JSON report.")
    args = parser.parse_args(argv)

    report = validate(args.lock_file.resolve(), repo_root, skip_git=args.skip_git)
    if args.json:
        print(json.dumps(report, ensure_ascii=False, sort_keys=True))
    elif report["ok"]:
        print("AI integration guardrails: PASS")
        if args.skip_git:
            print("Git checks: skipped by request")
        elif report.get("git", {}).get("head"):
            print(f"HEAD: {report['git']['head']}")
    else:
        print("AI integration guardrails: FAIL", file=sys.stderr)
        for error in report["errors"]:
            print(f"- [{error['code']}] {error['message']}", file=sys.stderr)
    return 0 if report["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
