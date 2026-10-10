"""DesignWorkflow: application orchestration over explicit ports, independent of HTTP and UI."""
from __future__ import annotations

import json
import os
import shutil
import time
import traceback
from dataclasses import dataclass
from design_generation_timing import DesignTimingHistory, timing_key
from model_contracts import Job, JobStopped, MAX_PROMPT_BYTES, MIN_MODEL_REFERENCE_EDGE
from model_creation_policy import _complete_design_timing, _use_unrestricted_creation
from model_job_support import (
    generation_prompt as _generation_prompt,
    printable_preview_message as _printable_preview_message,
)
from openai_preprocessor import (
    OpenAIPreprocessorError,
    PORTRAIT_FACE_LOCK_FILENAME,
    image_preprocessing_policy,
)
from pathlib import Path
from typing import Any, Callable, ContextManager

@dataclass(frozen=True)
class DesignWorkflowPorts:
    """Only the composition root chooses executors, storage and paid providers."""

    lock: ContextManager[Any]
    persist: Callable[..., None]
    stop_boundary: Callable[[Job], None]
    fail: Callable[[Job, str], None]
    fail_preprocess: Callable[[Job, Any], None]
    mark_stopped: Callable[[Job], None]
    finish_deleted: Callable[[Job], None]
    apply_printable_image: Callable[..., dict[str, Any]]
    assess_reference: Callable[[Job], None]
    assess_preview: Callable[..., None]
    generate_image: Callable[..., None]
    edit_image: Callable[..., None]
    vision_once: Callable[..., str]
    image_provider_status: Callable[[], dict[str, Any]]
    fallback_enabled: Callable[[], bool]
    validate_image: Callable[..., Any]
    review_nonportrait: Callable[..., dict[str, Any]]


