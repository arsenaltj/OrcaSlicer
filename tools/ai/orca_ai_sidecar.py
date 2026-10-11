#!/usr/bin/env python3
from __future__ import annotations
import atexit
import hashlib
import hmac
import json
import math
import os
import re
import secrets
import shutil
import socket
import ssl
import stat
import sys
import tempfile
import threading
import time
import traceback
import uuid
import zipfile
from ai_diagnostics import (
    diagnostic_context,
    event as diagnostic_event,
    exception_details,
    safe_endpoint,
)
from array import array
from capability_catalog import build_catalog
from collections import Counter, deque
from color_intent import (
    COLOR_INTENT_FILENAME,
    ColorIntentError,
    MAX_MANIFEST_BYTES as MAX_COLOR_INTENT_BYTES,
    SCHEMA_ID as COLOR_INTENT_SCHEMA,
    verify_color_intent_manifest_file,
    write_color_intent_manifest,
)
from concurrent.futures import Future, ThreadPoolExecutor
from config_proposal import (
    build_system_prompt,
    build_user_payload,
    extract_allowed_keys,
    extract_json_object,
    normalize_proposal,
)
from dataclasses import asdict, dataclass, field
from design_generation_timing import DesignTimingHistory, seconds as timing_seconds, timing_key
from design_workflow import DesignWorkflow, DesignWorkflowPorts
from email import policy
from email.parser import BytesParser
from glb_artifact import Glb, GlbError, prepare_generated_glb, write_analysis_obj
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from hunyuan_provider_gateway import (
    HunyuanModelProviderGateway,
    validate_options as validate_hunyuan_options,
)
from io import BytesIO
from model_artifact_workflow import ModelArtifactWorkflow, ModelArtifactWorkflowPorts
from model_color_regions import (
    _color_boundary_metrics,
    _consolidate_tiny_obj_color_components,
    _regularize_obj_color_boundaries,
)
from model_color_space import (
    _nearest_palette_index,
    _palette_data,
    _portrait_material_palette_indices,
    _portrait_material_role_indices,
    _semantic_palette_index,
    _srgb_to_lab,
    _vertex_color_data,
)
from model_contracts import (
    ApplicationResult,
    DEFAULT_GENERATION_PROFILE,
    DEFAULT_IMAGE_INSTRUCTION,
    DEFAULT_MODEL_FACE_LIMIT,
    DEFAULT_MODEL_SIZE_MM,
    DEFAULT_PALETTE_COLORS,
    GENERATION_PROFILES,
    GENERATION_PROFILE_FACE_LIMITS,
    JOB_STATE_FILENAME,
    JOB_STATE_VERSION,
    JOURNEY_EVENT_FILENAME,
    JOURNEY_EVENT_NAMES,
    Job,
    JobStopped,
    LEGACY_STYLE_ALIASES,
    MAX_ARCHIVE_FILES,
    MAX_ARTIFACT_BYTES,
    MAX_CHANGES,
    MAX_COLOR_BOUNDARY_PASSES,
    MAX_COLOR_BOUNDARY_SOURCE_AREA_RATIO,
    MAX_COLOR_BOUNDARY_SOURCE_NEIGHBORS,
    MAX_COLOR_BOUNDARY_SURFACE_AREA_RATIO,
    MAX_COLOR_CLEANUP_PASSES,
    MAX_COLOR_CLEANUP_SOURCE_AREA_RATIO,
    MAX_COLOR_CLEANUP_SURFACE_AREA_RATIO,
    MAX_CUSTOM_STYLE_BYTES,
    MAX_GENERATION_ATTEMPTS,
    MAX_IMAGE_BYTES,
    MAX_JOB_STATE_BYTES,
    MAX_JOURNEY_EVENT_FILE_BYTES,
    MAX_LOCAL_BOUNDARY_EDGES,
    MAX_LOCAL_REPAIR_DIAGONAL_RATIO,
    MAX_LOCAL_REPAIR_FACE_RATIO,
    MAX_MULTIPART_BYTES,
    MAX_NOISE_COMPONENT_DIAGONAL_RATIO,
    MAX_NOISE_COMPONENT_FACE_RATIO,
    MAX_PALETTE_COLORS,
    MAX_PROMPT_BYTES,
    MAX_REQUEST_BYTES,
    MAX_TEXTURE_PIXELS,
    MAX_TINY_COLOR_COMPONENT_AREA_RATIO,
    MAX_TINY_COLOR_COMPONENT_VERTEX_RATIO,
    MAX_UNPACKED_BYTES,
    MEANINGFUL_COLOR_SURFACE_AREA_RATIO,
    MIN_COLOR_BOUNDARY_SUPPORT_RATIO,
    MIN_MODEL_REFERENCE_EDGE,
    MIN_PALETTE_COLORS,
    MIN_SOURCE_IMAGE_EDGE,
    MODEL_ARTIFACT_FORMAT,
    MODEL_FACE_LIMITS,
    MODEL_QUALITY_FILENAME,
    PORTRAIT_FACE_DETAIL_GRID_SIZE,
    PORTRAIT_FACE_DETAIL_HALF_WIDTH_RATIO,
    PORTRAIT_FACE_DETAIL_MAX_HEIGHT_RATIO,
    PORTRAIT_FACE_DETAIL_MIN_HEIGHT_RATIO,
    PORTRAIT_FACE_DETAIL_SURFACE_TOLERANCE_MM,
    PORTRAIT_FRONT_SURFACE_QUANTILE,
    PORTRAIT_GARMENT_SMOOTHING_PASSES,
    PORTRAIT_GEOMETRY_MAX_SUBJECT_OCCUPANCY,
    PORTRAIT_GEOMETRY_PROVIDER_FILENAME,
    PORTRAIT_HAND_BOUNDARY_MAX_REMOVAL_RATIO,
    PORTRAIT_HAND_BOUNDARY_MIN_PRIMARY_SUPPORT,
    PORTRAIT_HAND_BOUNDARY_PASSES,
    PORTRAIT_HAND_COMPACT_EXTENT_RATIO,
    PORTRAIT_HAND_DIFFUSE_SIZE_RATIO,
    PORTRAIT_HAND_MIN_HEIGHT_RATIO,
    PORTRAIT_HEAD_GEOMETRY_MAX_SUBJECT_OCCUPANCY,
    PORTRAIT_HEAD_PREVIEW_FILENAME,
    PORTRAIT_REAR_GARMENT_HEIGHT_RATIO,
    PORTRAIT_REAR_HAIR_HEIGHT_RATIO,
    PORTRAIT_REAR_PLATE_MAX_START_RATIO,
    PORTRAIT_REAR_PLATE_MIN_RUN_RATIO,
    PORTRAIT_STRUCTURE_FRONT_QUANTILE,
    PortraitGeometryGateError,
    PortraitMultiviewPreparationError,
    RequestError,
    STYLE_IDS,
    SidecarRestart,
)
from model_creation_policy import (
    _complete_design_timing,
    _public_design_timing,
    _use_unrestricted_creation,
)
from model_generation_workflow import ModelGenerationWorkflow, ModelGenerationWorkflowPorts
from model_input_image_quality import (
    ModelInputImageQualityError,
    assess_model_input_image,
    recommend_printable_style,
)
from model_job_application import ModelJobApplication, ModelJobApplicationPorts
from model_job_lifecycle import ModelJobLifecycle, ModelJobLifecyclePorts
from model_job_repository import (
    _copy_job_file,
    _job_file,
    _job_path_value,
    _load_job,
    _persist_job,
)
from model_job_support import (
    MAX_MODEL_FACES,
    MAX_MODEL_FACE_RATIO,
    MIN_MODEL_FACE_RATIO,
    apply_legacy_material_fragmentation_gate as _apply_legacy_material_fragmentation_gate,
    assess_job_model_reference as _assess_job_model_reference,
    file_info as _file_info,
    generation_prompt as _generation_prompt,
    image_type as _image_type,
    legacy_face_error_is_recoverable as _legacy_face_error_is_recoverable,
    mark_prepaid_multiview_retry as _mark_prepaid_multiview_retry,
    model_input_quality_message as _model_input_quality_message,
    preprocess_failure_payload,
    printable_preview_message as _printable_preview_message,
    stale_high_quality_face_gate_is_recoverable as _stale_high_quality_face_gate_is_recoverable,
    stored_image_type as _stored_image_type,
    validate_face_target as _validate_face_target,
)
from model_mesh_repair import (
    _remove_small_detached_obj_components,
    _repair_small_obj_topology_defects,
)
from model_obj_io import (
    _extract_obj_package,
    _normalize_obj_for_orca,
    _obj_dependency_path,
    _obj_triangle_area,
    _obj_vertex_color_metrics,
    _read_material_textures,
    _read_obj_geometry,
    _resolve_obj_index,
    _safe_package_path,
    _validate_artifact,
    _validate_obj_palette,
    _validate_obj_topology,
    _validate_obj_vertex_colors,
    _write_mesh_repair_report,
    _write_obj_vertex_color_metrics,
)
from model_provider_gateway import (
    ModelProviderGateway,
    ModelTaskRequest,
    PaidTaskAuthorization,
    ProviderGatewayError,
    TextureTaskRequest,
    provider_policy,
)
from model_quality_workflow import ModelQualityWorkflow, ModelQualityWorkflowPorts
from model_refinement import build_model_refinement_advice
from model_request import (
    ValidatedImage,
    _boolean_field,
    _generation_options,
    _generation_provider,
    _multipart_palette,
    _multipart_palette_roles,
    _normalize_custom_style,
    _normalize_face_limit,
    _normalize_generation_profile,
    _normalize_image_instruction,
    _normalize_palette,
    _normalize_palette_color_count,
    _normalize_palette_recommendation,
    _normalize_palette_roles,
    _normalize_print_settings,
    _normalize_style,
    _text_field,
    _user_image_instruction,
    _validate_image_data,
    _validate_image_file,
)
from model_texture_baking import _bake_obj_texture_to_vertex_colors, _quantize_vertex_color_obj
from network_policy import network_diagnostics
from nonportrait_reference import review_nonportrait_reference
from openai_preprocessor import (
    IDENTITY_FIRST_PORTRAIT_STYLES,
    OpenAIPreprocessorError,
    PORTRAIT_FACE_LOCK_FILENAME,
    complete_text,
    complete_vision,
    complete_vision_once,
    edit_image,
    generate_geometry_reference_image,
    image_preprocessing_policy,
    image_provider_status,
    preprocess_image,
    recommend_printable_palette,
)
from pathlib import Path
from portrait_geometry_reference import (
    _copy_portrait_into_continuous_silhouette,
    _geometry_generation_reference,
    _identity_preserving_portrait_geometry_enabled,
    _model_generation_reference,
    _prepare_portrait_geometry_provider_reference,
    _refine_portrait_head_silhouette,
    _repair_portrait_head_shoulders_silhouette,
    _synchronize_geometry_reference_alpha,
)
from portrait_model_materials import (
    _capture_portrait_front_face_details,
    _restore_portrait_front_face_details,
    _review_portrait_rear_plate_masks,
    _stabilize_portrait_obj_garment_regions,
    _stabilize_portrait_obj_materials,
)
from portrait_multiview_cleanup import (
    PortraitProjectionError,
    project_front_portrait_materials,
    project_geometry_aligned_portrait_materials,
    quantize_geometry_aligned_material_reference,
)
from portrait_multiview_workflow import PortraitMultiviewWorkflow, PortraitMultiviewWorkflowPorts
from printable_image_pipeline import (
    PrintSettings,
    PrintableImageError,
    _portrait_source_subject_mask,
    process_printable_image,
)
from printable_model_quality import (
    GATE_VERSION as MODEL_QUALITY_GATE_VERSION,
    ModelQualityError,
    ModelQualityThresholds,
    analyze_printable_obj,
    write_model_quality_report,
)
from printable_model_views import ModelViewError, ModelViewSettings, render_model_views
from printable_multiview_reference import (
    HIGH_QUALITY_PORTRAIT_CANVAS_SIZE,
    MULTIVIEW_NORMALIZATION_VERSION,
    MultiviewReferenceError,
    PORTRAIT_MATERIAL_GATE_VERSION,
    VIEW_ORDER as MULTIVIEW_ORDER,
    build_multiview_input_sheet,
    evaluate_multiview_review_acceptance,
    evaluate_portrait_material_gate,
    normalize_multiview_inputs,
    process_multiview_crops,
    review_multiview_sheet,
    split_multiview_sheet,
    write_multiview_manifest,
)
from printable_palette import (
    LEGACY_DEFAULT_PRINTABLE_COLORS,
    MAX_PRINTABLE_COLORS,
    MIN_PRINTABLE_COLORS,
    PrintablePaletteError,
    active_palette_roles,
    assign_palette_roles,
    normalize_palette_color_count,
)
from printable_reference_visual_quality import review_prepared_reference
from printable_visual_quality import (
    REPORT_FILENAME as VISUAL_QUALITY_FILENAME,
    review_model_visual_quality,
)
from tripo_client import (
    TripoError,
    validate_generation_option_values,
    validate_generation_options,
)
from typing import Any, BinaryIO, Callable, Mapping


