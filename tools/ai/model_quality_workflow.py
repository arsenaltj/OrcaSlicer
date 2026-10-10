"""ModelQualityWorkflow: one application capability over explicit runtime ports."""
from __future__ import annotations

import hashlib
import json
import os
import re
import shutil
import tempfile
from dataclasses import dataclass
from glb_artifact import GlbError
from model_contracts import (
    Job,
    MAX_ARTIFACT_BYTES,
    PORTRAIT_GEOMETRY_PROVIDER_FILENAME,
    RequestError,
)
from model_input_image_quality import ModelInputImageQualityError, assess_model_input_image
from openai_preprocessor import IDENTITY_FIRST_PORTRAIT_STYLES
from pathlib import Path
from portrait_geometry_reference import (
    _geometry_generation_reference,
    _identity_preserving_portrait_geometry_enabled,
)
from printable_model_quality import GATE_VERSION as MODEL_QUALITY_GATE_VERSION, ModelQualityError
from printable_visual_quality import REPORT_FILENAME as VISUAL_QUALITY_FILENAME
from tripo_client import TripoError
from typing import Any, Callable, ContextManager, Mapping

@dataclass(frozen=True)
class ModelQualityWorkflowPorts:
    persist: Callable[..., None]
    assess_generation_reference: Callable[..., Any]
    analysis_artifact: Callable[..., Any]
    analyze_printable_obj: Callable[..., Any]
    assess_job_model_reference: Callable[..., Any]
    lock: ContextManager[Any]
    model_output_root: Callable[..., Any]
    read_job_report: Callable[..., Any]
    review_prepared_reference: Callable[..., Any]
    write_model_quality_report: Callable[..., Any]


