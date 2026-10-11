"""Shared job DTOs, errors and bounded model policies."""
from __future__ import annotations

import threading
import time
from concurrent.futures import Future
from dataclasses import asdict, dataclass, field
from pathlib import Path
from printable_image_pipeline import PrintSettings
from printable_palette import (
    LEGACY_DEFAULT_PRINTABLE_COLORS,
    MAX_PRINTABLE_COLORS,
    MIN_PRINTABLE_COLORS,
)
from tripo_client import TripoError
from typing import Any

MAX_REQUEST_BYTES = 256 * 1024

MAX_CHANGES = 8

MAX_PROMPT_BYTES = 2000

MAX_CUSTOM_STYLE_BYTES = 1000

MAX_IMAGE_BYTES = 20 * 1024 * 1024

MAX_MULTIPART_BYTES = MAX_IMAGE_BYTES + 256 * 1024

MIN_SOURCE_IMAGE_EDGE = 64

MIN_MODEL_REFERENCE_EDGE = 256

MAX_ARTIFACT_BYTES = 768 * 1024 * 1024

MAX_ARCHIVE_FILES = 128

MAX_UNPACKED_BYTES = 1024 * 1024 * 1024

MAX_TEXTURE_PIXELS = 64 * 1024 * 1024

MAX_PALETTE_COLORS = MAX_PRINTABLE_COLORS

MIN_PALETTE_COLORS = MIN_PRINTABLE_COLORS

DEFAULT_PALETTE_COLORS = LEGACY_DEFAULT_PRINTABLE_COLORS

MODEL_FACE_LIMITS = (100000, 300000, 500000, 1000000, 2000000)

DEFAULT_MODEL_FACE_LIMIT = 300000

GENERATION_PROFILES = ("quality", "performance")

DEFAULT_GENERATION_PROFILE = "quality"

GENERATION_PROFILE_FACE_LIMITS = {"quality": 1000000, "performance": 300000}

MAX_GENERATION_ATTEMPTS = 1

JOB_STATE_FILENAME = "job.json"

JOB_STATE_VERSION = 1

MAX_JOB_STATE_BYTES = 64 * 1024

JOURNEY_EVENT_FILENAME = "journey-events.jsonl"

MAX_JOURNEY_EVENT_FILE_BYTES = 5 * 1024 * 1024

JOURNEY_EVENT_NAMES = frozenset({
    "preview_requested",
    "preview_ready",
    "preview_failed",
    "preview_regenerated",
    "preview_accepted",
    "model_submitted",
    "model_ready",
    "model_failed",
    "model_imported",
    "slice_requested",
    "print_feedback_success",
    "print_feedback_issue",
})

MAX_LOCAL_REPAIR_DIAGONAL_RATIO = 0.05

MAX_LOCAL_REPAIR_FACE_RATIO = 0.01

MAX_LOCAL_BOUNDARY_EDGES = 64

MAX_NOISE_COMPONENT_FACE_RATIO = 0.0001

MAX_NOISE_COMPONENT_DIAGONAL_RATIO = 0.01

MAX_TINY_COLOR_COMPONENT_AREA_RATIO = 0.0001

MAX_TINY_COLOR_COMPONENT_VERTEX_RATIO = 0.0005

MAX_COLOR_CLEANUP_SOURCE_AREA_RATIO = 0.10

MAX_COLOR_CLEANUP_SURFACE_AREA_RATIO = 0.005

MEANINGFUL_COLOR_SURFACE_AREA_RATIO = 0.02

MAX_COLOR_CLEANUP_PASSES = 2

MAX_COLOR_BOUNDARY_SURFACE_AREA_RATIO = 0.0025

MAX_COLOR_BOUNDARY_SOURCE_AREA_RATIO = 0.02

MIN_COLOR_BOUNDARY_SUPPORT_RATIO = 1.25

MAX_COLOR_BOUNDARY_SOURCE_NEIGHBORS = 1

MAX_COLOR_BOUNDARY_PASSES = 2

PORTRAIT_GARMENT_SMOOTHING_PASSES = 4

PORTRAIT_HAND_BOUNDARY_PASSES = 4

PORTRAIT_HAND_BOUNDARY_MAX_REMOVAL_RATIO = 0.25

PORTRAIT_HAND_BOUNDARY_MIN_PRIMARY_SUPPORT = 2

PORTRAIT_HAND_COMPACT_EXTENT_RATIO = 0.45

PORTRAIT_HAND_DIFFUSE_SIZE_RATIO = 2.5

PORTRAIT_HAND_MIN_HEIGHT_RATIO = 0.30

PORTRAIT_REAR_GARMENT_HEIGHT_RATIO = 0.70

PORTRAIT_REAR_HAIR_HEIGHT_RATIO = 0.58

PORTRAIT_FRONT_SURFACE_QUANTILE = 0.50

PORTRAIT_STRUCTURE_FRONT_QUANTILE = 0.60

