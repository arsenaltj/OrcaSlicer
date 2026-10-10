"""Creation color policy and timing facts; no execution or transport."""
from __future__ import annotations

import time
from design_generation_timing import DesignTimingHistory, seconds as timing_seconds
from model_contracts import Job

def _use_unrestricted_creation(job: Job) -> None:
    """Apply the current policy only when starting a new creation stage.

    Completed history is never rewritten and restoring an already submitted
    provider task never creates another task. Old pending palette previews can
    continue with their unquantized design image.
    """
    job.palette = ()
    job.palette_roles = {}
    job.palette_recommendation = {}
    job.palette_recommendation_confirmed = False
    job.image_metrics["creation_color_policy"] = "unrestricted-v1"
    if job.raw_preview_path is not None and job.raw_preview_path.is_file():
        job.image_metrics["design_reference"] = "ai-design-v1"

def _public_design_timing(job: Job) -> dict[str, float]:
    timing = job.image_metrics.get("design_timing", {})
    if not isinstance(timing, dict) or job.design_started_monotonic is None:
        return {}  # Legacy/recovered jobs must not invent a start time.
    duration = timing_seconds(timing.get("duration_seconds"))
    elapsed = duration or max(0.0, time.monotonic() - job.design_started_monotonic)
    return {"elapsed_seconds": round(elapsed, 2),
            "estimated_seconds": timing_seconds(timing.get("estimated_seconds"))}

def _complete_design_timing(job: Job) -> None:
    timing = job.image_metrics.get("design_timing", {})
    if (job.design_started_monotonic is None or not isinstance(timing, dict)
            or "duration_seconds" in timing or job.stop_event.is_set()):
        return
    duration = max(0.0, time.monotonic() - job.design_started_monotonic)
    timing["duration_seconds"] = duration
    timing["finished_at"] = time.time()
    DesignTimingHistory(job.directory.parent).record(str(timing.get("group", "")), duration)