class DesignWorkflow:
    def __init__(self, ports: DesignWorkflowPorts):
        self.ports = ports


    def preprocess_text(self, job: Job, prompt: str) -> None:
        _use_unrestricted_creation(job)
        self.begin_timing(job)
        try:
            self.ports.stop_boundary(job)
            prepared = _generation_prompt(
                prompt,
                job.palette,
                max_prompt_bytes=MAX_PROMPT_BYTES,
                constrain_palette=False,
            )
            if not prepared or len(prepared.encode("utf-8")) > MAX_PROMPT_BYTES:
                raise OpenAIPreprocessorError("The prepared prompt is empty or exceeds the 2000-byte limit.")
            raw_preview = job.directory / "style-preview-raw.png"
            self.ports.generate_image(
                prompt, raw_preview, job.style, job.custom_style,
                palette=job.palette, palette_roles=job.palette_roles,
            )
            self.ports.validate_image(
                raw_preview,
                minimum_edge=MIN_MODEL_REFERENCE_EDGE,
                require_visual_detail=True,
            )
            job.raw_preview_path = raw_preview
            if job.palette:
                color_usage = self.ports.apply_printable_image(job, raw_preview)
            else:
                preview = job.directory / "preview.png"
                shutil.copyfile(raw_preview, preview)
                job.preview_path = preview
                color_usage = {}
            job.image_metrics["design_reference"] = "ai-design-v1"
            job.image_metrics["portrait_geometry"] = {
                "detected": False,
                "evidence": "not_applicable",
            }
            validated = self.ports.validate_image(
                raw_preview,
                minimum_edge=MIN_MODEL_REFERENCE_EDGE,
                require_visual_detail=True,
            )
            self.ports.assess_reference(job)
            (job.directory / "preview-colors.json").write_text(
                json.dumps(
                    {
                        "style": job.style,
                        "palette_constrained": bool(job.palette),
                        "palette_pixels": color_usage,
                        "palette_roles": job.palette_roles,
                        "print": job.print_settings,
                        "metrics": job.image_metrics,
                    },
                    ensure_ascii=False,
                    indent=2,
                ),
                encoding="utf-8",
            )
            job.preview_content_type = validated.content_type
            with self.ports.lock:
                if job.stop_event.is_set():
                    raise JobStopped()
                job.prepared_prompt = prepared
                job.preprocess_failure = {}
                job.state = "awaiting_confirmation"
                job.phase = "awaiting_confirmation"
                job.message = _printable_preview_message(job, "Review the prepared image before generation.")
                job.progress = 15
                _complete_design_timing(job)
                self.ports.persist(job)
        except JobStopped:
            self.ports.mark_stopped(job)
        except ValueError as exc:
            self.ports.fail(job, str(exc))
        except OpenAIPreprocessorError as exc:
            if not self.ports.fallback_enabled():
                self.ports.fail_preprocess(job, exc)
            else:
                with self.ports.lock:
                    job.prepared_prompt = _generation_prompt(
                        prompt, job.palette, max_prompt_bytes=MAX_PROMPT_BYTES
                    )
                    job.preprocess_failure = {}
                    job.state = "awaiting_confirmation"
                    job.phase = "awaiting_confirmation"
                    job.message = "Preprocessing is unavailable; review the original prompt before generation."
                    job.progress = 15
        except Exception:
            self.ports.fail(job, "Text preprocessing failed.")
        finally:
            self.ports.finish_deleted(job)

    def preprocess_image(self, job: Job, input_path: Path, instruction: str) -> None:
        _use_unrestricted_creation(job)
        self.begin_timing(job)
        raw_preview = job.directory / "style-preview-raw.png"
        geometry_reference = job.directory / "geometry-reference.png"
        preview = job.directory / "preview.png"
        try:
            self.ports.stop_boundary(job)
            options = {"filename": str(job.image_metrics.get("source_filename", input_path.name))}
            policy = image_preprocessing_policy(instruction, job.style, custom_style=job.custom_style, print_settings=job.print_settings,
                                                 preprocessing_options=options)
            job.image_metrics["image_preprocessing_policy"] = policy
            with self.ports.lock:
                job.phase = "image_generation"
                job.message = "The image service is preparing the model reference; this usually takes one to three minutes."
                job.progress = 11
                self.ports.persist(job)
            self.ports.edit_image(
                input_path,
                instruction,
                raw_preview,
                job.palette,
                job.style,
                str(job.print_settings.get("shadow_color", "blue")),
                job.palette_roles,
                job.custom_style,
                geometry_reference,
                print_settings=job.print_settings,
                preprocessing_options=options,
            )
            # Test adapters and older compatible preprocessors may not implement
            # the optional sculptural snapshot yet. Preserve the previous behavior
            # instead of failing the whole image journey.
            if not geometry_reference.is_file():
                shutil.copyfile(raw_preview, geometry_reference)
            with self.ports.lock:
                job.phase = "checking_image"
                job.message = "The generated subject is being checked for structure, framing, and usable detail."
                job.progress = 12
                self.ports.persist(job)
            self.ports.validate_image(
                raw_preview,
                minimum_edge=MIN_MODEL_REFERENCE_EDGE,
                require_visual_detail=True,
            )
            job.raw_preview_path = raw_preview
            job.geometry_reference_path = geometry_reference
            if job.palette:
                with self.ports.lock:
                    job.phase = "printability_check"
                    job.message = "Skin, clothing, accent, and structure colors are being separated into printable regions."
                    job.progress = 13
                    self.ports.persist(job)
                color_usage = self.ports.apply_printable_image(job, raw_preview)
                preview = job.preview_path or preview
            else:
                shutil.copyfile(raw_preview, preview)
                job.preview_path = preview
                color_usage = {}
            face_lock = job.directory / PORTRAIT_FACE_LOCK_FILENAME
            job.image_metrics["design_reference"] = "ai-design-v1"
            legacy_cleanup = job.image_metrics.get("portrait_skin_cleanup", {})
            portrait_detected = face_lock.is_file() or (
                isinstance(legacy_cleanup, dict) and legacy_cleanup.get("activated") == 1
            )
            job.image_metrics["portrait_geometry"] = {
                "detected": portrait_detected,
                "evidence": "source_face_lock" if face_lock.is_file() else "legacy_material_cleanup" if portrait_detected else "none",
            }
            self.ports.assess_reference(job)
            preview = job.preview_path or preview
            validated = self.ports.validate_image(
                raw_preview,
                minimum_edge=MIN_MODEL_REFERENCE_EDGE,
                require_visual_detail=True,
            )
            if policy["specialized"]:
                self.ports.stop_boundary(job)
                semantic_review = os.environ.get("ORCASLICER_AI_NONPORTRAIT_REVIEW", "").strip() == "1"
                try:
                    job.image_metrics["nonportrait_reference_quality"] = self.ports.review_nonportrait(
                        input_path, raw_preview, policy, job.directory / "nonportrait-review",
                        completion=self.ports.vision_once if semantic_review else None,
                        reviewer_model=os.environ.get("OPENAI_TEXT_MODEL", "gpt-5.4") if semantic_review else "",
                    )
                except (OSError, ValueError):
                    job.image_metrics["nonportrait_reference_quality"] = {
                        "status": "unavailable", "warnings": ["reference_quality_unavailable"],
                        "physical_print_qualified": False,
                    }
            else:
                self.ports.assess_preview(job, input_path)
            (job.directory / "preview-colors.json").write_text(
                json.dumps(
                    {
                        "style": job.style,
                        "palette_constrained": bool(job.palette),
                        "palette_pixels": color_usage,
                        "palette_roles": job.palette_roles,
                        "print": job.print_settings,
                        "metrics": job.image_metrics,
                    },
                    ensure_ascii=False,
                    indent=2,
                ),
                encoding="utf-8",
            )
            self.ports.stop_boundary(job)
            with self.ports.lock:
                if job.stop_event.is_set():
                    raise JobStopped()
                job.preview_path = preview
                job.preview_content_type = validated.content_type
                job.preprocess_failure = {}
                job.state = "awaiting_confirmation"
                job.phase = "awaiting_confirmation"
                reference_review = job.image_metrics.get("nonportrait_reference_quality", {})
                notice = (
                    "Compare parts, openings, viewpoint and subject lettering with the original before generation."
                    if any(str(code).startswith("reference_") and str(code).endswith("_changed")
                           for code in reference_review.get("warnings", [])) else
                    "Review the prepared image before generation."
                )
                job.message = _printable_preview_message(job, notice)
                job.progress = 15
                _complete_design_timing(job)
                self.ports.persist(job)
        except JobStopped:
            self.ports.mark_stopped(job)
        except ValueError as exc:
            self.ports.fail(job, str(exc))
        except OpenAIPreprocessorError as exc:
            self.ports.fail_preprocess(job, exc)
        except Exception:
            traceback.print_exc()
            self.ports.fail_preprocess(
                job,
                OpenAIPreprocessorError(
                    "The image was generated, but its local validation failed.",
                    code="local_image_processing_failed",
                    retryable=True,
                ),
            )
        finally:
            self.ports.finish_deleted(job)

    def begin_timing(self, job: Job) -> None:
        if job.design_started_monotonic is not None:
            return
        key = timing_key(self.ports.image_provider_status(), os.environ.get("OPENAI_IMAGE_MODEL", "gpt-image-2"),
                         os.environ.get("OPENAI_IMAGE_QUALITY", "high").strip().lower(), job.source, job.style)
        job.design_started_monotonic = time.monotonic()
        job.image_metrics["design_timing"] = {
            "started_at": time.time(), "group": key,
            "estimated_seconds": DesignTimingHistory(job.directory.parent).estimate(key),
        }
        self.ports.persist(job)
