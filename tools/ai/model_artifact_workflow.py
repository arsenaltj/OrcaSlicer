"""ModelArtifactWorkflow: one application capability over explicit runtime ports."""
from __future__ import annotations

import json
import shutil
from ai_diagnostics import event as diagnostic_event
from dataclasses import dataclass
from glb_artifact import GlbError
from model_color_regions import (
    _consolidate_tiny_obj_color_components,
    _regularize_obj_color_boundaries,
)
from model_contracts import (
    DEFAULT_MODEL_SIZE_MM,
    Job,
    MAX_ARTIFACT_BYTES,
    MODEL_QUALITY_FILENAME,
    PortraitGeometryGateError,
)
from model_job_support import MAX_MODEL_FACES
from model_mesh_repair import (
    _remove_small_detached_obj_components,
    _repair_small_obj_topology_defects,
)
from model_obj_io import (
    _extract_obj_package,
    _normalize_obj_for_orca,
    _validate_obj_palette,
    _validate_obj_vertex_colors,
    _write_mesh_repair_report,
    _write_obj_vertex_color_metrics,
)
from model_texture_baking import _bake_obj_texture_to_vertex_colors, _quantize_vertex_color_obj
from openai_preprocessor import OpenAIPreprocessorError
from pathlib import Path
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
)
from printable_image_pipeline import PrintableImageError, process_printable_image
from printable_model_quality import (
    GATE_VERSION as MODEL_QUALITY_GATE_VERSION,
    ModelQualityError,
    ModelQualityThresholds,
)
from printable_model_views import ModelViewError, ModelViewSettings, render_model_views
from printable_multiview_reference import MultiviewReferenceError
from tripo_client import TripoError
from typing import Any, Callable, ContextManager, Mapping

@dataclass(frozen=True)
class ModelArtifactWorkflowPorts:
    persist: Callable[..., None]
    validate_artifact: Callable[..., None]
    prepare_obj: Callable[..., Path]
    analyze_printable_obj: Callable[..., Any]
    lock: ContextManager[Any]
    model_gateway: Callable[..., Any]
    multiview_paths_from_metrics: Callable[..., Any]
    prepare_generated_glb: Callable[..., Any]
    prepare_portrait_geometry_material_views: Callable[..., Any]
    progress_callback: Callable[..., Any]
    record_attempt: Callable[..., Any]
    stop_boundary: Callable[..., Any]
    tripo_gateway: Callable[[], Any]
    write_analysis_obj: Callable[..., Any]
    write_model_quality_report: Callable[..., Any]


