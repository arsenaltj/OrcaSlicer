"""PortraitMultiviewWorkflow: one application capability over explicit runtime ports."""
from __future__ import annotations

import re
from dataclasses import dataclass
from model_contracts import Job, PortraitMultiviewPreparationError
from model_input_image_quality import ModelInputImageQualityError, assess_model_input_image
from model_obj_io import _write_mesh_repair_report
from model_request import _validate_image_file
from openai_preprocessor import OpenAIPreprocessorError
from pathlib import Path
from portrait_geometry_reference import _geometry_generation_reference
from portrait_multiview_cleanup import (
    PortraitProjectionError,
    quantize_geometry_aligned_material_reference,
)
from printable_image_pipeline import PrintSettings, PrintableImageError
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
from typing import Any, Callable, ContextManager, Mapping

@dataclass(frozen=True)
class PortraitMultiviewWorkflowPorts:
    persist: Callable[..., None]
    complete_vision: Callable[..., Any]
    edit_image: Callable[..., Any]
    lock: ContextManager[Any]
    stop_boundary: Callable[..., Any]


class PortraitMultiviewWorkflow:
    def __init__(self, ports: PortraitMultiviewWorkflowPorts):
        self.ports = ports

    def quality_portrait_multiview_enabled(self, job: Job) -> bool:
        cleanup = job.image_metrics.get("portrait_skin_cleanup", {})
        return (
            job.source == "image"
            and job.style == "realistic"
            and job.generation_profile == "quality"
            and isinstance(cleanup, dict)
            and cleanup.get("activated") == 1
            and job.model_reference_path is not None
        )

    def portrait_multiview_prompt(self, background_instruction: str | None = None) -> str:
        # Keep this below conservative image-edit prompt limits. The supplied image
        # owns identity, materials, pose and base; the prompt owns only view layout.
        background_direction = background_instruction or (
            "Use a genuine alpha-transparent background in every panel; do not paint a gray, white, checkerboard, studio or gradient "
            "background and do not add shadows. "
        )
        return (
            "Create one 2x2 orthographic turntable sheet of this exact finished portrait bust. "
            "Panels must be top-left front, top-right subject-left, bottom-left back, bottom-right subject-right. "
            "Render one unchanged sculpture at yaw 0, 90, 180 and 270 degrees, same scale, zero pitch and roll, full object visible. "
            "Preserve the exact real-person identity, adult age, head and face proportions, eyes, nose, cheeks, smile, lips, jaw, hair, "
            "pose, hands, clothing, accessories, material boundaries and integrated base. Never substitute a generic, doll, anime or "
            "caricature face. The front head must remain level with both eyes and the complete source smile readable; keep facial relief "
            "and natural asymmetry consistent across the side views. Reconstruct genuine rounded depth for the skull, hair, shoulders, "
            "jacket and torso in side and back views. No flat backing board, person-shaped wall, silhouette plate, photo cutout, rear sheet "
            "or straight vertical panel may exist behind the portrait; the back must be the actual curved back of the same bust. Keep broad "
            "clean printable color regions with no skin color on "
            "clothing and no black shadow patches inside a colored blouse. A partially hidden hand must be either one compact bounded "
            "skin region or fully tucked under the existing sleeve, never a skin stripe on the jacket. "
            + background_direction
            +
            "No labels, borders, extra views, perspective, added objects or changed geometry."
        )

    def portrait_geometry_material_prompt(self, palette_roles: Mapping[str, str]) -> str:
        try:
            primary = str(palette_roles["primary"]).strip().upper()
            structure = str(palette_roles["structure"]).strip().upper()
            skin = str(palette_roles["light"]).strip().upper()
            accent = str(palette_roles["accent"]).strip().upper()
        except KeyError:
            raise PortraitProjectionError("The portrait material roles are incomplete.") from None
        if any(not re.fullmatch(r"#[0-9A-F]{6}", color) for color in (primary, structure, skin, accent)):
            raise PortraitProjectionError("The portrait material roles contain an invalid color.")
        return (
            "Repaint this exact 2x2 orthographic turntable sheet as a material-ID reference for a four-filament 3D print. "
            "Preserve every panel, camera, silhouette, geometry, proportions, face landmarks, expression, crossed-arm order, "
            "clothing edge, base, scale and panel placement pixel-for-pixel as closely as possible. Change color only; do not "
            "redraw, beautify, reshape, smooth, move, crop, add, remove or relight the model. Use flat solid semantic materials "
            "with hard clean boundaries and no gradients, shading, texture, highlights, reflections, speckles, color fringes or "
            f"cast shadows. All blazer, jacket, lapels, sleeves, cuffs, pockets and rear outer clothing are {primary}. Only the "
            f"actual face, ears, neck, hands and genuinely exposed wrist skin are {skin}. In the back panel, the narrow exposed nape "
            f"immediately below the dark hairline remains {skin}; only the outward collar below it is {primary}. Hair, watch and the entire pedestal are "
            f"{structure}. The inner blouse only is {accent}. Treat the realistic face like a finely sculpted portrait: the face, "
            f"ears, nose, cheeks, lips and mouth interior stay in the same continuous {skin} material. Preserve only thin, connected, "
            f"source-faithful eyebrows, pupils and upper eyelids in {structure}; never make black eye sockets, eye whites, eyeliner wings, "
            f"a black mouth cavity or dark cheek seams. Preserve the visible teeth as one compact {primary} band bounded strictly inside "
            f"the original smile; never enlarge it into a white mouth block or place white on lips, cheeks or eyes. Never use {accent} on "
            f"any face, lips, teeth, skin, hand or neck. Never put skin on clothing, especially shoulder, "
            "upper arm, elbow, forearm, cuff, jacket back, collar or lapel. Never put accent or dark shading inside outer clothing. "
            "Keep the background one uniform very light neutral color, distinct from the subject, with no floor or shadow. Return "
            "exactly the same 2x2 layout and nothing else."
        )

    def prepare_portrait_geometry_material_views(self,
        natural_turntable: Path,
        output_directory: Path,
        palette_roles: Mapping[str, str],
    ) -> tuple[dict[str, Path], dict[str, Any]]:
        views_directory = natural_turntable / "model-views"
        masks_directory = natural_turntable / "model-masks"
        source_views = {
            view: views_directory / f"{view}.png"
            for view in MULTIVIEW_ORDER
        }
        if any(not path.is_file() for path in source_views.values()) or any(
            not (masks_directory / f"{view}.png").is_file() for view in MULTIVIEW_ORDER
        ):
            raise PortraitProjectionError("The exact portrait turntable or its masks are incomplete.")
        output_directory.mkdir(parents=True, exist_ok=True)
        source_sheet = build_multiview_input_sheet(
            source_views, output_directory / "natural-turntable-sheet.png"
        )
        semantic_sheet = output_directory / "image2-material-sheet.png"
        self.ports.edit_image(
            source_sheet,
            self.portrait_geometry_material_prompt(palette_roles),
            semantic_sheet,
        )
        crops = split_multiview_sheet(semantic_sheet, output_directory / "image2-crops")
        prepared: dict[str, Path] = {}
        view_reports: dict[str, Any] = {}
        for view in MULTIVIEW_ORDER:
            view_directory = output_directory / "views" / view
            view_reports[view] = quantize_geometry_aligned_material_reference(
                crops[view],
                masks_directory / f"{view}.png",
                view_directory,
                palette_roles,
                view_name=view,
            )
            prepared[view] = view_directory
        report = {
            "status": "prepared",
            "version": "image2-semantic-material-v1",
            "source_sheet": str(source_sheet.name),
            "semantic_sheet": str(semantic_sheet.name),
            "views": view_reports,
        }
        _write_mesh_repair_report(output_directory / "semantic-material-report.json", report)
        return prepared, report

    def create_portrait_multiview_sheet(self, job: Job, sheet: Path) -> None:
        """Create one isolated sheet with exactly one potentially billed edit."""
        source = _geometry_generation_reference(job) or job.model_reference_path
        if source is None:
            raise PortraitMultiviewPreparationError(
                "The identity-locked portrait front view is unavailable."
            )
        self.ports.edit_image(
            source,
            self.portrait_multiview_prompt(),
            sheet,
            background="transparent",
        )

    def multiview_paths_from_metrics(self, job: Job, key: str) -> dict[str, Path] | None:
        value = job.image_metrics.get("multiview_reference", {})
        stored = value.get(key) if isinstance(value, dict) else None
        if not isinstance(stored, dict) or set(stored) != set(MULTIVIEW_ORDER):
            return None
        result: dict[str, Path] = {}
        root = job.directory.resolve()
        for view in MULTIVIEW_ORDER:
            relative = stored.get(view)
            if not isinstance(relative, str) or not relative:
                return None
            try:
                candidate = (job.directory / relative).resolve()
                candidate.relative_to(root)
            except (OSError, ValueError):
                return None
            if not candidate.is_file():
                return None
            result[view] = candidate
        return result

    def multiview_sheet_fingerprint(self, sheet: Path) -> str:
        try:
            stat_result = sheet.stat()
        except OSError:
            return ""
        return f"{stat_result.st_size}:{stat_result.st_mtime_ns}"

    def mark_multiview_candidate_rejected(self, job: Job, sheet: Path, reason: str) -> None:
        with self.ports.lock:
            job.image_metrics["multiview_candidate_rejected"] = {
                "fingerprint": self.multiview_sheet_fingerprint(sheet),
                "reason": reason,
                "material_gate_version": PORTRAIT_MATERIAL_GATE_VERSION,
            }
            self.ports.persist(job)

    def can_reuse_multiview_candidate(self, job: Job, sheet: Path) -> bool:
        if not sheet.is_file() or job.model_reference_path is None:
            return False
        source = _geometry_generation_reference(job) or job.model_reference_path
        try:
            if sheet.stat().st_mtime_ns < source.stat().st_mtime_ns:
                return False
        except OSError:
            return False
        rejected = job.image_metrics.get("multiview_candidate_rejected")
        if isinstance(rejected, Mapping) and rejected.get("fingerprint") == self.multiview_sheet_fingerprint(sheet):
            rejected_reason = str(rejected.get("reason", ""))
            if rejected_reason.startswith("portrait_material_gate:"):
                # Re-evaluate an older candidate exactly once after material-gate
                # rules improve.  A candidate rejected by this version is still
                # regenerated on the next user retry.
                return rejected.get("material_gate_version") != PORTRAIT_MATERIAL_GATE_VERSION
            if rejected_reason != "identity_consistency_gate":
                return False
            stored_review = job.image_metrics.get("multiview_candidate_review")
            # Old jobs did not persist the review details. Recheck that exact sheet
            # once under the newer acceptance rules, then persist the result.
            if not isinstance(stored_review, Mapping):
                return True
            return evaluate_multiview_review_acceptance(stored_review).get("status") == "pass"
        return True

    def ensure_portrait_multiview(self, job: Job) -> dict[str, Path] | None:
        if not self.quality_portrait_multiview_enabled(job):
            return None
        cached = self.multiview_paths_from_metrics(job, "generation_views")
        stored_reference = job.image_metrics.get("multiview_reference", {})
        stored_normalization = (
            stored_reference.get("normalization", {})
            if isinstance(stored_reference, Mapping)
            else {}
        )
        cache_has_quality_resolution = (
            isinstance(stored_normalization, Mapping)
            and stored_normalization.get("version") == MULTIVIEW_NORMALIZATION_VERSION
            and stored_normalization.get("canvas_size") == list(HIGH_QUALITY_PORTRAIT_CANVAS_SIZE)
        )
        if cached is not None and cache_has_quality_resolution:
            return cached
        assert job.model_reference_path is not None
        locked_front = _geometry_generation_reference(job) or job.model_reference_path
        output = job.directory / "multiview"
        output.mkdir(parents=True, exist_ok=True)
        sheet = output / "multiview-sheet.png"
        self.ports.stop_boundary(job)
        with self.ports.lock:
            job.phase = "preparing_multiview"
            job.message = "Preparing identity-preserving portrait views."
            job.progress = 12
            self.ports.persist(job)
        try:
            reuse_candidate = self.can_reuse_multiview_candidate(job, sheet)
            if not reuse_candidate:
                # A neutral painted background is not sufficient for light-clothed
                # portraits: its border gradient can overlap the jacket colour and
                # make deterministic subject extraction punch holes through the
                # torso. Prefer real alpha and fall back to a palette-aware chroma
                # key when an OpenAI-compatible proxy lacks transparent edits.
                self.create_portrait_multiview_sheet(job, sheet)
                with self.ports.lock:
                    job.image_metrics.pop("multiview_candidate_rejected", None)
                    self.ports.persist(job)
            _validate_image_file(sheet, minimum_edge=512, require_visual_detail=True)
            crops = split_multiview_sheet(sheet, output / "crops")
            references, generation_references, metrics = process_multiview_crops(
                crops,
                output / "views",
                job.palette,
                job.print_settings,
                palette_roles=job.palette_roles,
            )
            normalization = normalize_multiview_inputs(
                references,
                generation_references,
                locked_front_material=job.preview_path,
                # Lock only the central facial identity pixels. Normalization keeps
                # the reviewed turntable front's complete silhouette and fills any
                # white-garment holes from it, so the source face cannot introduce
                # the checkerboard gaps that previously disabled identity locking.
                locked_front_generation=locked_front,
                target_canvas_size=HIGH_QUALITY_PORTRAIT_CANVAS_SIZE,
            )
            review_sheet = build_multiview_input_sheet(
                generation_references, output / "multiview-input-sheet.png"
            )
            rejected_views: list[str] = []
            material_review_views: list[str] = []
            for view in MULTIVIEW_ORDER:
                gate = evaluate_portrait_material_gate(
                    metrics[view], job.palette_roles, view_name=view,
                )
                metrics[view]["multiview_material_gate"] = gate
                if gate.get("status") != "pass":
                    if gate.get("status") == "review":
                        material_review_views.append(view)
                    else:
                        rejected_views.append(view)
            if rejected_views:
                self.mark_multiview_candidate_rejected(
                    job, sheet, "portrait_material_gate:" + ",".join(rejected_views)
                )
                raise PortraitMultiviewPreparationError(
                    "One or more portrait views still contain unsafe skin, garment, or detached material regions."
                )
            for view in MULTIVIEW_ORDER:
                quality = assess_model_input_image(generation_references[view])
                if not bool(quality.get("model_input_eligible", False)):
                    self.mark_multiview_candidate_rejected(job, sheet, f"model_input_gate:{view}")
                    raise PortraitMultiviewPreparationError(
                        f"The {view} portrait view is not suitable for 3D generation."
                    )
            self.ports.stop_boundary(job)
            with self.ports.lock:
                job.message = "Checking portrait identity across four views."
                job.progress = 17
                self.ports.persist(job)
            stored_review = job.image_metrics.get("multiview_candidate_review")
            if (
                reuse_candidate
                and isinstance(stored_review, Mapping)
                and evaluate_multiview_review_acceptance(stored_review).get("status") == "pass"
            ):
                review = dict(stored_review)
            else:
                review_description = (
                    "The exact approved real-person portrait bust, with unchanged identity, pose, clothing, "
                    "accessories and base."
                )
                if material_review_views:
                    review_description += (
                        " Give extra scrutiny to skin-versus-garment ownership and detached colour fragments in "
                        + ", ".join(material_review_views)
                        + " profile view(s), whose frontal face cleanup was intentionally deferred."
                    )
                review = review_multiview_sheet(
                    review_sheet,
                    review_description,
                    source_path=locked_front,
                    completion=self.ports.complete_vision,
                )
            review["material_review_views"] = material_review_views
            review_acceptance = evaluate_multiview_review_acceptance(review)
            review["acceptance"] = review_acceptance
            with self.ports.lock:
                job.image_metrics["multiview_candidate_review"] = review
                self.ports.persist(job)
            if review_acceptance.get("status") != "pass":
                self.mark_multiview_candidate_rejected(job, sheet, "identity_consistency_gate")
                raise PortraitMultiviewPreparationError(
                    "The generated portrait views did not preserve identity consistently."
                )
            manifest = write_multiview_manifest(
                output,
                sheet=review_sheet,
                references=references,
                generation_references=generation_references,
                metrics=metrics,
                review=review,
                palette=job.palette,
                settings=PrintSettings.from_mapping(job.print_settings),
            )
        except (
            OpenAIPreprocessorError,
            PrintableImageError,
            ModelInputImageQualityError,
            MultiviewReferenceError,
            ValueError,
        ) as exc:
            self.mark_multiview_candidate_rejected(job, sheet, "view_preparation_error")
            raise PortraitMultiviewPreparationError(
                f"The high-quality portrait views could not be prepared: {exc}"
            ) from None

        def relative_paths(paths: Mapping[str, Path]) -> dict[str, str]:
            return {view: paths[view].resolve().relative_to(job.directory.resolve()).as_posix() for view in MULTIVIEW_ORDER}

        with self.ports.lock:
            job.image_metrics.pop("multiview_candidate_rejected", None)
            job.image_metrics.pop("multiview_candidate_review", None)
            job.image_metrics.pop("multiview_retry", None)
            job.image_metrics["multiview_reference"] = {
                "status": "pass",
                "score": int(review.get("score", 0)),
                "sheet": review_sheet.resolve().relative_to(job.directory.resolve()).as_posix(),
                "provider_sheet": sheet.resolve().relative_to(job.directory.resolve()).as_posix(),
                "normalization": normalization,
                "manifest": manifest.resolve().relative_to(job.directory.resolve()).as_posix(),
                "generation_views": relative_paths(generation_references),
                "material_views": relative_paths(references),
            }
            self.ports.persist(job)
        return dict(generation_references)