# Compatibility exports for existing consumers; implementation lives in the atoms.

_MODEL_PROVIDER_GATEWAY = ModelProviderGateway()
_HUNYUAN_PROVIDER_GATEWAY = HunyuanModelProviderGateway()


def _model_gateway(provider: str):
    return _HUNYUAN_PROVIDER_GATEWAY if provider == "hunyuan" else _MODEL_PROVIDER_GATEWAY

HOST = os.environ.get("ORCASLICER_AI_SIDECAR_HOST", "127.0.0.1")
PORT = int(os.environ.get("ORCASLICER_AI_SIDECAR_PORT", "18764"))
SIDECAR_VERSION = "orcaslicer-ai-sidecar-v9"
SIDECAR_INSTANCE_ID = str(uuid.uuid4())
SIDECAR_SESSION_NONCE = secrets.token_hex(32)
_LOOPBACK_HOSTS = {"127.0.0.1", "localhost", "::1"}
_JOBS_LOCK = threading.RLock()
_JOBS: dict[str, "Job"] = {}
_JOURNEY_EVENT_LOCK = threading.Lock()
_DESIGN_EXECUTOR = ThreadPoolExecutor(max_workers=1, thread_name_prefix="orca-design-job")
_MODEL_EXECUTOR = ThreadPoolExecutor(max_workers=1, thread_name_prefix="orca-model-job")
_SHUTDOWN_LOCK = threading.Lock()
_SHUT_DOWN = False


def _environment_flag(name: str) -> bool:
    return os.environ.get(name, "").strip().lower() in {"1", "true", "yes", "on"}


def _preprocess_fallback_enabled() -> bool:
    return _environment_flag("ORCASLICER_AI_ALLOW_PREPROCESS_FALLBACK")


def _runtime_network_metadata() -> dict[str, dict[str, object]]:
    image_endpoint = (
        os.environ.get("OPENAI_PRO_URL", "").strip()
        or os.environ.get("OPENAI_BASE_URL", "https://api.openai.com/v1")
    )
    return {
        "openai": network_diagnostics(
            os.environ.get("OPENAI_BASE_URL", "https://api.openai.com/v1")
        ),
        "image2": network_diagnostics(image_endpoint),
        "tripo": network_diagnostics(
            os.environ.get("TRIPO_API_BASE", "https://openapi.tripo3d.com/v3")
        ),
    }


def _configured_session_token() -> str | None:
    value = os.environ.get("ORCASLICER_AI_SESSION_TOKEN", "")
    if not value:
        return ""
    return value if re.fullmatch(r"[0-9A-Fa-f]{64}", value) else None


def _session_required() -> bool:
    return (
        _environment_flag("ORCASLICER_AI_REQUIRE_SESSION")
        or os.environ.get("ORCASLICER_AI_CONFIG_MODE") == "internal_locked"
        or os.environ.get("ORCASLICER_AI_DISTRIBUTION_CHANNEL") in {"internal", "commercial"}
    )


def _session_hmac(token: str, message: str) -> str:
    return hmac.new(token.encode("ascii"), message.encode("ascii"), "sha256").hexdigest()


def _safe_runtime_identity() -> dict[str, str]:
    version = os.environ.get("ORCASLICER_AI_APP_VERSION", "unknown")
    if not re.fullmatch(r"[0-9A-Za-z._+-]{1,128}", version):
        version = "unknown"
    commit = os.environ.get("ORCASLICER_AI_APP_COMMIT", "unknown")
    if commit != "unknown" and not re.fullmatch(r"[0-9A-Fa-f]{40}", commit):
        commit = "unknown"
    revision = os.environ.get("ORCASLICER_AI_PACKAGE_REVISION", "unknown")
    if not re.fullmatch(r"[0-9A-Za-z._-]{1,128}", revision):
        revision = "unknown"
    channel = os.environ.get("ORCASLICER_AI_DISTRIBUTION_CHANNEL", "developer")
    if channel not in {"developer", "internal", "commercial"}:
        channel = "developer"
    return {
        "application_version": version,
        "application_commit": commit,
        "package_revision": revision,
        "distribution_channel": channel,
    }


def _configured_parent_pid() -> int | None:
    value = os.environ.get("ORCASLICER_AI_PARENT_PID", "").strip()
    if not value:
        return None
    if not value.isascii() or not value.isdecimal():
        return None
    parent_pid = int(value)
    return parent_pid if 0 < parent_pid <= 0xFFFFFFFF else None