PORTRAIT_FACE_DETAIL_MIN_HEIGHT_RATIO = 0.74

PORTRAIT_FACE_DETAIL_MAX_HEIGHT_RATIO = 0.94

PORTRAIT_FACE_DETAIL_HALF_WIDTH_RATIO = 0.23

PORTRAIT_FACE_DETAIL_SURFACE_TOLERANCE_MM = 0.35

PORTRAIT_FACE_DETAIL_GRID_SIZE = 256

PORTRAIT_GEOMETRY_PROVIDER_FILENAME = "geometry-provider-reference.png"

PORTRAIT_HEAD_PREVIEW_FILENAME = "portrait-head-shoulders-preview.png"

PORTRAIT_GEOMETRY_MAX_SUBJECT_OCCUPANCY = 0.88

PORTRAIT_HEAD_GEOMETRY_MAX_SUBJECT_OCCUPANCY = 0.96

PORTRAIT_REAR_PLATE_MIN_RUN_RATIO = 0.60

PORTRAIT_REAR_PLATE_MAX_START_RATIO = 0.15

DEFAULT_MODEL_SIZE_MM = 100.0

MODEL_ARTIFACT_FORMAT = "glb"

MODEL_QUALITY_FILENAME = "model-quality.json"

STYLE_IDS = (
    "sculpture", "realistic", "portrait_sketch", "cartoon", "low_poly", "relief", "ink_relief", "diorama", "custom",
)

LEGACY_STYLE_ALIASES = {
    "q_cartoon": "cartoon",
    "cel_shaded": "cartoon",
    "enamel_inlay": "realistic",
}

DEFAULT_IMAGE_INSTRUCTION = (
    "Stylize only the content already visible in the reference image. Preserve the exact crop, framing, visible regions, "
    "occlusions, subjects, objects, and background; do not add, remove, reveal, reconstruct, or extend anything."
)

@dataclass
class Job:
    id: str
    source: str
    directory: Path
    state: str = "preprocessing"
    phase: str = "preprocessing"
    message: str = "Preparing model generation request."
    progress: int = 5
    palette: tuple[str, ...] = field(default_factory=tuple)
    palette_roles: dict[str, str] = field(default_factory=dict)
    palette_color_count: int = DEFAULT_PALETTE_COLORS
    print_settings: dict[str, Any] = field(default_factory=lambda: asdict(PrintSettings()))
    style: str = "sculpture"
    custom_style: str = ""
    face_limit: int = DEFAULT_MODEL_FACE_LIMIT
    generation_profile: str = DEFAULT_GENERATION_PROFILE
    geometry_quality: str | None = None
    texture_quality: str = "standard"
    output_format: str = "glb"
    provider: str = "tripo"
    user_prompt: str = ""
    prepared_prompt: str = ""
    source_design_job_id: str = ""
    input_path: Path | None = None
    raw_preview_path: Path | None = None
    strict_preview_path: Path | None = None
    preview_path: Path | None = None
    model_reference_path: Path | None = None
    geometry_reference_path: Path | None = None
    preview_content_type: str = ""
    heatmap_path: Path | None = None
    metadata_path: Path | None = None
    background_mask_path: Path | None = None
    subject_mask_path: Path | None = None
    mask_paths: dict[str, Path] = field(default_factory=dict)
    image_metrics: dict[str, Any] = field(default_factory=dict)
    preprocess_failure: dict[str, Any] = field(default_factory=dict)
    artifact_path: Path | None = None
    artifact_format: str = ""
    color_intent_path: Path | None = None
    color_intent_schema: str = ""
    color_intent_sha256: str = ""
    palette_recommendation: dict[str, Any] = field(default_factory=dict)
    palette_recommendation_confirmed: bool = False
    generate_image: bool = False
    attempts: list[dict[str, Any]] = field(default_factory=list)
    updated_at: float = field(default_factory=time.time)
    design_started_monotonic: float | None = field(default=None, repr=False)
    stop_event: threading.Event = field(default_factory=threading.Event, repr=False)
    delete_requested: bool = field(default=False, repr=False)
    future: Future[Any] | None = field(default=None, repr=False)

class RequestError(Exception):
    def __init__(self, code: str, message: str, status: int, retryable: bool = False):
        super().__init__(message)
        self.code = code
        self.message = message
        self.status = status
        self.retryable = retryable


@dataclass(frozen=True)
class ApplicationResult:
    """Transport-neutral result of an explicit application command."""

    status: int
    payload: dict[str, Any]

class JobStopped(Exception):
    pass

class SidecarRestart(Exception):
    """Stops local work while keeping a paid remote task resumable."""

    pass

class PortraitMultiviewPreparationError(TripoError):
    """A recoverable, pre-paid portrait view preparation failure."""

    pass

class PortraitGeometryGateError(TripoError):
    """A paid portrait mesh has visually invalid large-scale geometry."""

    pass