class ModelQualityWorkflow:
    def __init__(self, ports: ModelQualityWorkflowPorts):
        self.ports = ports

    def check_saved_model(self, request: Mapping[str, Any]) -> dict[str, Any]:
        """Check the registered saved artifact without creating/adopting a generation job."""
        asset_id = request.get("asset_id")
        expected = request.get("artifact_sha256")
        read_only = request.get("read_only", False)
        if not isinstance(read_only, bool):
            raise RequestError("invalid_request", "read_only must be a boolean.", 400)
        if not isinstance(asset_id, str) or re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_-]{0,127}", asset_id) is None:
            raise RequestError("invalid_request", "A registered model asset ID is required.", 400)
        if not isinstance(expected, str) or re.fullmatch(r"[0-9a-f]{64}", expected) is None:
            raise RequestError("invalid_request", "The saved model fingerprint is required.", 400)
        root = self.ports.model_output_root()
        record_path = root / "downloads" / f"orcaslicer-ai-{asset_id}.json"
        try:
            if record_path.is_symlink() or record_path.resolve(strict=True) != record_path:
                raise ValueError("unregistered record")
            # Region documents can be larger than a generation job manifest.
            if record_path.stat().st_size > 64 * 1024 * 1024:
                raise ValueError("oversized record")
            record = json.loads(record_path.read_text(encoding="utf-8"))
            if not isinstance(record, dict) or record.get("job_id") != asset_id:
                raise ValueError("unregistered record")
            relative = Path(record["model_path"])
            if relative.is_absolute() or ".." in relative.parts:
                raise ValueError("unregistered artifact")
            artifact = root / relative
            if artifact.resolve(strict=True) != artifact:
                raise ValueError("linked artifact")
            artifact.relative_to(root)
            if artifact.suffix.lower() not in {".obj", ".glb"} or not 0 < artifact.stat().st_size <= MAX_ARTIFACT_BYTES:
                raise ValueError("unavailable artifact")
        except (OSError, ValueError, KeyError, TypeError, UnicodeError):
            raise RequestError("artifact_not_ready", "The registered saved model is unavailable.", 409) from None

        def fingerprint(path: Path) -> str:
            with path.open("rb") as stream:
                return hashlib.file_digest(stream, "sha256").hexdigest()

        try:
            if fingerprint(artifact) != expected or record.get("model_sha256", expected) != expected:
                raise RequestError("artifact_changed", "The saved model changed; reload it before checking.", 409)
            cache = root / ".model-checks"
            if read_only:
                # History must only restore a report for these exact bytes. Missing,
                # legacy or incompatible reports remain unchecked; never analyze here.
                quality = {}
                report = cache / f"{expected}-quality.json"
                try:
                    if cache.is_symlink() or cache.resolve(strict=True) != cache:
                        raise ValueError("linked report directory")
                    if report.is_symlink() or report.resolve(strict=True) != report or not 0 < report.stat().st_size <= 1024 * 1024:
                        raise ValueError("unavailable report")
                    candidate = json.loads(report.read_text(encoding="utf-8"))
                    if (isinstance(candidate, dict) and candidate.get("artifact_sha256") == expected
                            and candidate.get("units") == "mm"
                            and candidate.get("gate_version") == MODEL_QUALITY_GATE_VERSION):
                        quality = candidate
                except (OSError, ValueError, UnicodeError):
                    pass
                if fingerprint(artifact) != expected:
                    raise RequestError("artifact_changed", "The saved model changed during report recovery.", 409)
                return {"id": asset_id, "source": "saved_model", "state": "ready", "model_quality": quality}
            cache.mkdir(exist_ok=True)
            if cache.is_symlink() or cache.resolve() != cache:
                raise RequestError("quality_report_unavailable", "The model check storage is unavailable.", 503, True)
            # Work from an immutable copy. Each request has a private projection;
            # no shared analysis-model.obj in downloads can belong to another model.
            with tempfile.TemporaryDirectory(prefix="check-", dir=cache) as directory:
                snapshot = Path(directory) / ("model" + artifact.suffix.lower())
                shutil.copyfile(artifact, snapshot)
                if fingerprint(snapshot) != expected:
                    raise RequestError("artifact_changed", "The saved model changed during checking.", 409)
                analysis = self.ports.analysis_artifact(snapshot)
                quality = self.ports.analyze_printable_obj(analysis, allow_repairable_topology=True)
                if fingerprint(artifact) != expected:
                    raise RequestError("artifact_changed", "The saved model changed during checking.", 409)
                quality["artifact_sha256"] = expected
                quality["units"] = "mm"
                report = self.ports.write_model_quality_report(quality, Path(directory) / "quality.json")
                os.replace(report, cache / f"{expected}-quality.json")
        except (OSError, GlbError, TripoError, ModelQualityError, ValueError) as exc:
            raise RequestError("quality_report_unavailable", "The saved model could not be checked. Its files are unchanged.", 503, True) from exc
        return {"id": asset_id, "source": "saved_model", "state": "ready", "model_quality": quality}

    def assess_reference_advice(self, job: Job) -> tuple[dict[str, Any], dict[str, Any]]:
        """Retain cheap image findings without making heuristic failures a gate.

        Image decoding and provider format/size validation stay at the input
        boundary. An unavailable quality assessment is itself only advice.
        """
        reports = []
        for key, assess in (
            ("model_input_quality", self.ports.assess_job_model_reference),
            ("generation_input_quality", self.ports.assess_generation_reference),
        ):
            try:
                report = assess(job)
            except ModelInputImageQualityError:
                report = {
                    "model_input_eligible": True,
                    "warnings": ["reference_quality_unavailable"],
                    "blockers": [],
                    "status": "unavailable",
                }
                job.image_metrics[key] = report
            reports.append(report)
        return reports[0], reports[1]

    def assess_job_generation_reference(self, job: Job) -> dict[str, Any]:
        reference = _geometry_generation_reference(job)
        if reference is None:
            raise ModelInputImageQualityError("The model generation image is unavailable.")
        quality = assess_model_input_image(
            reference,
            reject_rectangular_cutouts=_identity_preserving_portrait_geometry_enabled(job),
        )
        if job.image_metrics.get("design_reference") == "ai-design-v1":
            strategy = "ai_design"
        elif _identity_preserving_portrait_geometry_enabled(job):
            provider_reference = job.directory / PORTRAIT_GEOMETRY_PROVIDER_FILENAME
            provider_canvas = job.image_metrics.get("geometry_provider_canvas", {})
            color_provider = (
                reference == provider_reference
                and isinstance(provider_canvas, Mapping)
                and provider_canvas.get("appearance_source")
                == "identity_color_model_reference"
            )
            strategy = (
                "identity_color_geometry_reference"
                if color_provider
                else "identity_sculpted_geometry_reference"
                if reference == job.geometry_reference_path or reference == provider_reference
                else "identity_locked_model_reference"
                if reference != job.input_path
                else "original_identity_image"
            )
        elif reference == job.raw_preview_path and bool(job.palette):
            strategy = "raw_preview"
        else:
            strategy = "model_reference"
        # Generic cutout analysis downscales the reference and detects enclosed
        # rectangles well, but an exterior shoulder notch is open to the background
        # and can disappear at that resolution. The portrait-specific native-size
        # repair is therefore an independent, paid-task hard gate. Existing paid
        # attempts keep their immutable evidence and are not retroactively rejected.
        if identity_locked := strategy in {
            "identity_color_geometry_reference",
            "identity_sculpted_geometry_reference",
            "identity_locked_model_reference",
            "original_identity_image",
        }:
            provider_canvas = job.image_metrics.get("geometry_provider_canvas", {})
            compaction = (
                provider_canvas.get("portrait_compaction", {})
                if isinstance(provider_canvas, Mapping)
                else {}
            )
            shoulder_silhouette = (
                compaction.get("shoulder_silhouette", {})
                if isinstance(compaction, Mapping)
                else {}
            )
            compact_portrait = bool(
                isinstance(compaction, Mapping) and compaction.get("applied") is True
            )
            silhouette_verified = bool(
                isinstance(shoulder_silhouette, Mapping)
                and shoulder_silhouette.get("status") == "pass"
                and shoulder_silhouette.get("source_mask_used") is True
                and int(shoulder_silhouette.get("remaining_row_gap_pixels", -1)) == 0
            )
            if compact_portrait and not job.attempts and not silhouette_verified:
                existing = quality.get("blockers", [])
                blocker_codes = (
                    [str(code) for code in existing]
                    if isinstance(existing, list)
                    else []
                )
                quality["blockers"] = list(dict.fromkeys(
                    ["portrait_shoulder_silhouette_unverified"] + blocker_codes
                ))
                quality["model_input_eligible"] = False
                quality["score"] = min(float(quality.get("score", 100)), 60.0)
        job.image_metrics["generation_input_quality"] = quality
        job.image_metrics["generation_reference"] = strategy
        job.image_metrics["geometry_strategy"] = {
            "version": (
                "portrait-sculpted-head-shoulders-front-v16"
                if strategy == "identity_sculpted_geometry_reference"
                and isinstance(job.image_metrics.get("geometry_provider_canvas"), Mapping)
                and job.image_metrics["geometry_provider_canvas"].get("version")
                == "square-transparent-black-head-shoulders-v10"
                else "portrait-identity-color-front-v8"
                if strategy == "identity_color_geometry_reference"
                else
                "portrait-identity-sculpted-front-v7"
                if strategy == "identity_sculpted_geometry_reference"
                else "portrait-identity-front-v4"
                if identity_locked
                else "default-v1"
            ),
            "reference": strategy,
            "multiview_geometry": False if identity_locked else None,
            "post_generation_material_turntable": True if identity_locked else None,
            "identity_review_reference": strategy if identity_locked else None,
            "provider_autofix": False if identity_locked else None,
        }
        return quality

    def apply_preview_visual_quality_gate(self, job: Job, report: Mapping[str, Any]) -> list[str]:
        """Merge a live or persisted visual decision into both paid-input gates."""

        for key in ("model_input_quality", "generation_input_quality"):
            quality = job.image_metrics.get(key)
            if isinstance(quality, dict):
                warning = "reference_visual_review_unavailable"
                warnings = [code for code in quality.get("warnings", []) if code != warning]
                if report.get("status") == "unavailable":
                    warnings.append(warning)
                quality["warnings"] = warnings
        blockers = [str(code) for code in report.get("blocking_warnings", [])]
        if bool(report.get("model_generation_recommended", True)) or not blockers:
            return []
        priority = (
            "preview_identity_mismatch",
            "preview_face_geometry_drift",
            "preview_age_expression_drift",
            "preview_material_mixing",
            "preview_base_mixing",
            "preview_pose_clothing_drift",
            "preview_modeling_reference_unclear",
        )
        ordered = [code for code in priority if code in blockers]
        ordered.extend(code for code in blockers if code not in ordered)
        for key in ("model_input_quality", "generation_input_quality"):
            quality = job.image_metrics.get(key)
            if not isinstance(quality, dict):
                continue
            existing = quality.get("blockers", [])
            existing_codes = [str(code) for code in existing] if isinstance(existing, list) else []
            quality["blockers"] = list(dict.fromkeys(ordered + existing_codes))
            quality["model_input_eligible"] = False
            quality["score"] = min(float(quality.get("score", 100)), float(report.get("score", 0)))
        return ordered

    def assess_job_preview_visual_quality(self, job: Job, original: Path) -> dict[str, Any] | None:
        """Gate high-quality realistic portrait previews before paid 3D submission."""

        preprocessing = job.image_metrics.get("image_preprocessing_policy", {})
        if (
            job.source != "image"
            or (isinstance(preprocessing, Mapping) and preprocessing.get("specialized", False))
            or job.style not in IDENTITY_FIRST_PORTRAIT_STYLES
            or job.generation_profile != "quality"
            or job.model_reference_path is None
            or job.preview_path is None
            or not os.environ.get("OPENAI_API_KEY", "")
        ):
            return None
        with self.ports.lock:
            job.phase = "checking_image"
            job.message = "Comparing the prepared face and material ownership with the original image."
            job.progress = 14
            self.ports.persist(job)
        # Gate the exact image that will be submitted for geometry. The restored
        # face preview can look nearly pixel-identical while hiding an identity
        # drift in the sculptural Image2 output that Tripo actually receives.
        submitted_reference = _geometry_generation_reference(job) or job.model_reference_path
        report = self.ports.review_prepared_reference(
            original,
            submitted_reference,
            submitted_reference if job.image_metrics.get("design_reference") == "ai-design-v1" else job.preview_path,
            job.directory / "preview-visual-review",
        )
        job.image_metrics["preview_visual_quality"] = report
        self.apply_preview_visual_quality_gate(job, report)
        return report

    def automatic_visual_review(self, job: Job, artifact: Path) -> dict[str, Any] | None:
        """Reuse existing advice; the explicit visual-review action performs new reviews.

        Full-mesh turntables plus a remote review took over five minutes for a
        two-million-face model. They must not delay an otherwise usable result.
        """
        return self.ports.read_job_report(job, VISUAL_QUALITY_FILENAME) or None