def _parent_process_alive(parent_pid: int) -> bool:
    if os.name == "nt":
        # os.kill(pid, 0) is not a harmless existence probe on Windows. Query a
        # synchronize-only process handle and never inherit it into children.
        import ctypes

        synchronize = 0x00100000
        wait_timeout = 0x00000102
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.OpenProcess.argtypes = (ctypes.c_uint32, ctypes.c_int, ctypes.c_uint32)
        kernel32.OpenProcess.restype = ctypes.c_void_p
        kernel32.WaitForSingleObject.argtypes = (ctypes.c_void_p, ctypes.c_uint32)
        kernel32.WaitForSingleObject.restype = ctypes.c_uint32
        kernel32.CloseHandle.argtypes = (ctypes.c_void_p,)
        kernel32.CloseHandle.restype = ctypes.c_int
        handle = kernel32.OpenProcess(synchronize, False, parent_pid)
        if not handle:
            return False
        try:
            return kernel32.WaitForSingleObject(handle, 0) == wait_timeout
        finally:
            kernel32.CloseHandle(handle)

    try:
        os.kill(parent_pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


def _open_parent_process_handle(parent_pid: int) -> int | None:
    if os.name != "nt":
        return None
    import ctypes

    synchronize = 0x00100000
    wait_timeout = 0x00000102
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.OpenProcess.argtypes = (ctypes.c_uint32, ctypes.c_int, ctypes.c_uint32)
    kernel32.OpenProcess.restype = ctypes.c_void_p
    kernel32.WaitForSingleObject.argtypes = (ctypes.c_void_p, ctypes.c_uint32)
    kernel32.WaitForSingleObject.restype = ctypes.c_uint32
    kernel32.CloseHandle.argtypes = (ctypes.c_void_p,)
    kernel32.CloseHandle.restype = ctypes.c_int
    handle = kernel32.OpenProcess(synchronize, False, parent_pid)
    if not handle:
        return None
    if kernel32.WaitForSingleObject(handle, 0) != wait_timeout:
        kernel32.CloseHandle(handle)
        return None
    return int(handle)


def _close_parent_process_handle(handle: int | None) -> None:
    if os.name != "nt" or handle is None:
        return
    import ctypes

    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.CloseHandle.argtypes = (ctypes.c_void_p,)
    kernel32.CloseHandle.restype = ctypes.c_int
    kernel32.CloseHandle(handle)


def _parent_process_handle_alive(handle: int) -> bool:
    import ctypes

    wait_timeout = 0x00000102
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.WaitForSingleObject.argtypes = (ctypes.c_void_p, ctypes.c_uint32)
    kernel32.WaitForSingleObject.restype = ctypes.c_uint32
    return kernel32.WaitForSingleObject(handle, 0) == wait_timeout


def _monitor_parent(
    server: ThreadingHTTPServer,
    parent_pid: int,
    parent_handle: int | None = None,
) -> None:
    if os.name == "nt":
        # Keep one synchronize-only handle for the whole Sidecar lifetime. A
        # handle continues to identify the original Orca process even if its PID
        # is later reused by Windows.
        import ctypes

        synchronize = 0x00100000
        wait_object_0 = 0x00000000
        wait_timeout = 0x00000102
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.OpenProcess.argtypes = (ctypes.c_uint32, ctypes.c_int, ctypes.c_uint32)
        kernel32.OpenProcess.restype = ctypes.c_void_p
        kernel32.WaitForSingleObject.argtypes = (ctypes.c_void_p, ctypes.c_uint32)
        kernel32.WaitForSingleObject.restype = ctypes.c_uint32
        kernel32.CloseHandle.argtypes = (ctypes.c_void_p,)
        kernel32.CloseHandle.restype = ctypes.c_int
        handle = parent_handle or kernel32.OpenProcess(synchronize, False, parent_pid)
        if not handle:
            diagnostic_event("sidecar.parent.unavailable", level="ERROR", parent_pid=parent_pid)
            server.shutdown()
            return
        try:
            while not _SHUT_DOWN:
                result = kernel32.WaitForSingleObject(handle, 2000)
                if result == wait_timeout:
                    continue
                event = "sidecar.parent.exited" if result == wait_object_0 else "sidecar.parent.wait_failed"
                diagnostic_event(event, level="INFO" if result == wait_object_0 else "ERROR", parent_pid=parent_pid)
                server.shutdown()
                return
        finally:
            kernel32.CloseHandle(handle)
        return

    while not _SHUT_DOWN:
        if not _parent_process_alive(parent_pid):
            diagnostic_event("sidecar.parent.exited", parent_pid=parent_pid)
            server.shutdown()
            return
        time.sleep(2.0)


def provider_request(request: dict[str, Any]) -> dict[str, Any]:
    try:
        content = complete_text(
            build_system_prompt(request),
            json.dumps(build_user_payload(request), ensure_ascii=False, separators=(",", ":")),
        )
    except OpenAIPreprocessorError as exc:
        raise RuntimeError(str(exc)) from None
    return extract_json_object(content)


def _model_output_root() -> Path:
    return Path(os.environ.get("ORCASLICER_AI_OUTPUT_DIR", Path.cwd() / "generated_models")).resolve()


def _record_journey_event(request: dict[str, Any]) -> dict[str, Any]:
    if set(request) - {"event", "job_id"}:
        raise RequestError(
            "invalid_journey_event",
            "Journey events only accept event and job_id.",
            400,
        )
    event = request.get("event")
    if not isinstance(event, str) or event not in JOURNEY_EVENT_NAMES:
        raise RequestError("invalid_journey_event", "Journey event is not allowed.", 400)
    job_id = request.get("job_id", "")
    if not isinstance(job_id, str):
        raise RequestError("invalid_journey_event", "job_id must be a UUID string.", 400)
    if job_id:
        try:
            parsed_job_id = uuid.UUID(job_id)
        except ValueError:
            raise RequestError("invalid_journey_event", "job_id must be a UUID string.", 400) from None
        if str(parsed_job_id) != job_id.lower():
            raise RequestError("invalid_journey_event", "job_id must be a canonical UUID string.", 400)
        job_id = job_id.lower()

    record = {
        "version": 1,
        "event": event,
        "job_id": job_id,
        "recorded_at": time.time(),
    }
    encoded = json.dumps(record, ensure_ascii=True, separators=(",", ":")) + "\n"
    output_root = _model_output_root()
    destination = output_root / JOURNEY_EVENT_FILENAME
    rotated = output_root / f"{JOURNEY_EVENT_FILENAME}.1"
    try:
        with _JOURNEY_EVENT_LOCK:
            output_root.mkdir(parents=True, exist_ok=True)
            if destination.is_file() and destination.stat().st_size + len(encoded) > MAX_JOURNEY_EVENT_FILE_BYTES:
                rotated.unlink(missing_ok=True)
                os.replace(destination, rotated)
            with destination.open("a", encoding="ascii", newline="\n") as stream:
                stream.write(encoded)
    except OSError:
        raise RequestError("journey_event_unavailable", "Local journey event log is unavailable.", 503) from None
    return record


def _new_job(
    source: str,
    palette: tuple[str, ...] = (),
    palette_roles: dict[str, str] | None = None,
    style: str = "sculpture",
    custom_style: str = "",
    print_settings: dict[str, Any] | None = None,
    palette_color_count: int | None = None,
    provider: str = "tripo",
    generation_options: dict[str, Any] | None = None,
) -> Job:
    return _model_job_lifecycle().new_job(source, palette, palette_roles, style, custom_style, print_settings, palette_color_count, provider, generation_options)


def _reuse_design_job(source: Job) -> Job:
    return _model_job_lifecycle().reuse_design_job(source)


def _restore_jobs(*, resume_jobs: bool = True) -> list[Job]:
    return _model_job_lifecycle().restore_jobs(resume_jobs=resume_jobs)


def _resume_restored_jobs(restored: list[Job]) -> None:
    return _model_job_lifecycle().resume_restored_jobs(restored)


def _check_saved_model(request: Mapping[str, Any]) -> dict[str, Any]:
    return _model_quality_workflow().check_saved_model(request)


def _adopt_legacy_completed_job(job_id: str) -> Job | None:
    return _model_job_lifecycle().adopt_legacy_completed_job(job_id)


def _latest_job_is_restorable(job: Job) -> bool:
    return _model_job_lifecycle().latest_job_is_restorable(job)


def _read_job_report(job: Job, filename: str) -> dict[str, Any]:
    return _model_job_lifecycle().read_job_report(job, filename)


def _public_job(job: Job) -> dict[str, Any]:
    return _model_job_lifecycle().public_job(job)


def _cleanup_job(job: Job) -> None:
    # Job removal only releases in-memory state. Generated inputs, previews, and
    # model resources are user artifacts and remain available on disk.
    return _model_job_lifecycle().cleanup_job(job)


def _remove_job_state(job: Job) -> None:
    return _model_job_lifecycle().remove_job_state(job)


def _finish_deleted(job: Job) -> None:
    return _model_job_lifecycle().finish_deleted(job)


def _clear_job_artifact(job: Job) -> None:
    return _model_job_lifecycle().clear_job_artifact(job)


def _mark_stopped(job: Job) -> None:
    return _model_job_lifecycle().mark_stopped(job)


def _stop_boundary(job: Job) -> None:
    return _model_job_lifecycle().stop_boundary(job)


def _fail_job(job: Job, message: str) -> None:
    return _model_job_lifecycle().fail_job(job, message)


def _return_to_portrait_multiview_retry(job: Job, message: str) -> None:
    return _model_job_lifecycle().return_to_portrait_multiview_retry(job, message)


def _fail_preprocess_job(job: Job, error: OpenAIPreprocessorError) -> None:
    return _model_job_lifecycle().fail_preprocess_job(job, error)
def _apply_printable_image_pipeline(job: Job, raw_preview: Path) -> dict[str, int]:
    try:
        result = process_printable_image(
            raw_preview,
            job.directory,
            job.palette,
            job.print_settings,
            palette_roles=job.palette_roles,
            subject_reference_path=job.input_path if job.source == "image" else None,
        )
    except PrintableImageError as exc:
        raise OpenAIPreprocessorError(str(exc)) from None
    job.raw_preview_path = raw_preview
    job.strict_preview_path = result.strict_preview
    job.preview_path = result.clean_preview
    job.model_reference_path = result.model_reference
    job.heatmap_path = result.heatmap
    job.metadata_path = result.metadata
    job.background_mask_path = result.background_mask
    job.subject_mask_path = result.subject_mask
    job.mask_paths = result.masks
    job.image_metrics = result.metrics
    portrait_cleanup = result.metrics.get("portrait_skin_cleanup", {})
    if isinstance(portrait_cleanup, dict) and portrait_cleanup.get("activated") == 1:
        garment_color = str(portrait_cleanup.get("garment_color", "")).upper()
        skin_color = str(portrait_cleanup.get("skin_color", "")).upper()
        if garment_color in job.palette and skin_color in job.palette and garment_color != skin_color:
            # Persist the semantic recovery so a restarted native client does
            # not re-submit an inverted skin/garment mapping on the next image.
            job.palette_roles = assign_palette_roles(
                job.palette,
                {"primary": garment_color, "light": skin_color},
            ).color_by_role
    return result.palette_usage


def _write_job_color_intent(job: Job, artifact: Path) -> None:
    if not job.palette or artifact.suffix.lower() == ".glb":
        job.color_intent_path, job.color_intent_schema, job.color_intent_sha256 = None, "", ""
        return
    appearance = job.raw_preview_path or job.model_reference_path
    material = job.preview_path or job.strict_preview_path
    if appearance is None or material is None:
        job.color_intent_path, job.color_intent_schema, job.color_intent_sha256 = None, "", ""
        return
    if not job.palette_roles:
        job.palette_roles = assign_palette_roles(job.palette).color_by_role
    try:
        result = write_color_intent_manifest(
            job.directory / COLOR_INTENT_FILENAME, artifact, appearance, material,
            job.palette, job.palette_roles,
            geometry_reference_path=_geometry_generation_reference(job),
        )
    except ColorIntentError as exc:
        raise TripoError(f"The color-intent manifest could not be published: {exc}") from None
    job.color_intent_path, job.color_intent_schema, job.color_intent_sha256 = (
        result.path, result.schema, result.sha256
    )


def _assess_reference_advice(job: Job) -> tuple[dict[str, Any], dict[str, Any]]:
    return _model_quality_workflow().assess_reference_advice(job)


def _assess_job_generation_reference(job: Job) -> dict[str, Any]:
    return _model_quality_workflow().assess_job_generation_reference(job)


def _apply_preview_visual_quality_gate(job: Job, report: Mapping[str, Any]) -> list[str]:
    return _model_quality_workflow().apply_preview_visual_quality_gate(job, report)


def _assess_job_preview_visual_quality(job: Job, original: Path) -> dict[str, Any] | None:
    return _model_quality_workflow().assess_job_preview_visual_quality(job, original)


def _quality_portrait_multiview_enabled(job: Job) -> bool:
    return _portrait_multiview_workflow().quality_portrait_multiview_enabled(job)


def _portrait_multiview_prompt(background_instruction: str | None = None) -> str:
    # Keep this below conservative image-edit prompt limits. The supplied image
    # owns identity, materials, pose and base; the prompt owns only view layout.
    return _portrait_multiview_workflow().portrait_multiview_prompt(background_instruction)


def _portrait_geometry_material_prompt(palette_roles: Mapping[str, str]) -> str:
    return _portrait_multiview_workflow().portrait_geometry_material_prompt(palette_roles)


def _prepare_portrait_geometry_material_views(
    natural_turntable: Path,
    output_directory: Path,
    palette_roles: Mapping[str, str],
) -> tuple[dict[str, Path], dict[str, Any]]:
    return _portrait_multiview_workflow().prepare_portrait_geometry_material_views(natural_turntable, output_directory, palette_roles)


def _create_portrait_multiview_sheet(job: Job, sheet: Path) -> None:
    return _portrait_multiview_workflow().create_portrait_multiview_sheet(job, sheet)


def _multiview_paths_from_metrics(job: Job, key: str) -> dict[str, Path] | None:
    return _portrait_multiview_workflow().multiview_paths_from_metrics(job, key)


def _multiview_sheet_fingerprint(sheet: Path) -> str:
    return _portrait_multiview_workflow().multiview_sheet_fingerprint(sheet)


def _mark_multiview_candidate_rejected(job: Job, sheet: Path, reason: str) -> None:
    return _portrait_multiview_workflow().mark_multiview_candidate_rejected(job, sheet, reason)


def _can_reuse_multiview_candidate(job: Job, sheet: Path) -> bool:
    return _portrait_multiview_workflow().can_reuse_multiview_candidate(job, sheet)


def _ensure_portrait_multiview(job: Job) -> dict[str, Path] | None:
    return _portrait_multiview_workflow().ensure_portrait_multiview(job)


def _recommend_palette_job(job: Job) -> None:
    # A queued legacy recommendation resumes into creation without another
    # color-recommendation request or printer palette.
    if job.source == "text":
        _preprocess_text_job(job, job.user_prompt)
    elif job.input_path is not None:
        _preprocess_image_job(job, job.input_path, _normalize_image_instruction(job.user_prompt))
    else:
        _fail_job(job, "The stored reference image is unavailable.")


def _progress_callback(job: Job, start: int, end: int) -> Callable[[int | float | None], None]:
    def update(value: int | float | None) -> None:
        try:
            fraction = max(0.0, min(float(value), 100.0)) / 100.0
        except (TypeError, ValueError):
            return
        with _JOBS_LOCK:
            if not job.stop_event.is_set():
                job.progress = start + int((end - start) * fraction)
                _persist_job(job)

    return update


def _automatic_visual_review(job: Job, artifact: Path) -> dict[str, Any] | None:
    return _model_quality_workflow().automatic_visual_review(job, artifact)


def _prepare_obj_artifact(
    raw_download: Path,
    job_directory: Path,
    palette: tuple[str, ...],
    palette_roles: Mapping[str, str] | None = None,
    portrait_materials: bool = False,
    front_material_reference: Path | None = None,
    status_callback: Callable[[str, int], None] | None = None,
    build_aligned_portrait_reference: bool = False,
) -> Path:
    return _model_artifact_workflow().prepare_obj_artifact(raw_download, job_directory, palette, palette_roles, portrait_materials, front_material_reference, status_callback, build_aligned_portrait_reference)


def _persist_attempts(job: Job) -> None:
    temporary = job.directory / "attempts.json.part"
    destination = job.directory / "attempts.json"
    try:
        temporary.write_text(json.dumps({"attempts": job.attempts}, ensure_ascii=False, indent=2), encoding="utf-8")
        os.replace(temporary, destination)
    except OSError:
        try:
            temporary.unlink(missing_ok=True)
        except OSError:
            pass


def _promote_attempt_artifact(candidate: Path, artifact: Path) -> None:
    """Publish an accepted attempt together with its local quality evidence.

    Conversion and structural analysis run inside ``attempt-XX`` (or a recovery
    directory), while the public job contract reads reports next to the final
    artifact.  Moving only the OBJ made the completed UI say "not checked" even
    though the gate had just finished.  Copy the small reports atomically before
    the job becomes ready so restart recovery and the live result card agree.
    """

    try:
        if candidate.resolve() != artifact.resolve():
            shutil.copyfile(candidate, artifact)
        for filename in (MODEL_QUALITY_FILENAME, "vertex-color-metrics.json", "analysis-model.obj", "provider-model.glb"):
            source = candidate.parent / filename
            destination = artifact.parent / filename
            if not source.is_file() or source.resolve() == destination.resolve():
                continue
            temporary = destination.with_name(destination.name + ".part")
            try:
                shutil.copyfile(source, temporary)
                os.replace(temporary, destination)
            finally:
                temporary.unlink(missing_ok=True)
    except OSError:
        raise TripoError("The accepted model and its quality report could not be published.") from None


def _record_attempt(job: Job, attempt_number: int, **updates: Any) -> None:
    with _JOBS_LOCK:
        while len(job.attempts) < attempt_number:
            job.attempts.append({"attempt": len(job.attempts) + 1})
        job.attempts[attempt_number - 1].update(updates)
        _persist_attempts(job)
        _persist_job(job)


def _refresh_stale_face_limit_report(path: Path, palette: tuple[str, ...]) -> None:
    return _model_artifact_workflow().refresh_stale_face_limit_report(path, palette)


def _analysis_artifact(artifact: Path) -> Path:
    return _model_artifact_workflow().analysis_artifact(artifact)


def _download_generation_artifact(job: Job, generation_id: str, attempt_number: int = 1, resume: bool = False) -> Path:
    return _model_artifact_workflow().download_generation_artifact(job, generation_id, attempt_number, resume)


def _download_conversion(
    job: Job, generation_id: str, format_name: str, attempt_number: int = 1, resume: bool = False
) -> Path:
    return _model_artifact_workflow().download_conversion(job, generation_id, format_name, attempt_number, resume)


def _can_manually_retry_hunyuan(job: Job) -> bool:
    return _model_job_lifecycle().can_manually_retry_hunyuan(job)


def _latest_generation_task_id(job: Job) -> str:
    return _model_job_lifecycle().latest_generation_task_id(job)


def _can_retry_unsubmitted_model(job: Job) -> bool:
    return _model_job_lifecycle().can_retry_unsubmitted_model(job)


def _run_worker_with_diagnostics(job: Job, worker: Callable[..., None], args: tuple[Any, ...]) -> None:
    with diagnostic_context(job.id):
        started = time.monotonic()
        worker_name = getattr(worker, "__name__", type(worker).__name__)
        diagnostic_event("job.worker.started", worker=worker_name, source=job.source, phase=job.phase)
        try:
            worker(job, *args)
        except Exception as exc:
            diagnostic_event(
                "job.worker.unhandled_exception",
                level="ERROR",
                worker=worker_name,
                exception_chain=exception_details(exc),
            )
            raise
        finally:
            diagnostic_event(
                "job.worker.completed",
                worker=worker_name,
                state=job.state,
                phase=job.phase,
                elapsed_ms=round((time.monotonic() - started) * 1000),
            )


def _executor_for(worker: Callable[..., None]) -> ThreadPoolExecutor:
    # Provider geometry is intentionally serialized in its own lane so a long
    # paid task cannot starve palette recommendation or Image2 preprocessing.
    return _MODEL_EXECUTOR if worker in {_generate_job, _retexture_job} else _DESIGN_EXECUTOR


def _submit(job: Job, worker: Callable[..., None], *args: Any) -> None:
    if worker in {_preprocess_text_job, _preprocess_image_job, _recommend_palette_job}:
        with _JOBS_LOCK:
            job.design_started_monotonic = None
            _begin_design_timing(job)
    try:
        future = _executor_for(worker).submit(_run_worker_with_diagnostics, job, worker, args)
    except RuntimeError:
        raise RequestError("service_unavailable", "The model job service is shutting down.", 503, True) from None
    with _JOBS_LOCK:
        job.future = future


def shutdown_sidecar() -> None:
    global _SHUT_DOWN
    with _SHUTDOWN_LOCK:
        if _SHUT_DOWN:
            return
        _SHUT_DOWN = True
    with _JOBS_LOCK:
        jobs = list(_JOBS.values())
        for job in jobs:
            job.stop_event.set()
    _DESIGN_EXECUTOR.shutdown(wait=True, cancel_futures=False)
    _MODEL_EXECUTOR.shutdown(wait=True, cancel_futures=False)
    with _JOBS_LOCK:
        _JOBS.clear()


atexit.register(shutdown_sidecar)


def _design_workflow() -> DesignWorkflow:
    return DesignWorkflow(DesignWorkflowPorts(
        lock=_JOBS_LOCK,
        persist=lambda job, **kwargs: _persist_job(job, **kwargs),
        stop_boundary=lambda job: _stop_boundary(job),
        fail=lambda job, message: _fail_job(job, message),
        fail_preprocess=lambda job, error: _fail_preprocess_job(job, error),
        mark_stopped=lambda job: _mark_stopped(job),
        finish_deleted=lambda job: _finish_deleted(job),
        apply_printable_image=lambda *args, **kwargs: _apply_printable_image_pipeline(*args, **kwargs),
        assess_reference=lambda job: _assess_reference_advice(job),
        assess_preview=lambda *args, **kwargs: _assess_job_preview_visual_quality(*args, **kwargs),
        generate_image=lambda *args, **kwargs: generate_geometry_reference_image(*args, **kwargs),
        edit_image=lambda *args, **kwargs: preprocess_image(*args, **kwargs),
        vision_once=lambda *args, **kwargs: complete_vision_once(*args, **kwargs),
        image_provider_status=lambda: image_provider_status(),
        fallback_enabled=lambda: _preprocess_fallback_enabled(),
        validate_image=lambda *args, **kwargs: _validate_image_file(*args, **kwargs),
        review_nonportrait=lambda *args, **kwargs: review_nonportrait_reference(*args, **kwargs),
    ))


def _model_workflow() -> ModelGenerationWorkflow:
    return ModelGenerationWorkflow(ModelGenerationWorkflowPorts(
        lock=_JOBS_LOCK,
        persist=lambda job, **kwargs: _persist_job(job, **kwargs),
        stop_boundary=lambda job: _stop_boundary(job),
        fail=lambda job, message: _fail_job(job, message),
        mark_stopped=lambda job: _mark_stopped(job),
        finish_deleted=lambda job: _finish_deleted(job),
        gateway=lambda provider: _model_gateway(provider),
        download_artifact=lambda *args, **kwargs: _download_generation_artifact(*args, **kwargs),
        promote_artifact=lambda *args, **kwargs: _promote_attempt_artifact(*args, **kwargs),
        analysis_artifact=lambda *args, **kwargs: _analysis_artifact(*args, **kwargs),
        automatic_visual_review=lambda *args, **kwargs: _automatic_visual_review(*args, **kwargs),
        ensure_multiview=lambda job: _ensure_portrait_multiview(job),
        multiview_paths=lambda job, key: _multiview_paths_from_metrics(job, key),
        write_color_intent=lambda job, artifact: _write_job_color_intent(job, artifact),
        progress_callback=lambda *args, **kwargs: _progress_callback(*args, **kwargs),
        record_attempt=lambda *args, **kwargs: _record_attempt(*args, **kwargs),
        return_to_multiview_retry=lambda *args, **kwargs: _return_to_portrait_multiview_retry(*args, **kwargs),
        multiview_enabled=lambda job: _quality_portrait_multiview_enabled(job),
        is_shutting_down=lambda: _SHUT_DOWN,
        validate_image=lambda *args, **kwargs: _validate_image_file(*args, **kwargs),
        validate_topology=lambda *args, **kwargs: _validate_obj_topology(*args, **kwargs),
    ))


def _preprocess_text_job(job: Job, prompt: str) -> None:
    _design_workflow().preprocess_text(job, prompt)


def _preprocess_image_job(job: Job, input_path: Path, instruction: str) -> None:
    _design_workflow().preprocess_image(job, input_path, instruction)


def _begin_design_timing(job: Job) -> None:
    _design_workflow().begin_timing(job)


def _generate_job(
    job: Job,
    prepared_prompt: str,
    resume: bool = False,
    authorization: PaidTaskAuthorization | None = None,
) -> None:
    _model_workflow().generate(job, prepared_prompt, resume, authorization)


def _retexture_job(
    job: Job,
    source_job_id: str,
    source_task_id: str,
    resume: bool = False,
    authorization: PaidTaskAuthorization | None = None,
) -> None:
    _model_workflow().retexture(job, source_job_id, source_task_id, resume, authorization)


def _job_application(get_job=None) -> ModelJobApplication:
    return ModelJobApplication(ModelJobApplicationPorts(
        jobs=_JOBS,
        lock=_JOBS_LOCK,
        get_job=get_job or (lambda job_id: _JOBS.get(job_id)),
        new_job=lambda *args, **kwargs: _new_job(*args, **kwargs),
        persist=lambda *args, **kwargs: _persist_job(*args, **kwargs),
        cleanup=lambda *args, **kwargs: _cleanup_job(*args, **kwargs),
        submit=lambda *args, **kwargs: _submit(*args, **kwargs),
        preprocess_text=_preprocess_text_job,
        preprocess_image=_preprocess_image_job,
        present=lambda *args, **kwargs: _public_job(*args, **kwargs),
        fallback_enabled=lambda *args, **kwargs: _preprocess_fallback_enabled(*args, **kwargs),
        image_provider_status=lambda *args, **kwargs: image_provider_status(*args, **kwargs),
        can_retry_hunyuan=lambda *args, **kwargs: _can_manually_retry_hunyuan(*args, **kwargs),
        can_retry_unsubmitted=lambda *args, **kwargs: _can_retry_unsubmitted_model(*args, **kwargs),
        clear_artifact=lambda *args, **kwargs: _clear_job_artifact(*args, **kwargs),
        latest_task_id=lambda *args, **kwargs: _latest_generation_task_id(*args, **kwargs),
        assess_reference=lambda *args, **kwargs: _assess_reference_advice(*args, **kwargs),
        gateway=lambda *args, **kwargs: _model_gateway(*args, **kwargs),
        generate=_generate_job,
        retexture=_retexture_job,
        adopt_legacy=lambda *args, **kwargs: _adopt_legacy_completed_job(*args, **kwargs),
        reuse_design=lambda *args, **kwargs: _reuse_design_job(*args, **kwargs),
        record_attempt=lambda *args, **kwargs: _record_attempt(*args, **kwargs),
        remove_state=lambda *args, **kwargs: _remove_job_state(*args, **kwargs),
        analysis_artifact=lambda *args, **kwargs: _analysis_artifact(*args, **kwargs),
        validate_image=lambda *args, **kwargs: _validate_image_file(*args, **kwargs),
        analyze_model=lambda *args, **kwargs: analyze_printable_obj(*args, **kwargs),
        write_quality=lambda *args, **kwargs: write_model_quality_report(*args, **kwargs),
        review_model=lambda *args, **kwargs: review_model_visual_quality(*args, **kwargs),
    ))


def _portrait_multiview_workflow() -> PortraitMultiviewWorkflow:
    return PortraitMultiviewWorkflow(PortraitMultiviewWorkflowPorts(
        persist=lambda *args, **kwargs: _persist_job(*args, **kwargs),
        complete_vision=lambda *args, **kwargs: complete_vision(*args, **kwargs),
        edit_image=lambda *args, **kwargs: edit_image(*args, **kwargs),
        lock=_JOBS_LOCK,
        stop_boundary=lambda *args, **kwargs: _stop_boundary(*args, **kwargs),
    ))


def _model_quality_workflow() -> ModelQualityWorkflow:
    return ModelQualityWorkflow(ModelQualityWorkflowPorts(
        persist=lambda *args, **kwargs: _persist_job(*args, **kwargs),
        assess_generation_reference=lambda *args, **kwargs: _assess_job_generation_reference(*args, **kwargs),
        analysis_artifact=lambda *args, **kwargs: _analysis_artifact(*args, **kwargs),
        analyze_printable_obj=lambda *args, **kwargs: analyze_printable_obj(*args, **kwargs),
        assess_job_model_reference=lambda *args, **kwargs: _assess_job_model_reference(*args, **kwargs),
        lock=_JOBS_LOCK,
        model_output_root=lambda *args, **kwargs: _model_output_root(*args, **kwargs),
        read_job_report=lambda *args, **kwargs: _read_job_report(*args, **kwargs),
        review_prepared_reference=lambda *args, **kwargs: review_prepared_reference(*args, **kwargs),
        write_model_quality_report=lambda *args, **kwargs: write_model_quality_report(*args, **kwargs),
    ))


def _model_artifact_workflow() -> ModelArtifactWorkflow:
    return ModelArtifactWorkflow(ModelArtifactWorkflowPorts(
        persist=lambda *args, **kwargs: _persist_job(*args, **kwargs),
        validate_artifact=lambda *args, **kwargs: _validate_artifact(*args, **kwargs),
        prepare_obj=lambda *args, **kwargs: _prepare_obj_artifact(*args, **kwargs),
        analyze_printable_obj=lambda *args, **kwargs: analyze_printable_obj(*args, **kwargs),
        lock=_JOBS_LOCK,
        model_gateway=lambda *args, **kwargs: _model_gateway(*args, **kwargs),
        multiview_paths_from_metrics=lambda *args, **kwargs: _multiview_paths_from_metrics(*args, **kwargs),
        prepare_generated_glb=lambda *args, **kwargs: prepare_generated_glb(*args, **kwargs),
        prepare_portrait_geometry_material_views=lambda *args, **kwargs: _prepare_portrait_geometry_material_views(*args, **kwargs),
        progress_callback=lambda *args, **kwargs: _progress_callback(*args, **kwargs),
        record_attempt=lambda *args, **kwargs: _record_attempt(*args, **kwargs),
        stop_boundary=lambda *args, **kwargs: _stop_boundary(*args, **kwargs),
        tripo_gateway=lambda: _MODEL_PROVIDER_GATEWAY,
        write_analysis_obj=lambda *args, **kwargs: write_analysis_obj(*args, **kwargs),
        write_model_quality_report=lambda *args, **kwargs: write_model_quality_report(*args, **kwargs),
    ))


def _model_job_lifecycle() -> ModelJobLifecycle:
    return ModelJobLifecycle(ModelJobLifecyclePorts(
        apply_preview_visual_quality_gate=lambda *args, **kwargs: _apply_preview_visual_quality_gate(*args, **kwargs),
        assess_reference_advice=lambda *args, **kwargs: _assess_reference_advice(*args, **kwargs),
        generate_job=_generate_job,
        is_shutting_down=lambda: _SHUT_DOWN,
        jobs=_JOBS,
        lock=_JOBS_LOCK,
        model_output_root=lambda *args, **kwargs: _model_output_root(*args, **kwargs),
        persist_job=lambda *args, **kwargs: _persist_job(*args, **kwargs),
        retexture_job=_retexture_job,
        submit=lambda *args, **kwargs: _submit(*args, **kwargs),
        validate_image_file=lambda *args, **kwargs: _validate_image_file(*args, **kwargs),
    ))


class Handler(BaseHTTPRequestHandler):
    server_version = "OrcaAISidecar/1.0"

    def log_message(self, fmt: str, *args: Any) -> None:
        diagnostic_event("http.access", detail=fmt % args, client_host=self.client_address[0])

    def send_json(self, status: int, payload: dict[str, Any]) -> None:
        encoded = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(encoded)))
        self.end_headers()
        try:
            self.wfile.write(encoded)
        except (BrokenPipeError, ConnectionAbortedError, ConnectionResetError):
            # The native client may cancel an obsolete poll while a newer one is
            # already in flight.  A closed response socket is not a job failure
            # and must not surface as an alarming server-side traceback.
            pass

    def send_bytes(
        self,
        stream: BinaryIO,
        size: int,
        content_type: str,
        filename: str | None = None,
    ) -> None:
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(size))
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Cache-Control", "no-store")
        if filename is not None:
            self.send_header("Content-Disposition", f'attachment; filename="{filename}"')
        self.end_headers()
        try:
            while chunk := stream.read(64 * 1024):
                self.wfile.write(chunk)
        except (BrokenPipeError, ConnectionAbortedError, ConnectionResetError):
            pass

    def read_json(self) -> dict[str, Any]:
        length = int(self.headers.get("Content-Length", "0"))
        if length <= 0:
            return {}
        if length > MAX_REQUEST_BYTES:
            raise ValueError("request body too large")
        parsed = json.loads(self.rfile.read(length).decode("utf-8"))
        if not isinstance(parsed, dict):
            raise ValueError("request body must be a JSON object")
        return parsed

    def _read_body(self, limit: int) -> bytes:
        raw_length = self.headers.get("Content-Length")
        if raw_length is None:
            raise RequestError("invalid_request", "Content-Length is required.", 400)
        try:
            length = int(raw_length)
        except ValueError:
            raise RequestError("invalid_request", "Content-Length is invalid.", 400) from None
        if length < 0:
            raise RequestError("invalid_request", "Content-Length is invalid.", 400)
        if length > limit:
            raise RequestError("request_too_large", "Request body is too large.", 413)
        body = self.rfile.read(length)
        if len(body) != length:
            raise RequestError("invalid_request", "Request body is incomplete.", 400)
        return body

    def _read_model_json(self) -> dict[str, Any]:
        content_type = self.headers.get("Content-Type", "").split(";", 1)[0].strip().lower()
        if content_type != "application/json":
            raise RequestError("unsupported_media_type", "Content-Type must be application/json.", 415)
        body = self._read_body(MAX_REQUEST_BYTES)
        try:
            parsed = json.loads(body.decode("utf-8")) if body else {}
        except (UnicodeDecodeError, json.JSONDecodeError):
            raise RequestError("invalid_json", "Request body contains malformed JSON.", 400) from None
        if not isinstance(parsed, dict):
            raise RequestError("invalid_request", "Request body must be a JSON object.", 400)
        return parsed

    def _read_image_multipart(self) -> tuple[dict[str, str], bytes, str]:
        content_type = self.headers.get("Content-Type", "")
        if len(content_type) > 1024 or not content_type.lower().startswith("multipart/form-data;"):
            raise RequestError("unsupported_media_type", "Content-Type must be multipart/form-data.", 415)
        body = self._read_body(MAX_MULTIPART_BYTES)
        try:
            header = b"Content-Type: " + content_type.encode("latin-1") + b"\r\nMIME-Version: 1.0\r\n\r\n"
        except UnicodeEncodeError:
            raise RequestError("invalid_multipart", "Multipart Content-Type is invalid.", 400) from None
        message = BytesParser(policy=policy.default).parsebytes(header + body)
        if not message.is_multipart():
            raise RequestError("invalid_multipart", "Multipart request is malformed.", 400)

        fields: dict[str, str] = {}
        image: bytes | None = None
        image_content_type = ""
        seen: set[str] = set()
        for part in message.iter_parts():
            if part.is_multipart() or part.get_content_disposition() != "form-data":
                raise RequestError("invalid_multipart", "Nested or invalid multipart data is not supported.", 400)
            name = part.get_param("name", header="content-disposition")
            if name not in {
                "request_id", "instruction", "palette", "palette_roles", "palette_recommendation_confirmed", "provider",
                "face_limit", "geometry_quality", "texture_quality", "output_format",
                "palette_color_count", "style", "custom_style", "print", "image", "generate_image",
            } or name in seen:
                raise RequestError("invalid_multipart", "Multipart fields are unexpected or duplicated.", 400)
            seen.add(name)
            payload = part.get_payload(decode=True) or b""
            if name == "image":
                image = payload
                image_content_type = part.get_content_type().lower()
                # Keep only an advisory basename, never a client-supplied path.
                filename = part.get_filename() or ""
                fields["_source_filename"] = re.split(r"[\\/]", filename)[-1][:256]
            else:
                if len(payload) > MAX_PROMPT_BYTES:
                    raise RequestError("invalid_request", f"{name} exceeds the 2000-byte limit.", 400)
                try:
                    fields[name] = payload.decode(part.get_content_charset() or "utf-8")
                except (LookupError, UnicodeDecodeError):
                    raise RequestError("invalid_request", f"{name} must be UTF-8 text.", 400) from None
        if image is None:
            raise RequestError("invalid_request", "image is required.", 400)
        return fields, image, image_content_type

    def _require_session(self) -> bool:
        expected = _configured_session_token()
        if expected is None:
            self._model_error(503, "session_configuration_invalid", "AI Sidecar session protection is invalid.")
            return False
        if not expected and _session_required():
            self._model_error(503, "session_configuration_missing", "AI Sidecar session protection is required.")
            return False
        if expected:
            expected_proof = _session_hmac(expected, f"client:{SIDECAR_SESSION_NONCE}")
            provided = self.headers.get("X-OrcaSlicer-Session-Proof", "")
            if len(provided) != len(expected_proof) or not hmac.compare_digest(provided, expected_proof):
                self._model_error(401, "session_required", "A valid OrcaSlicer AI session is required.")
                return False
        return True

    def _require_native_client(self) -> bool:
        if self.headers.get("X-OrcaSlicer-Client") != "native":
            self._model_error(401, "client_required", "X-OrcaSlicer-Client must be native.")
            return False
        return self._require_session()

    def _model_error(
        self,
        status: int,
        code: str,
        message: str,
        retryable: bool = False,
    ) -> None:
        self.send_json(status, {"error": {"code": code, "message": message, "retryable": retryable}})

    @staticmethod
    def _job_route(path: str) -> tuple[str | None, str | None]:
        prefix = "/v1/orcaslicer/model-jobs/"
        if not path.startswith(prefix):
            return None, None
        parts = path[len(prefix) :].split("/")
        if len(parts) == 1 and parts[0]:
            action = "status"
        elif len(parts) == 2 and parts[0] and (
            parts[1] in {
                "input", "raw-preview", "strict-preview", "preview", "model-reference", "heatmap", "metadata",
                "background-mask", "subject-mask", "generate", "retexture", "stop", "artifact", "color-intent",
                "recheck", "visual-review", "model-view-sheet", "confirm-palette", "generation-options", "reuse-design",
            }
            or re.fullmatch(r"mask-[a-z0-9_]+", parts[1])
        ):
            action = parts[1]
        else:
            return None, None
        try:
            parsed = uuid.UUID(parts[0])
        except ValueError:
            return None, None
        if str(parsed) != parts[0].lower():
            return None, None
        return parts[0].lower(), action

    def _get_job(self, job_id: str) -> Job | None:
        with _JOBS_LOCK:
            return _JOBS.get(job_id)

    def do_GET(self) -> None:
        if self.path == "/v1/orcaslicer/capabilities":
            if not self._require_native_client():
                return
            image_available = bool(image_provider_status()["available"])
            policy = provider_policy()
            self.send_json(200, build_catalog({
                "image": image_available,
                "text": image_available or _preprocess_fallback_enabled(),
                "geometry": any(_model_gateway(name).model_generation_available() for name in policy.geometry_providers),
                "texture": _MODEL_PROVIDER_GATEWAY.model_generation_available(),
            }))
            return
        if self.path == "/v1/orcaslicer/session-challenge":
            token = _configured_session_token()
            if token is None:
                self._model_error(503, "session_configuration_invalid", "AI Sidecar session protection is invalid.")
                return
            client_nonce = self.headers.get("X-OrcaSlicer-Client-Nonce", "")
            if not token or not re.fullmatch(r"[0-9A-Fa-f]{64}", client_nonce):
                self._model_error(401, "session_challenge_required", "A valid session challenge is required.")
                return
            self.send_json(
                200,
                {
                    "ok": True,
                    "protocol_version": 2,
                    "sidecar_version": SIDECAR_VERSION,
                    "session_protected": True,
                    "server_nonce": SIDECAR_SESSION_NONCE,
                    "server_proof": _session_hmac(
                        token, f"server:{client_nonce}:{SIDECAR_SESSION_NONCE}"
                    ),
                },
            )
            return
        if self.path == "/health":
            if not self._require_session():
                return
            config = os.environ.get("OPENAI_API_KEY", "")
            image_provider = image_provider_status()
            text_preprocessing = image_provider["available"] or _preprocess_fallback_enabled()
            generation_preprocessing = text_preprocessing
            policy = provider_policy()
            self.send_json(
                200,
                {
                    "ok": True,
                    "protocol_version": 2,
                    "sidecar_version": SIDECAR_VERSION,
                    "runtime": {
                        "health_schema_version": 2,
                        "instance_id": SIDECAR_INSTANCE_ID,
                        "session_protected": bool(_configured_session_token()),
                        "build": _safe_runtime_identity(),
                        "openai_base_url": safe_endpoint(
                            os.environ.get("OPENAI_BASE_URL", "https://api.openai.com/v1")
                        ).rstrip("/"),
                        "image_provider_base_url": image_provider["base_url"],
                        "image_provider_source": image_provider["source"],
                        "tripo_base_url": safe_endpoint(
                            os.environ.get("TRIPO_API_BASE", "https://openapi.tripo3d.com/v3")
                        ).rstrip("/"),
                        "configuration_mode": "internal_locked"
                        if os.environ.get("ORCASLICER_AI_CONFIG_MODE") == "internal_locked"
                        else "external",
                        "network": _runtime_network_metadata(),
                    },
                    "capabilities": {
                        "config_proposal": {"available": bool(config)},
                        "model_generation": {
                            "available": generation_preprocessing and any(
                                _model_gateway(name).model_generation_available() for name in policy.geometry_providers),
                            "providers": {name: {"available": _model_gateway(name).model_generation_available()}
                                          for name in policy.geometry_providers},
                            "sources": ["text", "image"],
                            "styles": list(STYLE_IDS),
                            "artifact_formats": ["glb", "obj"],
                            "face_limits": [300000, 1000000, 2000000],
                            "geometry_qualities": ["standard", "detailed"],
                            "texture_qualities": ["standard", "detailed", "extreme"],
                            "output_formats": ["glb", "obj"],
                            "default_face_limit": GENERATION_PROFILE_FACE_LIMITS[DEFAULT_GENERATION_PROFILE],
                            "generation_profiles": list(GENERATION_PROFILES),
                            "default_generation_profile": DEFAULT_GENERATION_PROFILE,
                            "provider_policy": {
                                "design_providers": list(policy.design_providers),
                                "geometry_provider": policy.geometry_provider,
                                "geometry_providers": list(policy.geometry_providers),
                                "automatic_fallback": policy.automatic_fallback,
                                "max_paid_model_tasks_per_confirmation":
                                    policy.max_paid_model_tasks_per_confirmation,
                            },
                            "source_availability": {
                                "text": text_preprocessing,
                                "image": image_provider["available"],
                            },
                            "image_provider": image_provider,
                            "palette_recommendation": {
                                "available": False,
                                "reason": "creation_preserves_natural_colors",
                            },
                            "style_recommendation": {
                                "available": True,
                                "local_only": True,
                            },
                            "printable_image_pipeline": {
                                "available": False,
                                "print_modes": ["solid_regions"],
                                "color_distances": ["ciede2000", "delta_e76"],
                                "outputs": [
                                    "raw_preview", "strict_preview", "clean_preview", "model_reference",
                                    "heatmap", "masks", "metadata",
                                ],
                            },
                        },
                    },
                },
            )
            return

        if not self.path.startswith("/v1/orcaslicer/model-jobs"):
            self.send_json(404, {"error": "not found"})
            return
        if not self._require_native_client():
            return
        if self.path == "/v1/orcaslicer/model-jobs/latest":
            with _JOBS_LOCK:
                candidates = [job for job in _JOBS.values() if _latest_job_is_restorable(job)]
                response = _public_job(max(candidates, key=lambda item: item.updated_at)) if candidates else None
            self.send_json(200, {"job": response})
            return
        job_id, action = self._job_route(self.path)
        downloadable = {
            "status", "input", "raw-preview", "strict-preview", "preview", "model-reference", "heatmap", "metadata",
            "background-mask", "subject-mask", "artifact", "color-intent", "model-view-sheet",
        }
        if not job_id or (action not in downloadable and not (action or "").startswith("mask-")):
            self._model_error(404, "not_found", "Model job route not found.")
            return
        job = self._get_job(job_id)
        if job is None and action == "status":
            # Model-library entries created before durable job manifests still
            # retain their bounded provider attempt log.  Adopt them lazily so
            # the native history UI can backfill the real 3D provider task ID
            # without scanning or reviving every legacy model at startup.
            job = _adopt_legacy_completed_job(job_id)
        if job is None:
            self._model_error(404, "job_not_found", "Model job not found.")
            return
        if action == "status":
            with _JOBS_LOCK:
                response = _public_job(job)
            self.send_json(200, {"job": response})
            return
        self._download_job_file(job, action)

    def do_POST(self) -> None:
        if self.path == "/v1/orcaslicer/shutdown":
            if not self._require_native_client():
                return
            self.send_json(202, {"ok": True, "state": "stopping"})
            threading.Thread(
                target=self.server.shutdown,
                name="orca-sidecar-shutdown",
                daemon=True,
            ).start()
            return

        if self.path == "/v1/orcaslicer/config-proposal":
            if not self._require_native_client():
                return
            try:
                request = self.read_json()
                if not str(request.get("user_message", "")).strip():
                    self.send_json(400, {"error": "user_message is required"})
                    return
                if not extract_allowed_keys(request):
                    self.send_json(400, {"error": "allowed_changes is required"})
                    return
                self.send_json(200, normalize_proposal(provider_request(request), request))
            except ValueError as exc:
                self.send_json(400, {"error": str(exc)})
            except Exception as exc:
                self.send_json(502, {"error": str(exc)})
            return

        if self.path == "/v1/orcaslicer/journey-events":
            if not self._require_native_client():
                return
            try:
                record = _record_journey_event(self._read_model_json())
                self.send_json(201, {"event": record})
            except RequestError as exc:
                self._model_error(exc.status, exc.code, exc.message, exc.retryable)
            return

        if self.path == "/v1/orcaslicer/model-check":
            if not self._require_native_client():
                return
            try:
                self.send_json(200, {"job": _check_saved_model(self._read_model_json())})
            except RequestError as exc:
                self._model_error(exc.status, exc.code, exc.message, exc.retryable)
            return

        if self.path == "/v1/orcaslicer/model-style-recommendation":
            if not self._require_native_client():
                return
            try:
                self._recommend_model_style()
            except RequestError as exc:
                self._model_error(exc.status, exc.code, exc.message, exc.retryable)
            return

        if not self.path.startswith("/v1/orcaslicer/model-jobs"):
            self.send_json(404, {"error": "not found"})
            return
        if not self._require_native_client():
            return
        try:
            if self.path == "/v1/orcaslicer/model-jobs/text":
                self._create_text_job()
                return
            if self.path == "/v1/orcaslicer/model-jobs/image":
                self._create_image_job()
                return
            if self.path == "/v1/orcaslicer/model-jobs/recommend-text-palette":
                self._create_text_palette_recommendation()
                return
            if self.path == "/v1/orcaslicer/model-jobs/recommend-image-palette":
                self._create_image_palette_recommendation()
                return
            job_id, action = self._job_route(self.path)
            if not job_id or action not in {
                "generate", "retexture", "stop", "recheck", "visual-review", "confirm-palette", "generation-options", "reuse-design"
            }:
                self._model_error(404, "not_found", "Model job route not found.")
                return
            self._send_application_result(
                self._application().execute_job(action, job_id, self._read_model_json())
            )
        except RequestError as exc:
            self._model_error(exc.status, exc.code, exc.message, exc.retryable)

    def do_DELETE(self) -> None:
        if not self.path.startswith("/v1/orcaslicer/model-jobs"):
            self.send_json(404, {"error": "not found"})
            return
        if not self._require_native_client():
            return
        job_id, action = self._job_route(self.path)
        if not job_id or action != "status":
            self._model_error(404, "not_found", "Model job route not found.")
            return
        try:
            self._send_application_result(self._application().delete(job_id))
        except RequestError as exc:
            self._model_error(exc.status, exc.code, exc.message, exc.retryable)

    def _application(self) -> ModelJobApplication:
        return _job_application(self._get_job)

    def _send_application_result(self, result: ApplicationResult) -> None:
        if result.status == 204:
            self.send_response(204)
            self.end_headers()
        else:
            self.send_json(result.status, result.payload)

    def _create_text_job(self) -> None:
        self._application().require_design_source("text")
        self._send_application_result(self._application().create_text_job(self._read_model_json()))

    def _create_text_palette_recommendation(self) -> None:
        # Compatibility route: old clients now create an unrestricted design.
        self._create_text_job()

    def _recommend_model_style(self) -> None:
        self._send_application_result(self._application().recommend_model_style(*self._read_image_multipart()))

    def _create_image_job(self) -> None:
        self._application().require_design_source("image")
        self._send_application_result(self._application().create_image_job(*self._read_image_multipart()))

    def _create_image_palette_recommendation(self) -> None:
        # Compatibility route: old clients now create an unrestricted design.
        self._create_image_job()

    def _confirm_palette(self, job_id: str) -> None:
        self._send_application_result(self._application().confirm_palette(job_id, self._read_model_json()))

    def _set_generation_options(self, job_id: str) -> None:
        self._send_application_result(self._application().set_generation_options(job_id, self._read_model_json()))

    def _reuse_design(self, job_id: str) -> None:
        self._send_application_result(self._application().reuse_design(job_id, self._read_model_json()))

    def _generate(self, job_id: str) -> None:
        self._send_application_result(self._application().generate(job_id, self._read_model_json()))

    def _retexture(self, reference_job_id: str) -> None:
        self._send_application_result(self._application().retexture(reference_job_id, self._read_model_json()))

    def _stop(self, job_id: str) -> None:
        self._send_application_result(self._application().stop(job_id, self._read_model_json()))

    def _recheck(self, job_id: str) -> None:
        self._send_application_result(self._application().recheck(job_id, self._read_model_json()))

    def _visual_review(self, job_id: str) -> None:
        self._send_application_result(self._application().visual_review(job_id, self._read_model_json()))

    def _download_job_file(self, job: Job, kind: str) -> None:
        with _JOBS_LOCK:
            fixed_paths = {
                "input": job.input_path,
                "raw-preview": job.raw_preview_path,
                "strict-preview": job.strict_preview_path,
                "preview": job.preview_path,
                # The UI labels this as the actual 3D-service input. Keep that
                # promise by serving the sculptural geometry reference when the
                # quality portrait strategy selects it.
                "model-reference": _geometry_generation_reference(job),
                "heatmap": job.heatmap_path,
                "metadata": job.metadata_path,
                "background-mask": job.background_mask_path,
                "subject-mask": job.subject_mask_path,
                "artifact": job.artifact_path,
                "color-intent": job.color_intent_path,
                "model-view-sheet": job.directory / "model-view-sheet.png",
            }
            path = job.mask_paths.get(kind[5:]) if kind.startswith("mask-") else fixed_paths.get(kind)
            ready, size = _file_info(path)
            if not ready or path is None:
                self._model_error(409, f"{kind}_not_ready", f"Model job {kind} is not ready.", True)
                return
            if kind == "color-intent" and size > MAX_COLOR_INTENT_BYTES:
                self._model_error(409, "color_intent_invalid", "The color-intent manifest exceeds its size limit.")
                return
            image_kinds = {
                "input", "raw-preview", "strict-preview", "preview", "model-reference", "heatmap",
                "background-mask", "subject-mask", "model-view-sheet",
            }
            content_type = _stored_image_type(path) if kind in image_kinds or kind.startswith("mask-") else \
                "application/json; charset=utf-8" if kind in {"metadata", "color-intent"} else {
                "obj": "model/obj",
                "glb": "model/gltf-binary",
                "3mf": "application/vnd.ms-package.3dmanufacturing-3dmodel+xml",
                "stl": "model/stl",
            }.get(job.artifact_format, "application/octet-stream")
            filename = f"orcaslicer-model-{job.id}.{job.artifact_format}" if kind == "artifact" else \
                f"orcaslicer-color-intent-{job.id}.json" if kind == "color-intent" else None
            try:
                stream = path.open("rb")
            except OSError:
                self._model_error(503, "file_unavailable", "Model job file is unavailable.", True)
                return
        with stream:
            self.send_bytes(stream, size, content_type, filename)