class ModelArtifactWorkflow:
    def __init__(self, ports: ModelArtifactWorkflowPorts):
        self.ports = ports

    def prepare_obj_artifact(self,
        raw_download: Path,
        job_directory: Path,
        palette: tuple[str, ...],
        palette_roles: Mapping[str, str] | None = None,
        portrait_materials: bool = False,
        front_material_reference: Path | None = None,
        status_callback: Callable[[str, int], None] | None = None,
        build_aligned_portrait_reference: bool = False,
    ) -> Path:
        def report_status(message: str, progress: int) -> None:
            if status_callback is not None:
                status_callback(message, progress)

        try:
            with raw_download.open("rb") as stream:
                signature = stream.read(4)
        except OSError:
            raise TripoError("The generated artifact could not be read.") from None
        destination = job_directory / "model-vertex-color.obj"
        natural_portrait_obj = (
            job_directory / "portrait-natural-reference.tmp.obj"
            if build_aligned_portrait_reference and palette and portrait_materials
            else None
        )
        report_status("Converting textures into printable colors.", 96)
        if signature.startswith(b"PK\x03\x04"):
            archive = job_directory / "artifact-raw.zip"
            raw_download.replace(archive)
            obj_path = _extract_obj_package(archive, job_directory / "package")
            _bake_obj_texture_to_vertex_colors(
                obj_path,
                job_directory / "package",
                destination,
                palette,
                palette_roles,
                portrait_materials,
                natural_portrait_obj,
            )
        else:
            raw_obj = job_directory / "artifact-raw.obj"
            raw_download.replace(raw_obj)
            _validate_obj_vertex_colors(raw_obj)
            _quantize_vertex_color_obj(
                raw_obj, destination, palette, palette_roles, portrait_materials
            )
        _normalize_obj_for_orca(destination)
        effective_front_reference = front_material_reference
        geometry_aligned_reference = False
        geometry_aligned_view_directories: dict[str, Path] = {}
        if natural_portrait_obj is not None:
            aligned_report_path = job_directory / "portrait-aligned-reference.json"
            try:
                _normalize_obj_for_orca(natural_portrait_obj)
                aligned_directory = job_directory / "portrait-aligned-reference"
                natural_turntable = aligned_directory / "natural-turntable"
                # Rendering every triangle in four directions is intentionally
                # expensive for the quality profile.  Persist the stage before the
                # render starts so the desktop UI does not appear frozen on the
                # earlier texture-conversion message for several minutes.
                report_status("Rendering exact portrait material reference views.", 97)
                render_report = render_model_views(
                    natural_portrait_obj,
                    natural_turntable,
                    ModelViewSettings(
                        width=768,
                        height=768,
                        margin_ratio=0.06,
                        # Material ownership must see every generated triangle.
                        # QA views may be sampled, but a sampled semantic pass left
                        # visible skin/garment pinholes on high-detail portraits.
                        max_render_faces=MAX_MODEL_FACES,
                    ),
                    force=True,
                    include_isometric=False,
                    include_masks=True,
                    progress_callback=lambda _view, completed, total: report_status(
                        f"Rendering exact portrait material reference views ({completed}/{total}).",
                        97,
                    ),
                )
                rear_plate_report = _review_portrait_rear_plate_masks(
                    natural_turntable / "model-masks"
                )
                _write_mesh_repair_report(
                    aligned_directory / "rear-plate-gate.json", rear_plate_report
                )
                natural_front = aligned_directory / "natural-front.png"
                shutil.copyfile(natural_turntable / "model-views" / "front.png", natural_front)
                semantic_error = ""
                try:
                    report_status("Classifying skin and garment ownership in four views.", 97)
                    geometry_aligned_view_directories, semantic_report = (
                        self.ports.prepare_portrait_geometry_material_views(
                            natural_turntable,
                            aligned_directory / "semantic-materials",
                            palette_roles or {},
                        )
                    )
                    effective_front_reference = (
                        geometry_aligned_view_directories["front"] / "aligned_reference.png"
                    )
                    accepted = True
                    report = {
                        "status": "used",
                        "reason": "image2_semantic_material_gate_passed",
                        "render": render_report,
                        "semantic_materials": semantic_report,
                        "reference": str(effective_front_reference.relative_to(job_directory)),
                        "views": {
                            view: {
                                "reference": str(
                                    (directory / "aligned_reference.png").relative_to(job_directory)
                                ),
                                "mask": str(
                                    (directory / "mask_subject.png").relative_to(job_directory)
                                ),
                            }
                            for view, directory in geometry_aligned_view_directories.items()
                        },
                    }
                except (OpenAIPreprocessorError, PortraitProjectionError, MultiviewReferenceError, OSError) as exc:
                    # A paid geometry result remains recoverable if Image2 is
                    # temporarily unavailable. Keep the deterministic path as a
                    # fallback; the final visual gate still blocks mixed materials.
                    semantic_error = str(exc)
                    processed_views: dict[str, Any] = {}
                    geometry_aligned_view_directories = {}
                    for view in ("front", "right", "back", "left"):
                        view_directory = aligned_directory / "processed" / view
                        processed_views[view] = process_printable_image(
                            natural_turntable / "model-views" / f"{view}.png",
                            view_directory,
                            palette,
                            None,
                            palette_roles=palette_roles,
                        )
                        geometry_aligned_view_directories[view] = view_directory
                    processed = processed_views["front"]
                    accepted = bool(
                        processed.metrics.get("palette_quality_ok")
                        and processed.metrics.get("material_fragmentation_ok", True)
                    )
                    report = {
                        "status": "fallback" if accepted else "rejected",
                        "reason": (
                            "deterministic_material_fallback"
                            if accepted else "printable_reference_quality_gate_failed"
                        ),
                        "semantic_material_error": semantic_error,
                        "render": render_report,
                        "palette_quality_ok": bool(processed.metrics.get("palette_quality_ok")),
                        "material_fragmentation_ok": bool(
                            processed.metrics.get("material_fragmentation_ok", True)
                        ),
                        "quality_warnings": list(processed.metrics.get("quality_warnings", [])),
                        "reference": str(processed.clean_preview.relative_to(job_directory)),
                        "views": {
                            view: {
                                "reference": str(result.clean_preview.relative_to(job_directory)),
                                "portrait_skin_cleanup": result.metrics.get("portrait_skin_cleanup", {}),
                            }
                            for view, result in processed_views.items()
                        },
                    }
                    if accepted:
                        effective_front_reference = processed.clean_preview
                _write_mesh_repair_report(aligned_report_path, report)
                if accepted:
                    geometry_aligned_reference = True
                else:
                    geometry_aligned_view_directories = {}
            except (PortraitGeometryGateError, ModelViewError, PrintableImageError, TripoError, OSError) as exc:
                _write_mesh_repair_report(
                    aligned_report_path,
                    {
                        "status": "fallback",
                        "reason": str(exc),
                        "reference": (
                            str(front_material_reference.relative_to(job_directory))
                            if front_material_reference is not None
                            and front_material_reference.is_relative_to(job_directory)
                            else ""
                        ),
                    },
                )
            finally:
                try:
                    natural_portrait_obj.unlink(missing_ok=True)
                except OSError:
                    pass
        report_status("Separating portrait skin and garment materials.", 97)
        if palette and portrait_materials and effective_front_reference is not None:
            try:
                project_front_portrait_materials(
                    destination,
                    effective_front_reference,
                    job_directory / "front-material-projection.json",
                    palette_roles or {},
                )
            except PortraitProjectionError as exc:
                raise TripoError(str(exc)) from None
        if palette and portrait_materials:
            _stabilize_portrait_obj_materials(
                destination,
                job_directory / "portrait-material-cleanup.json",
                palette,
                palette_roles,
                True,
            )
        repair_report = _remove_small_detached_obj_components(destination, job_directory / "mesh-repair.json")
        _repair_small_obj_topology_defects(destination, job_directory / "mesh-repair.json", repair_report)
        captured_face_details: dict[int, tuple[int, int, int]] = {}
        face_detail_capture_report: dict[str, Any] = {"status": "not_applicable"}
        if palette and portrait_materials and effective_front_reference is None:
            captured_face_details, face_detail_capture_report = _capture_portrait_front_face_details(
                destination, palette_roles
            )
        report_status("Cleaning printable color regions.", 98)
        if palette:
            report_status("Merging tiny printable color islands.", 98)
            _consolidate_tiny_obj_color_components(destination, job_directory / "vertex-color-cleanup.json")
            report_status("Smoothing printable material boundaries.", 98)
            _regularize_obj_color_boundaries(destination, job_directory / "color-boundary-cleanup.json")
            if portrait_materials:
                report_status("Keeping skin, sleeves and garments in their own materials.", 98)
                _stabilize_portrait_obj_garment_regions(
                    destination,
                    job_directory / "portrait-garment-cleanup.json",
                    palette,
                    palette_roles,
                    True,
                )
            # Restore only dark vertices in the strict front-centre garment region.
            # This recovers a real scarf or shirt that generic cleanup mapped to hair
            # colour without allowing the reference to repaint skin, white sleeves,
            # the head, the back, or the base.
            if portrait_materials and effective_front_reference is not None:
                report_status("Restoring the approved front garment material.", 98)
                try:
                    project_front_portrait_materials(
                        destination,
                        effective_front_reference,
                        job_directory / "front-accent-projection.json",
                        palette_roles or {},
                        repair_skin=False,
                        restore_accent=True,
                    )
                except PortraitProjectionError as exc:
                    raise TripoError(str(exc)) from None
                _consolidate_tiny_obj_color_components(
                    destination,
                    job_directory / "front-material-color-cleanup.json",
                )
                _regularize_obj_color_boundaries(
                    destination,
                    job_directory / "front-material-boundary-cleanup.json",
                )
                # A front projection can reconnect real blouse panels, but it may
                # also recreate dark lighting folds. Re-run the semantic ownership
                # pass before touching the face so the final garment is continuous.
                report_status("Removing remaining portrait material cross-colour.", 98)
                _stabilize_portrait_obj_garment_regions(
                    destination,
                    job_directory / "portrait-final-cleanup.json",
                    palette,
                    palette_roles,
                    True,
                )
                # The semantic pass above intentionally removes suspicious accent
                # islands, but on crossed-arm portraits it can also reopen a thin
                # white seam through a real continuous blouse. Re-apply only the
                # high-confidence front-centre accent label from the approved
                # reference, then clean its boundary before touching the face.
                try:
                    report_status("Finalizing the approved front garment boundary.", 98)
                    project_front_portrait_materials(
                        destination,
                        effective_front_reference,
                        job_directory / "front-final-accent-projection.json",
                        palette_roles or {},
                        repair_skin=False,
                        restore_accent=True,
                    )
                except PortraitProjectionError as exc:
                    raise TripoError(str(exc)) from None
                _consolidate_tiny_obj_color_components(
                    destination,
                    job_directory / "front-final-accent-color-cleanup.json",
                )
                _regularize_obj_color_boundaries(
                    destination,
                    job_directory / "front-final-accent-boundary-cleanup.json",
                )
                # Normalize only the actually visible central face from the exact,
                # same-source material reference. This removes false eye/skin
                # islands, restores high-confidence dark linework, and permits only
                # geometry-aligned lower-face tooth evidence (never white eye patches).
                try:
                    report_status("Restoring source-faithful facial material details.", 98)
                    normalization_options: dict[str, Any] = {
                        "repair_skin": False,
                        "normalize_face_details": True,
                    }
                    if geometry_aligned_reference:
                        normalization_options["reference_is_geometry_aligned"] = True
                    project_front_portrait_materials(
                        destination,
                        effective_front_reference,
                        job_directory / "front-face-normalization.json",
                        palette_roles or {},
                        **normalization_options,
                    )
                except PortraitProjectionError as exc:
                    raise TripoError(str(exc)) from None
                # The face projection deliberately adds a small amount of dark
                # printable linework. Consolidate it once more so isolated pixels
                # cannot survive as freckles, eyeliner fragments, or cheek seams;
                # coherent brows and pupils remain protected by component area.
                _consolidate_tiny_obj_color_components(
                    destination,
                    job_directory / "front-face-color-cleanup.json",
                )
                _regularize_obj_color_boundaries(
                    destination,
                    job_directory / "front-face-boundary-cleanup.json",
                )
            elif portrait_materials:
                # Older and single-view jobs have no exact material reference. Keep
                # their provider-aligned face labels on the original visible mesh
                # vertices rather than projecting an unrelated image through the
                # cheeks or rear of the head.
                _restore_portrait_front_face_details(
                    destination,
                    job_directory / "front-face-detail-restoration.json",
                    captured_face_details,
                    face_detail_capture_report,
                )
            if portrait_materials and len(geometry_aligned_view_directories) >= 2:
                try:
                    report_status("Applying four-view skin and garment ownership.", 98)
                    project_geometry_aligned_portrait_materials(
                        destination,
                        geometry_aligned_view_directories,
                        job_directory / "geometry-material-projection.json",
                        palette_roles or {},
                        margin_ratio=0.06,
                    )
                except PortraitProjectionError as exc:
                    raise TripoError(str(exc)) from None
                # The semantic projection is the final source of material truth.
                # Clean after—not before—it, otherwise a late projection can
                # recreate the exact freckles and wrist/base leakage we removed.
                _consolidate_tiny_obj_color_components(
                    destination,
                    job_directory / "geometry-material-color-cleanup.json",
                )
                _regularize_obj_color_boundaries(
                    destination,
                    job_directory / "geometry-material-boundary-cleanup.json",
                )
                # Do not run the legacy connected-component garment heuristic after
                # exact-mesh four-view projection. On crossed-arm portraits it
                # classified both correctly projected hands as jacket noise and
                # erased them. Accent and base ownership are already guarded by the
                # geometry projection itself.
                # The four-view projection already resolved visibility against the
                # actual mesh and is therefore the authoritative source for pupils,
                # brows, teeth and garment ownership.  A former planar face pass
                # ran after this cleanup and erased the correctly projected eyes on
                # a real head/shoulder beta model because its fixed vertical bands
                # were calibrated for a different bust proportion.  Finish with
                # conservative component and boundary cleanup instead: it removes
                # isolated freckles while retaining coherent, mesh-visible facial
                # features from the four-view result.
                _consolidate_tiny_obj_color_components(
                    destination,
                    job_directory / "geometry-face-color-cleanup.json",
                )
                _regularize_obj_color_boundaries(
                    destination,
                    job_directory / "geometry-face-boundary-cleanup.json",
                )
            _validate_obj_palette(destination, palette)
        else:
            _validate_obj_vertex_colors(destination)
        _write_obj_vertex_color_metrics(destination, job_directory / "vertex-color-metrics.json")
        self.ports.validate_artifact(destination, "obj", allow_repairable_obj=True)
        report_status("Checking the high-detail mesh for printing.", 99)
        quality = self.ports.analyze_printable_obj(
            destination,
            ModelQualityThresholds(max_faces=MAX_MODEL_FACES),
            allow_repairable_topology=True,
            target_palette=palette,
        )
        try:
            self.ports.write_model_quality_report(quality, job_directory / MODEL_QUALITY_FILENAME)
        except ModelQualityError:
            diagnostic_event("model.quality_report.unavailable", level="WARNING")
        return destination

    def refresh_stale_face_limit_report(self, path: Path, palette: tuple[str, ...]) -> None:
        """Recheck an already-processed high-detail OBJ rejected by the old 1M gate."""

        report_path = path.parent / MODEL_QUALITY_FILENAME
        try:
            report = json.loads(report_path.read_text(encoding="utf-8"))
        except (OSError, UnicodeError, json.JSONDecodeError):
            return
        if not isinstance(report, Mapping):
            return
        errors = report.get("errors", [])
        thresholds = report.get("thresholds", {})
        previous_limit = thresholds.get("max_faces", 0) if isinstance(thresholds, Mapping) else 0
        if not isinstance(errors, list) or "too_many_faces" not in errors:
            return
        try:
            previous_limit = int(previous_limit)
        except (TypeError, ValueError):
            previous_limit = 0
        if previous_limit >= MAX_MODEL_FACES and report.get("gate_version") == MODEL_QUALITY_GATE_VERSION:
            return

        quality = self.ports.analyze_printable_obj(
            path,
            ModelQualityThresholds(max_faces=MAX_MODEL_FACES),
            allow_repairable_topology=True,
            target_palette=palette,
        )
        try:
            self.ports.write_model_quality_report(quality, report_path)
        except ModelQualityError as exc:
            raise TripoError(str(exc)) from None

    def analysis_artifact(self, artifact: Path) -> Path:
        if artifact.suffix.lower() != ".glb":
            return artifact
        analysis = artifact.parent / "analysis-model.obj"
        if not analysis.is_file() or analysis.stat().st_mtime_ns < artifact.stat().st_mtime_ns:
            try:
                self.ports.write_analysis_obj(artifact, analysis)
            except (GlbError, OSError, ValueError, KeyError, TypeError) as exc:
                raise TripoError(f"The GLB could not be read for model checks: {exc}") from None
        return analysis

    def download_generation_artifact(self, job: Job, generation_id: str, attempt_number: int = 1, resume: bool = False) -> Path:
        existing = job.attempts[attempt_number - 1] if len(job.attempts) >= attempt_number else {}
        # GLB consumes the original generation result. OBJ explicitly includes one
        # basic provider conversion; frozen conversions keep their existing task ID.
        if job.provider == "tripo" and (existing.get("conversion_task_id") or job.output_format == "obj"):
            return self.download_conversion(job, generation_id, "obj", attempt_number, resume)
        directory = job.directory / f"attempt-{attempt_number:02d}"
        directory.mkdir(exist_ok=True)
        candidate = directory / "model.glb"
        if resume and candidate.is_file():
            try:
                self.ports.validate_artifact(candidate, "glb")
                self.analysis_artifact(candidate)
                return candidate
            except TripoError:
                diagnostic_event("model.glb_cache.invalid", level="WARNING")
        with self.ports.lock:
            job.phase = "downloading_artifact"
            job.message = "Downloading the generated model."
            job.progress = 75
            self.ports.persist(job)
        gateway = self.ports.model_gateway(job.provider)
        result = gateway.wait_for_task(generation_id, stop_event=job.stop_event)
        self.ports.stop_boundary(job)
        raw = directory / "artifact-raw.download"
        if job.provider == "hunyuan":
            gateway.download_artifact(result, raw, MAX_ARTIFACT_BYTES, output_format=job.output_format)
        else:
            gateway.download_artifact(result, raw, MAX_ARTIFACT_BYTES)
        self.ports.stop_boundary(job)
        with raw.open("rb") as stream:
            is_glb = stream.read(4) == b"glTF"
        if not is_glb:
            # Retain compatibility with providers/frozen jobs that return OBJ/ZIP.
            if resume and (directory / "package").exists():
                # Keep an interrupted extraction intact and retry in a fresh directory.
                recovery_number = 1
                while (directory / f"recovery-{recovery_number:02d}").exists():
                    recovery_number += 1
                directory = directory / f"recovery-{recovery_number:02d}"
                directory.mkdir(parents=False, exist_ok=False)
                raw = raw.replace(directory / "artifact-raw.download")
            return self.ports.prepare_obj(raw, directory, job.palette, job.palette_roles)
        original = directory / "provider-model.glb"
        raw.replace(original)
        analysis = directory / "analysis-model.obj"
        try:
            self.ports.prepare_generated_glb(original, candidate, analysis, DEFAULT_MODEL_SIZE_MM)
        except (GlbError, OSError, ValueError, KeyError, TypeError) as exc:
            raise TripoError(f"The generated GLB could not be prepared: {exc}") from None
        self.ports.stop_boundary(job)
        with self.ports.lock:
            job.phase = "checking_model"
            job.message = "Checking model geometry and colors."
            job.progress = 99
            self.ports.persist(job)
        quality = self.ports.analyze_printable_obj(analysis, ModelQualityThresholds(max_faces=MAX_MODEL_FACES),
                                        allow_repairable_topology=True)
        try:
            self.ports.write_model_quality_report(quality, directory / MODEL_QUALITY_FILENAME)
        except ModelQualityError:
            diagnostic_event("model.quality_report.unavailable", level="WARNING")
        _write_obj_vertex_color_metrics(analysis, directory / "vertex-color-metrics.json")
        return candidate

    def download_conversion(self,
        job: Job, generation_id: str, format_name: str, attempt_number: int = 1, resume: bool = False
    ) -> Path:
        with self.ports.lock:
            job.state = "running"
            job.phase = "converting"
            job.message = f"Converting generated geometry to {format_name.upper()}."
            job.progress = 75
            self.ports.persist(job)
        existing = job.attempts[attempt_number - 1] if len(job.attempts) >= attempt_number else {}
        conversion_id = existing.get("conversion_task_id", "")
        if not conversion_id:
            if job.output_format != "obj" or existing.get("conversion_submission_started"):
                raise TripoError("Conversion submission has no confirmed task ID; it will not be submitted again.")
            self.ports.stop_boundary(job)
            # Persist before the paid POST. An interrupted response must never create
            # another conversion during recovery, even if the server accepted it.
            self.ports.record_attempt(job, attempt_number, conversion_submission_started=True)
            self.ports.persist(job, required=True)
        conversion_ref = self.ports.tripo_gateway().start_or_reuse_conversion(
            generation_id,
            format_name,
            existing_task_id=conversion_id if isinstance(conversion_id, str) else "",
            allow_create=job.output_format == "obj" and not conversion_id,
        )
        conversion_id = conversion_ref.task_id
        if not conversion_ref.reused:
            self.ports.record_attempt(job, attempt_number, conversion_task_id=conversion_id)
        attempt_directory = job.directory / f"attempt-{attempt_number:02d}"
        attempt_directory.mkdir(parents=False, exist_ok=True)
        if resume and format_name == "obj":
            candidates = sorted(attempt_directory.rglob("model-vertex-color.obj"), reverse=True)
            for candidate in candidates:
                try:
                    candidate.resolve().relative_to(attempt_directory.resolve())
                    self.ports.validate_artifact(candidate, "obj", allow_repairable_obj=True)
                    self.refresh_stale_face_limit_report(candidate, job.palette)
                except (OSError, TripoError, ValueError):
                    continue
                return candidate
        self.ports.stop_boundary(job)
        result = self.ports.tripo_gateway().wait_for_task(
            conversion_id,
            stop_event=job.stop_event,
            progress=self.ports.progress_callback(job, 75, 95),
        )
        self.ports.stop_boundary(job)
        with self.ports.lock:
            job.phase = "downloading_artifact"
            job.message = "Preparing the generated artifact."
            job.progress = 95
            self.ports.persist(job)
        work_directory = attempt_directory
        if resume:
            recovery_number = 1
            while (attempt_directory / f"recovery-{recovery_number:02d}").exists():
                recovery_number += 1
            work_directory = attempt_directory / f"recovery-{recovery_number:02d}"
            work_directory.mkdir(parents=False, exist_ok=False)
        destination = work_directory / "artifact-raw.download"
        self.ports.tripo_gateway().download_artifact(result, destination, MAX_ARTIFACT_BYTES)
        self.ports.stop_boundary(job)
        if format_name == "obj":
            portrait_cleanup = job.image_metrics.get("portrait_skin_cleanup", {})
            portrait_materials = (
                job.style == "realistic"
                and isinstance(portrait_cleanup, dict)
                and portrait_cleanup.get("activated") == 1
            )
            effective_palette_roles = dict(job.palette_roles)
            if portrait_materials:
                garment_color = str(portrait_cleanup.get("garment_color", "")).strip().upper()
                skin_color = str(portrait_cleanup.get("skin_color", "")).strip().upper()
                if garment_color in job.palette and skin_color in job.palette and garment_color != skin_color:
                    effective_palette_roles["primary"] = garment_color
                    effective_palette_roles["light"] = skin_color
            material_views = self.ports.multiview_paths_from_metrics(job, "material_views")
            build_aligned_reference = bool(
                portrait_materials
                and job.generation_profile == "quality"
                and job.palette
            )
            def artifact_status(message: str, progress: int) -> None:
                self.ports.stop_boundary(job)
                with self.ports.lock:
                    job.phase = "checking_model"
                    job.message = message
                    job.progress = progress
                    self.ports.persist(job)
                self.ports.stop_boundary(job)

            prepare_options: dict[str, Any] = {}
            if build_aligned_reference:
                prepare_options["build_aligned_portrait_reference"] = True
            if material_views is not None:
                return self.ports.prepare_obj(
                    destination,
                    work_directory,
                    job.palette,
                    effective_palette_roles,
                    portrait_materials,
                    material_views["front"],
                    artifact_status,
                    **prepare_options,
                )
            return self.ports.prepare_obj(
                destination,
                work_directory,
                job.palette,
                effective_palette_roles,
                portrait_materials,
                None,
                artifact_status,
                **prepare_options,
            )
        self.ports.validate_artifact(destination, format_name)
        return destination
