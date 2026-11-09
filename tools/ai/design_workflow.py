"""DesignWorkflow: application orchestration over explicit ports, independent of HTTP and UI."""
from __future__ import annotations

import json
import hashlib
import os
import shutil
import time
import traceback
from dataclasses import asdict, dataclass
from design_generation_timing import DesignTimingHistory, timing_key
from model_contracts import Job, JobStopped, RequestError, MAX_PROMPT_BYTES, MIN_MODEL_REFERENCE_EDGE
from model_request import _normalize_palette_recommendation
from tripo_client import TripoError
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
    recommend_colors: Callable[..., Any] | None = None
    portrait_vision_once: Callable[..., str] | None = None


class DesignWorkflow:
    def __init__(self, ports: DesignWorkflowPorts):
        self.ports = ports

    @staticmethod
    def portrait_instruction(job: Job, instruction: str) -> str:
        if not job.portrait_print:
            return instruction
        return instruction + (
            "\nONE-CLICK PORTRAIT FRAMING: create exactly one single-person half-body bust. "
            "Keep the complete head, shoulders, upper torso and source-visible arms/hands; "
            "finish the lower torso cleanly, never add pelvis, legs, feet, scenery or a display base. "
            "Orca adds a geometric base after 3D generation. This framing overrides full-body framing."
        )

    def portrait_call(self, job: Job, stage: str, callback: Callable[[], Any]) -> Any:
        """One initial request plus one explicitly failed retry; never replay unknown submissions."""
        if not job.portrait_print:
            return callback()
        while True:
            self.ports.stop_boundary(job)
            ledger = job.image_metrics.setdefault("portrait_attempts", {})
            number = int(ledger.get(stage, 0)) + 1
            ledger[stage] = number
            self.ports.persist(job, required=True)
            raw = job.directory / "style-preview-raw.png"
            if stage == "design" and number > 1 and raw.is_file():
                evidence = job.directory / "portrait-evidence"
                evidence.mkdir(exist_ok=True)
                shutil.copyfile(raw, evidence / f"design-before-{number}.png")
            try:
                result = callback()
                if job.image_metrics.get("portrait_failure", {}).get("stage") == stage:
                    job.image_metrics.pop("portrait_failure", None)
                return result
            except OpenAIPreprocessorError as error:
                job.image_metrics["portrait_failure"] = {
                    "stage": stage, "code": error.code, "message": str(error), "ambiguous": error.ambiguous,
                }
                explicit = not error.ambiguous and (
                    error.code in {"provider_http_error", "provider_unavailable", "provider_failed", "upstream_error"}
                    or any(marker in str(error).lower() for marker in ("http 502", "http 503", "http 504", "upstream_grpc_error"))
                )
                self.ports.persist(job, required=True)
                if not (explicit and number == 1 and job.portrait_retry_authorized):
                    raise
                raw = job.directory / "style-preview-raw.png"
                if raw.is_file():
                    evidence = job.directory / "portrait-evidence"
                    evidence.mkdir(exist_ok=True)
                    shutil.copyfile(raw, evidence / f"{stage}-{number}.png")

    def check_portrait_design(self, job: Job, image: Path) -> None:
        if not job.portrait_print:
            return
        report = {"status": "unavailable", "reason": "半身人像核验暂不可用。"}
        try:
            if self.ports.portrait_vision_once is None:
                raise ValueError("vision channel unavailable")
            reply = self.portrait_call(job, "portrait_review", lambda: self.ports.portrait_vision_once(
                "Inspect framing only. Return a JSON object with booleans single_person, half_body, "
                "head_visible and a short Chinese reason. A sculpted bust or upper-body portrait is half_body. "
                "The clean lower torso crop is intentional. Ignore likeness, aesthetics, palette, texture, "
                "minor clipping and printability. Do not require any pedestal or base.",
                "Check whether this generated image is one usable single-person half-body portrait.", (image,)))
            value = json.loads(reply.strip().removeprefix("```json").removeprefix("```").removesuffix("```").strip())
            if not isinstance(value, dict) or any(type(value.get(key)) is not bool
                for key in ("single_person", "half_body", "head_visible")):
                raise ValueError("invalid framing response")
            passed = all(value[key] for key in ("single_person", "half_body", "head_visible"))
            report = {"status": "pass" if passed else "not_portrait", "reason": str(value.get("reason", ""))[:1000]}
        except (OpenAIPreprocessorError, ValueError, OSError) as error:
            report["reason"] = "半身人像核验暂不可用：" + str(error)[:800]
        report["sha256"] = hashlib.sha256(image.read_bytes()).hexdigest()
        job.image_metrics["portrait_design"] = report
        # Heuristic/image-aesthetic checks are advice in this explicit route.
        for key in ("model_input_quality", "generation_input_quality"):
            quality = job.image_metrics.setdefault(key, {})
            quality["warnings"] = list(dict.fromkeys(quality.get("warnings", []) + quality.get("blockers", [])))
            quality["blockers"] = []
            quality["model_input_eligible"] = report["status"] == "pass"

    def recommend_portrait_palette(self, job: Job) -> None:
        try:
            self.ports.stop_boundary(job)
            recommendation = self.portrait_call(job, "palette", lambda: self.ports.recommend_colors(
                job.user_prompt, job.style, job.custom_style,
                image_path=job.input_path, color_count=job.palette_color_count))
            count = len(recommendation.colors)
            if count not in {4, 5, 6}:
                raise ValueError("The portrait recommendation must contain 4, 5 or 6 colors.")
            normalized = _normalize_palette_recommendation(
                {"summary": recommendation.summary, "colors": [asdict(color) for color in recommendation.colors]}, count)
            self.ports.stop_boundary(job)
            with self.ports.lock:
                # Retain the actual validated count; never pad or merge colors.
                job.palette_color_count = count
                job.palette_recommendation = normalized
                job.state = job.phase = "awaiting_palette_confirmation"
                job.message = "The validated portrait palette is ready for confirmation."
                job.progress = 10
                self.ports.persist(job, required=True)
        except JobStopped:
            self.ports.mark_stopped(job)
        except OpenAIPreprocessorError as exc:
            self.ports.fail_preprocess(job, exc)
        except (ValueError, RequestError, TripoError):
            self.ports.fail(job, "Portrait palette recommendation did not pass validation; no image or model was generated.")
        finally:
            self.ports.finish_deleted(job)


    def preprocess_text(self, job: Job, prompt: str) -> None:
        _use_unrestricted_creation(job)
        prompt = self.portrait_instruction(job, prompt)
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
            self.portrait_call(job, "design", lambda: self.ports.generate_image(
                prompt, raw_preview, job.style, job.custom_style,
                palette=job.palette, palette_roles=job.palette_roles,
                job=job, provider=job.design_provider, model=job.design_model,
            ))
            self.ports.validate_image(
                raw_preview,
                minimum_edge=MIN_MODEL_REFERENCE_EDGE,
                require_visual_detail=True,
            )
            job.raw_preview_path = raw_preview
            if job.palette and not job.portrait_print:
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
            self.check_portrait_design(job, raw_preview)
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
            if job.portrait_print or job.workflow_mode == "smart" or not self.ports.fallback_enabled():
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
        instruction = self.portrait_instruction(job, instruction)
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
            self.portrait_call(job, "design", lambda: self.ports.edit_image(
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
                job=job, provider=job.design_provider, model=job.design_model,
            ))
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
            if job.palette and not job.portrait_print:
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
            elif not job.portrait_print:
                self.ports.assess_preview(job, input_path)
            self.check_portrait_design(job, raw_preview)
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
        try:
            status = self.ports.image_provider_status(job.design_provider)
        except TypeError:
            status = self.ports.image_provider_status()
        model = job.design_model or os.environ.get("OPENAI_IMAGE_MODEL", "gpt-image-2")
        key = timing_key(status, model,
                         os.environ.get("OPENAI_IMAGE_QUALITY", "high").strip().lower(), job.source, job.style)
        job.design_started_monotonic = time.monotonic()
        job.image_metrics["design_timing"] = {
            "started_at": time.time(), "group": key,
            "estimated_seconds": DesignTimingHistory(job.directory.parent).estimate(key),
        }
        self.ports.persist(job)