class LoopbackServer(ThreadingHTTPServer):
    daemon_threads = False


def main() -> int:
    host = HOST.strip().lower()
    if host not in _LOOPBACK_HOSTS:
        print("ORCASLICER_AI_SIDECAR_HOST must be 127.0.0.1, localhost, or ::1.", file=sys.stderr)
        return 2
    if _configured_session_token() is None:
        print("ORCASLICER_AI_SESSION_TOKEN must be a 64-character hexadecimal capability.", file=sys.stderr)
        return 2
    if _session_required() and not _configured_session_token():
        print("This AI Sidecar runtime requires an authenticated OrcaSlicer session.", file=sys.stderr)
        return 2
    parent_pid = _configured_parent_pid()
    if os.environ.get("ORCASLICER_AI_PARENT_PID", "").strip() and parent_pid is None:
        print("ORCASLICER_AI_PARENT_PID must identify the owning OrcaSlicer process.", file=sys.stderr)
        return 2
    if _session_required() and parent_pid is None:
        print("This AI Sidecar runtime requires an owning OrcaSlicer process.", file=sys.stderr)
        return 2
    parent_handle: int | None = None
    if parent_pid is not None:
        if os.name == "nt":
            parent_handle = _open_parent_process_handle(parent_pid)
            parent_alive = parent_handle is not None
        else:
            parent_alive = _parent_process_alive(parent_pid)
        if not parent_alive:
            print("The owning OrcaSlicer process is no longer running.", file=sys.stderr)
            return 2
    if host == "::1":
        LoopbackServer.address_family = socket.AF_INET6
    image_provider = image_provider_status()
    diagnostic_event(
        "sidecar.starting",
        sidecar_version=SIDECAR_VERSION,
        instance_id=SIDECAR_INSTANCE_ID,
        session_protected=bool(_configured_session_token()),
        build=_safe_runtime_identity(),
        endpoint=safe_endpoint(f"http://{HOST}:{PORT}"),
        python_version=sys.version.split()[0],
        openssl_version=ssl.OPENSSL_VERSION,
        output_directory=str(_model_output_root()),
        openai_configured=bool(os.environ.get("OPENAI_API_KEY", "")),
        image_provider_configured=image_provider["available"],
        image_provider_source=image_provider["source"],
        tripo_configured=bool(os.environ.get("TRIPO_API_KEY", "")),
        configuration_mode="internal_locked"
        if os.environ.get("ORCASLICER_AI_CONFIG_MODE") == "internal_locked"
        else "external",
        openai_endpoint=safe_endpoint(os.environ.get("OPENAI_BASE_URL", "https://api.openai.com/v1")),
        image_provider_endpoint=image_provider["base_url"],
        tripo_endpoint=safe_endpoint(os.environ.get("TRIPO_API_BASE", "https://openapi.tripo3d.com/v3")),
        network=_runtime_network_metadata(),
    )
    # Restore local state first, but do not touch an existing remote task until
    # the owning Orca process has been rechecked and the listener is ready.
    try:
        restored_jobs = _restore_jobs(resume_jobs=False)
    except Exception:
        _close_parent_process_handle(parent_handle)
        raise
    try:
        server = LoopbackServer((HOST, PORT), Handler)
    except OSError as exc:
        _close_parent_process_handle(parent_handle)
        diagnostic_event(
            "sidecar.bind.failed",
            level="ERROR",
            endpoint=safe_endpoint(f"http://{HOST}:{PORT}"),
            exception_chain=exception_details(exc),
        )
        raise
    if parent_pid is not None:
        if os.name == "nt":
            parent_alive = parent_handle is not None and _parent_process_handle_alive(parent_handle)
        else:
            parent_alive = _parent_process_alive(parent_pid)
        if not parent_alive:
            _close_parent_process_handle(parent_handle)
            server.server_close()
            diagnostic_event("sidecar.parent.unavailable", level="ERROR", parent_pid=parent_pid)
            return 2
        threading.Thread(
            target=_monitor_parent,
            args=(server, parent_pid, parent_handle),
            name="orca-parent-monitor",
            daemon=True,
        ).start()
        parent_handle = None  # The monitor thread owns and closes this handle.
    _resume_restored_jobs(restored_jobs)
    diagnostic_event("sidecar.listening", endpoint=safe_endpoint(f"http://{HOST}:{PORT}"))
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        _close_parent_process_handle(parent_handle)
        server.server_close()
        shutdown_sidecar()
        diagnostic_event("sidecar.stopped")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
