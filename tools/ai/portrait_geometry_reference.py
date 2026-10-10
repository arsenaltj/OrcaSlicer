"""Portrait reference geometry and alpha preparation; no image generation."""
from __future__ import annotations

import math
import os
import threading
import uuid
from collections import deque
from model_contracts import (
    PORTRAIT_GEOMETRY_MAX_SUBJECT_OCCUPANCY,
    PORTRAIT_GEOMETRY_PROVIDER_FILENAME,
    PORTRAIT_HEAD_GEOMETRY_MAX_SUBJECT_OCCUPANCY,
    PORTRAIT_HEAD_PREVIEW_FILENAME,
)
from model_input_image_quality import ModelInputImageQualityError
from openai_preprocessor import IDENTITY_FIRST_PORTRAIT_STYLES
from pathlib import Path
from printable_image_pipeline import _portrait_source_subject_mask
from typing import Any, Mapping


_GEOMETRY_REFERENCE_LOCK = threading.RLock()

def _model_generation_reference(job: Job) -> Path | None:
    """Submit the approved AI design, retaining legacy reference routing for old jobs."""
    if job.image_metrics.get("design_reference") == "ai-design-v1":
        return job.raw_preview_path
    if job.model_reference_path is not None:
        return job.model_reference_path
    if job.palette and job.raw_preview_path is not None:
        return job.raw_preview_path
    return job.preview_path

def _identity_preserving_portrait_geometry_enabled(job: Job) -> bool:
    cleanup = job.image_metrics.get("portrait_skin_cleanup", {})
    portrait_geometry = job.image_metrics.get("portrait_geometry", {})
    portrait_detected = (
        isinstance(portrait_geometry, dict)
        and portrait_geometry.get("detected") is True
    ) or (
        isinstance(cleanup, dict)
        and cleanup.get("activated") == 1
    )
    return (
        job.source == "image"
        and job.style in IDENTITY_FIRST_PORTRAIT_STYLES
        and job.generation_profile == "quality"
        and portrait_detected
        and job.input_path is not None
        and job.input_path.is_file()
    )

def _geometry_generation_reference(job: Job) -> Path | None:
    """New designs retain the approved composition; old jobs keep their evidence."""

    if job.image_metrics.get("design_reference") == "ai-design-v1":
        return job.raw_preview_path

    if _identity_preserving_portrait_geometry_enabled(job):
        # The chooser is used concurrently by paid submission, public status,
        # preview download and restart recovery. Serialize the small atomic
        # image rewrite so those routes cannot replace one another's temporary
        # files while a request is being confirmed.
        with _GEOMETRY_REFERENCE_LOCK:
            # Repair here—not only during initial preprocessing—so every route
            # receives the same validated provider image.
            _synchronize_geometry_reference_alpha(job)
            # Preserve the historical geometry contract for existing jobs.
            if job.geometry_reference_path is not None and job.geometry_reference_path.is_file():
                return _prepare_portrait_geometry_provider_reference(job)
        return _model_generation_reference(job) or job.input_path
    return _model_generation_reference(job)

def _refine_portrait_head_silhouette(
    geometry: Any,
    alpha: Any,
    source_alpha: Any | None = None,
) -> tuple[Any, dict[str, Any]]:
    """Remove attached light backdrop halos around a portrait head.

    Image generators sometimes draw a checkerboard or soft white oval instead
    of emitting true transparency.  Generic connected-component cleanup cannot
    remove it because the oval touches the hair and shoulders.  In the upper
    portrait region, however, real hair/skin supplies a strong non-neutral
    span on every row.  Trim only light-neutral pixels outside that span, stop
    when the silhouette widens into the shoulders, and leave the white jacket
    and the rest of the bust completely untouched.
    """

    subject_bbox = alpha.getbbox()
    report: dict[str, Any] = {
        "version": "portrait-silhouette-v6",
        "status": "not_needed",
        "removed_pixels": 0,
        "removed_detached_pixels": 0,
        "source_head_backdrop_removed_pixels": 0,
        "source_head_backdrop_component_count": 0,
        "light_backdrop_removed_pixels": 0,
        "light_backdrop_component_count": 0,
        "source_neck_removed_pixels": 0,
    }
    if subject_bbox is None:
        return alpha, report
    left, top, right, bottom = subject_bbox
    subject_width = right - left
    subject_height = bottom - top
    if subject_width < 32 or subject_height < 64:
        return alpha, report

    alpha_pixels = alpha.load()
    geometry_pixels = geometry.load()
    row_bounds: dict[int, tuple[int, int, int]] = {}
    for y in range(top, bottom):
        row = [x for x in range(left, right) if alpha_pixels[x, y] >= 128]
        if row:
            row_bounds[y] = (row[0], row[-1], len(row))
    subject_area = max(1, sum(item[2] for item in row_bounds.values()))

    shoulder_threshold = max(24, int(round(subject_width * 0.55)))
    search_start = top + int(round(subject_height * 0.22))
    search_end = min(bottom, top + int(round(subject_height * 0.58)))
    shoulder_y: int | None = None
    for y in range(search_start, search_end):
        current = row_bounds.get(y)
        if current is None or current[2] < shoulder_threshold:
            continue
        preceding = [
            row_bounds[row][2]
            for row in range(max(top, y - 36), max(top, y - 6))
            if row in row_bounds
        ]
        following = [
            row_bounds[row][2]
            for row in range(y, min(bottom, y + 8))
            if row in row_bounds
        ]
        if (
            preceding
            and following
            and min(following) >= shoulder_threshold
            and current[2] >= min(preceding) * 1.18
        ):
            shoulder_y = y
            break
    if shoulder_y is None:
        report["reason"] = "shoulder_transition_not_detected"
        return alpha, report

    refined = alpha.copy()
    refined_pixels = refined.load()
    margin = max(1, int(round(subject_width * 0.002)))
    removed_pixels = 0
    cleaned_rows = 0
    source_mask_used = False
    source_removed_pixels = 0
    source_head_backdrop_removed_pixels = 0
    source_head_backdrop_component_count = 0
    light_backdrop_removed_pixels = 0
    light_backdrop_component_count = 0
    light_backdrop_end = 0
    source_neck_removed_pixels = 0
    source_neck_margin = 0
    source_neck_end = 0
    source_torso_removed_pixels = 0
    source_torso_margin = 0
    source_torso_end = 0
    source_head_end = max(top, shoulder_y - max(8, round(subject_height * 0.04)))
    if source_alpha is not None and getattr(source_alpha, "size", None) == alpha.size:
        source_pixels = source_alpha.load()
        source_count = 0
        overlap = 0
        for y in range(top, source_head_end):
            for x in range(left, right):
                if source_pixels[x, y] >= 128:
                    source_count += 1
                    overlap += alpha_pixels[x, y] >= 128
        if source_count >= 32 and overlap >= source_count * 0.70:
            from PIL import ImageFilter

            source_mask_used = True
            for y in range(top, source_head_end):
                for x in range(left, right):
                    if refined_pixels[x, y] >= 128 and source_pixels[x, y] < 128:
                        refined_pixels[x, y] = 0
                        source_removed_pixels += 1
            removed_pixels += source_removed_pixels

            # The aligned source mask is intentionally expanded by a few pixels
            # so it cannot shave real hair. That safety rim can also retain a
            # narrow opaque checkerboard crescent beside dark hair. Detect only
            # large light-neutral components that touch the refined head edge
            # and live mostly outside an eroded source core. Real white or gray
            # hair extends well into that core and is therefore preserved.
            source_core_kernel = max(
                3,
                min(9, (round(min(alpha.width, alpha.height) * 0.008) | 1)),
            )
            source_core = source_alpha.filter(ImageFilter.MinFilter(source_core_kernel))
            source_core_pixels = source_core.load()
            head_candidates = bytearray(alpha.width * alpha.height)
            for y in range(top, source_head_end):
                row = y * alpha.width
                for x in range(left, right):
                    if refined_pixels[x, y] < 128:
                        continue
                    red, green, blue = geometry_pixels[x, y][:3]
                    if min(red, green, blue) >= 200 and max(red, green, blue) - min(red, green, blue) <= 32:
                        head_candidates[row + x] = 1

            visited_head = bytearray(len(head_candidates))
            removable_head_components: list[list[int]] = []
            minimum_head_component = max(48, round(subject_area * 0.0001))
            for seed, is_candidate in enumerate(head_candidates):
                if not is_candidate or visited_head[seed]:
                    continue
                visited_head[seed] = 1
                pending: deque[int] = deque([seed])
                component: list[int] = []
                outside_core_count = 0
                touches_alpha_edge = False
                while pending:
                    offset = pending.popleft()
                    component.append(offset)
                    x = offset % alpha.width
                    y = offset // alpha.width
                    outside_core_count += source_core_pixels[x, y] < 128
                    for neighbor in (
                        offset - 1 if x else -1,
                        offset + 1 if x + 1 < alpha.width else -1,
                        offset - alpha.width if y else -1,
                        offset + alpha.width if y + 1 < alpha.height else -1,
                    ):
                        if neighbor < 0:
                            touches_alpha_edge = True
                            continue
                        neighbor_x = neighbor % alpha.width
                        neighbor_y = neighbor // alpha.width
                        if refined_pixels[neighbor_x, neighbor_y] < 128:
                            touches_alpha_edge = True
                        elif head_candidates[neighbor] and not visited_head[neighbor]:
                            visited_head[neighbor] = 1
                            pending.append(neighbor)
                area = len(component)
                if (
                    area >= minimum_head_component
                    and outside_core_count >= round(area * 0.55)
                    and touches_alpha_edge
                ):
                    removable_head_components.append(component)
            head_removal = sum(len(component) for component in removable_head_components)
            if head_removal <= round(subject_area * 0.005):
                for component in removable_head_components:
                    for offset in component:
                        refined_pixels[offset % alpha.width, offset // alpha.width] = 0
                source_head_backdrop_removed_pixels = head_removal
                source_head_backdrop_component_count = len(removable_head_components)
                removed_pixels += head_removal

            # The source-locked head removes the large hair halo, but white
            # checkerboard patches can remain attached to shoulders and crossed
            # arms. Tripo turns those neutral patches into small connected
            # plates. Real sculptural skin and garments in this reference are
            # warm gray; use large, boundary-connected neutral-white components
            # as seeds and require that each component visibly escapes the
            # source silhouette. This keeps a white jacket and the cropped
            # bust while removing the backdrop before a paid submission.
            source_bbox = source_alpha.getbbox()
            if source_bbox is not None:
                light_backdrop_end = min(
                    bottom,
                    round(alpha.height * 0.76),
                    source_bbox[3] + max(12, round(subject_height * 0.05)),
                )
                candidates = bytearray(alpha.width * alpha.height)
                seeds = bytearray(alpha.width * alpha.height)
                for y in range(source_head_end, light_backdrop_end):
                    row = y * alpha.width
                    for x in range(left, right):
                        if refined_pixels[x, y] < 128:
                            continue
                        red, green, blue = geometry_pixels[x, y][:3]
                        minimum = min(red, green, blue)
                        spread = max(red, green, blue) - minimum
                        offset = row + x
                        if minimum >= 225 and spread <= 10:
                            candidates[offset] = 1
                        if minimum >= 235 and spread <= 6:
                            seeds[offset] = 1

                visited = bytearray(len(candidates))
                removable_components: list[list[int]] = []
                minimum_component_area = max(128, round(subject_area * 0.0005))
                for seed, is_candidate in enumerate(candidates):
                    if not is_candidate or visited[seed]:
                        continue
                    visited[seed] = 1
                    pending: deque[int] = deque([seed])
                    component: list[int] = []
                    seed_count = 0
                    outside_source_count = 0
                    touches_alpha_edge = False
                    while pending:
                        offset = pending.popleft()
                        component.append(offset)
                        x = offset % alpha.width
                        y = offset // alpha.width
                        seed_count += bool(seeds[offset])
                        outside_source_count += source_pixels[x, y] < 128
                        for neighbor in (
                            offset - 1 if x else -1,
                            offset + 1 if x + 1 < alpha.width else -1,
                            offset - alpha.width if y else -1,
                            offset + alpha.width if y + 1 < alpha.height else -1,
                        ):
                            if neighbor < 0:
                                touches_alpha_edge = True
                                continue
                            neighbor_x = neighbor % alpha.width
                            neighbor_y = neighbor // alpha.width
                            if alpha_pixels[neighbor_x, neighbor_y] < 128:
                                touches_alpha_edge = True
                            elif candidates[neighbor] and not visited[neighbor]:
                                visited[neighbor] = 1
                                pending.append(neighbor)
                    area = len(component)
                    if (
                        area >= minimum_component_area
                        and seed_count >= max(32, round(area * 0.15))
                        and outside_source_count >= max(24, round(area * 0.03))
                        and touches_alpha_edge
                    ):
                        removable_components.append(component)

                candidate_removal = sum(len(component) for component in removable_components)
                if candidate_removal <= round(subject_area * 0.06):
                    for component in removable_components:
                        for offset in component:
                            refined_pixels[offset % alpha.width, offset // alpha.width] = 0
                    light_backdrop_removed_pixels = candidate_removal
                    light_backdrop_component_count = len(removable_components)
                    removed_pixels += candidate_removal

                # Neutral-component cleanup deliberately ignores warm-gray
                # pixels, but a studio-shadow fringe at the neck can share that
                # warmer tone and remain attached to the generated shoulders.
                # On a single-view image-to-3D request even a small triangle in
                # this narrow transition becomes a vertical plate behind the
                # collar.  The edit is composition-locked to the user's photo,
                # so constrain only this short neck-to-shoulder band to the
                # aligned source silhouette.  A modest row-wise margin avoids
                # clipping hair or lapels; the operation stops before the main
                # jacket and crossed arms, and a hard area cap rejects a badly
                # aligned source mask.
                source_neck_margin = max(4, min(12, round(subject_width * 0.015)))
                source_neck_end = min(
                    light_backdrop_end,
                    shoulder_y + max(16, round(subject_height * 0.024)),
                )
                neck_offsets: list[int] = []
                for y in range(source_head_end, source_neck_end):
                    source_row = [
                        x for x in range(left, right) if source_pixels[x, y] >= 128
                    ]
                    if not source_row:
                        continue
                    keep_left = max(left, source_row[0] - source_neck_margin)
                    keep_right = min(right - 1, source_row[-1] + source_neck_margin)
                    row = y * alpha.width
                    for x in range(left, keep_left):
                        if refined_pixels[x, y] >= 128:
                            neck_offsets.append(row + x)
                    for x in range(keep_right + 1, right):
                        if refined_pixels[x, y] >= 128:
                            neck_offsets.append(row + x)
                if len(neck_offsets) <= round(subject_area * 0.01):
                    for offset in neck_offsets:
                        refined_pixels[offset % alpha.width, offset // alpha.width] = 0
                    source_neck_removed_pixels = len(neck_offsets)
                    removed_pixels += source_neck_removed_pixels

                # A neutral plate can carry warm-gray shadows along the neck,
                # so the color seed intentionally leaves a small triangular
                # remainder.  In only the shared shoulder/upper-torso band,
                # apply a generously dilated source silhouette as a second
                # guard. The margin scales with the portrait and the operation
                # stops well before the generated bust finish.
                source_torso_margin = max(8, min(24, round(subject_width * 0.03)))
                source_torso_end = min(
                    light_backdrop_end,
                    max(source_head_end, source_bbox[3] - source_torso_margin * 2),
                )
                source_torso_alpha = source_alpha.filter(
                    ImageFilter.MaxFilter(source_torso_margin * 2 + 1)
                )
                source_torso_pixels = source_torso_alpha.load()
                torso_offsets: list[int] = []
                for y in range(source_head_end, source_torso_end):
                    row = y * alpha.width
                    for x in range(left, right):
                        if refined_pixels[x, y] >= 128 and source_torso_pixels[x, y] < 128:
                            torso_offsets.append(row + x)
                if len(torso_offsets) <= round(subject_area * 0.015):
                    for offset in torso_offsets:
                        refined_pixels[offset % alpha.width, offset // alpha.width] = 0
                    source_torso_removed_pixels = len(torso_offsets)
                    removed_pixels += source_torso_removed_pixels

    # The aligned source silhouette is more reliable than color thresholds and
    # preserves white/gray hair. Use the row-wise neutral-halo fallback only
    # when a trustworthy source mask could not be recovered.
    for y in (() if source_mask_used else range(top, shoulder_y)):
        bounds = row_bounds.get(y)
        if bounds is None:
            continue
        row_left, row_right, row_count = bounds
        content: list[int] = []
        for x in range(row_left, row_right + 1):
            if alpha_pixels[x, y] < 128:
                continue
            red, green, blue = geometry_pixels[x, y][:3]
            # The endpoint often paints a soft studio shadow around an opaque
            # checkerboard.  Its inner edge can be medium gray (roughly 170),
            # not merely near-white.  Treat the smooth neutral ramp as empty
            # until real hair/skin chroma or darker sculptural detail begins.
            # Cleanup is still limited to rows above the detected shoulders,
            # so a cream or white jacket is never trimmed by this threshold.
            light_neutral_background = (
                min(red, green, blue) >= 170
                and max(red, green, blue) - min(red, green, blue) <= 32
            )
            if not light_neutral_background:
                content.append(x)
        if len(content) < max(8, int(round(row_count * 0.25))):
            continue
        keep_left = max(row_left, content[0] - margin)
        keep_right = min(row_right, content[-1] + margin)
        if keep_left <= row_left and keep_right >= row_right:
            continue
        for x in range(row_left, keep_left):
            if refined_pixels[x, y] >= 128:
                refined_pixels[x, y] = 0
                removed_pixels += 1
        for x in range(keep_right + 1, row_right + 1):
            if refined_pixels[x, y] >= 128:
                refined_pixels[x, y] = 0
                removed_pixels += 1
        cleaned_rows += 1

    # A single opaque checkerboard dash is enough for image-to-3D to create a
    # thin spike beside the bust. Portrait geometry is expected to be one
    # connected bust silhouette, so retain only the largest 4-connected
    # alpha component after the halo trim. Use two bounded flood-fill passes to
    # avoid retaining a large per-component pixel list for megapixel images.
    foreground = bytes(value >= 128 for value in refined.tobytes())
    visited = bytearray(len(foreground))
    largest_seed = -1
    largest_area = 0
    for seed, is_foreground in enumerate(foreground):
        if not is_foreground or visited[seed]:
            continue
        visited[seed] = 1
        pending: deque[int] = deque([seed])
        area = 0
        while pending:
            offset = pending.popleft()
            area += 1
            x = offset % refined.width
            for neighbor in (
                offset - 1 if x else -1,
                offset + 1 if x + 1 < refined.width else -1,
                offset - refined.width if offset >= refined.width else -1,
                offset + refined.width if offset + refined.width < len(foreground) else -1,
            ):
                if neighbor >= 0 and foreground[neighbor] and not visited[neighbor]:
                    visited[neighbor] = 1
                    pending.append(neighbor)
        if area > largest_area:
            largest_seed = seed
            largest_area = area

    detached_pixels = 0
    if largest_seed >= 0 and largest_area:
        keep = bytearray(len(foreground))
        keep[largest_seed] = 1
        pending = deque([largest_seed])
        while pending:
            offset = pending.popleft()
            x = offset % refined.width
            for neighbor in (
                offset - 1 if x else -1,
                offset + 1 if x + 1 < refined.width else -1,
                offset - refined.width if offset >= refined.width else -1,
                offset + refined.width if offset + refined.width < len(foreground) else -1,
            ):
                if neighbor >= 0 and foreground[neighbor] and not keep[neighbor]:
                    keep[neighbor] = 1
                    pending.append(neighbor)
        for offset, is_foreground in enumerate(foreground):
            if is_foreground and not keep[offset]:
                refined_pixels[offset % refined.width, offset // refined.width] = 0
                detached_pixels += 1
        removed_pixels += detached_pixels

    report.update({
        "status": "refined" if removed_pixels else "not_needed",
        "shoulder_y": shoulder_y,
        "cleaned_rows": cleaned_rows,
        "margin_px": margin,
        "removed_pixels": removed_pixels,
        "removed_detached_pixels": detached_pixels,
        "source_mask_used": source_mask_used,
        "source_removed_pixels": source_removed_pixels,
        "source_head_backdrop_removed_pixels": source_head_backdrop_removed_pixels,
        "source_head_backdrop_component_count": source_head_backdrop_component_count,
        "source_head_end": source_head_end,
        "light_backdrop_removed_pixels": light_backdrop_removed_pixels,
        "light_backdrop_component_count": light_backdrop_component_count,
        "light_backdrop_end": light_backdrop_end,
        "source_neck_removed_pixels": source_neck_removed_pixels,
        "source_neck_margin": source_neck_margin,
        "source_neck_end": source_neck_end,
        "source_torso_removed_pixels": source_torso_removed_pixels,
        "source_torso_margin": source_torso_margin,
        "source_torso_end": source_torso_end,
        "removed_subject_ratio": round(
            removed_pixels / subject_area, 6
        ),
    })
    return refined, report

def _synchronize_geometry_reference_alpha(job: Job) -> None:
    """Give the sculptural RGB reference the pipeline's hard subject silhouette.

    Some OpenAI-compatible image endpoints render a checkerboard into an opaque
    RGB result even when transparent output was requested.  The printable-image
    pipeline already recovers a clean, connected binary subject mask.  Prefer
    that mask over ``model_reference.png`` alpha: the latter can intentionally
    retain a soft portrait shadow which single-view image-to-3D may extrude into
    a person-shaped rear plate.  Reusing only the hard silhouette preserves the
    provider's sculptural face planes while preventing checkerboards, shadows or
    tiny detached fragments from reaching image-to-3D.

    This runs during assessment as well as initial preprocessing so restored
    Beta jobs created by an older sidecar are repaired before a paid task can be
    submitted.
    """

    geometry_reference = job.geometry_reference_path
    model_reference = job.model_reference_path
    subject_mask = job.subject_mask_path
    if (
        not _identity_preserving_portrait_geometry_enabled(job)
        or geometry_reference is None
        or model_reference is None
        or not geometry_reference.is_file()
        or not model_reference.is_file()
    ):
        return
    previous_cleanup = job.image_metrics.get("geometry_silhouette_cleanup", {})
    if (
        isinstance(previous_cleanup, Mapping)
        and previous_cleanup.get("version") == "portrait-silhouette-v6"
        and previous_cleanup.get("alpha_synced") is True
    ):
        cached_cleanup = dict(previous_cleanup)
        cached_cleanup["revalidated"] = True
        job.image_metrics["geometry_silhouette_cleanup"] = cached_cleanup
        return
    try:
        from PIL import Image, ImageChops, UnidentifiedImageError
    except ImportError:
        raise ModelInputImageQualityError(
            "Pillow is required to prepare the portrait geometry reference."
        ) from None

    temporary = geometry_reference.with_name(
        f"{geometry_reference.name}.{uuid.uuid4().hex}.alpha.tmp"
    )
    try:
        mask_path = (
            subject_mask
            if subject_mask is not None and subject_mask.is_file()
            else model_reference
        )
        with Image.open(geometry_reference) as geometry_opened, Image.open(mask_path) as mask_opened:
            if geometry_opened.size != mask_opened.size:
                raise ModelInputImageQualityError(
                    "The portrait geometry reference and validated silhouette have different sizes."
                )
            validated_alpha = (
                mask_opened.convert("L")
                if mask_path == subject_mask
                else mask_opened.getchannel("A")
                if "A" in mask_opened.getbands()
                else None
            )
            if validated_alpha is None:
                return
            # A hard edge is intentional here. Semi-transparent antialiasing or
            # a portrait drop shadow is useful for 2D preview, but is ambiguous
            # geometry evidence and can become a printable vertical sheet.
            geometry = geometry_opened.convert("RGBA")
            validated_alpha = validated_alpha.point(lambda value: 255 if value >= 128 else 0)
            # Never re-open pixels removed by an earlier cleanup. Restored jobs
            # can be assessed repeatedly (startup, UI download, paid submit),
            # and replacing alpha with the original broad mask would otherwise
            # turn the sanitized transparent-black halo back into opaque black
            # geometry on the next pass.
            current_alpha = geometry.getchannel("A").point(
                lambda value: 255 if value >= 128 else 0
            )
            validated_alpha = ImageChops.darker(validated_alpha, current_alpha)
            already_refined = (
                isinstance(previous_cleanup, Mapping)
                and previous_cleanup.get("version") == "portrait-silhouette-v6"
                and previous_cleanup.get("status") in {"refined", "not_needed"}
            )
            if already_refined:
                silhouette_report = dict(previous_cleanup)
                silhouette_report["revalidated"] = True
            else:
                source_alpha = None
                if job.input_path is not None and job.input_path.is_file():
                    source_mask_data = _portrait_source_subject_mask(
                        geometry.width,
                        geometry.height,
                        job.input_path,
                    )
                    if source_mask_data:
                        source_alpha = Image.frombytes("L", geometry.size, source_mask_data)
                validated_alpha, silhouette_report = _refine_portrait_head_silhouette(
                    geometry,
                    validated_alpha,
                    source_alpha,
                )
            geometry.putalpha(validated_alpha)

            # Alpha alone is not sufficient for provider interoperability.
            # Some upload/conversion paths flatten PNGs after discarding alpha,
            # exposing the checkerboard or white RGB values that were hidden in
            # transparent pixels. Image-to-3D can then extrude that hidden image
            # into a large rear plate. Keep the complete subject unchanged, but
            # make every non-subject pixel transparent black so both alpha-aware
            # and RGB-only decoders see an unambiguous empty background.
            sanitized = Image.new("RGBA", geometry.size, (0, 0, 0, 0))
            sanitized.paste(geometry, (0, 0), validated_alpha)
            if geometry_opened.convert("RGBA").tobytes() == sanitized.tobytes():
                silhouette_report["alpha_synced"] = True
                job.image_metrics["geometry_silhouette_cleanup"] = silhouette_report
                return
            sanitized.save(temporary, format="PNG")
        os.replace(temporary, geometry_reference)
        silhouette_report["alpha_synced"] = True
        job.image_metrics["geometry_silhouette_cleanup"] = silhouette_report
    except ModelInputImageQualityError:
        raise
    except (OSError, UnidentifiedImageError, Image.DecompressionBombError):
        raise ModelInputImageQualityError(
            "The sculptural portrait reference could not inherit the validated subject silhouette."
        ) from None
    finally:
        try:
            temporary.unlink(missing_ok=True)
        except OSError:
            pass

def _copy_portrait_into_continuous_silhouette(
    portrait: Any,
    current_alpha: Any,
    target_alpha: Any,
) -> Any:
    """Keep valid portrait pixels exact and synthesize only repaired mask gaps.

    A paid image-to-3D request must never expose transparent holes or the RGB
    checkerboard hidden behind them.  Pixels already belonging to the validated
    subject are copied byte-for-byte (apart from hardening alpha).  A newly
    filled gap inherits the nearest valid pixel on the same scanline, which is
    deliberately less imaginative than inpainting and cannot alter the face.
    """

    from bisect import bisect_left
    from PIL import Image

    if portrait.size != current_alpha.size or portrait.size != target_alpha.size:
        raise ModelInputImageQualityError(
            "The portrait silhouette repair images have different sizes."
        )
    source = portrait.convert("RGBA")
    current = current_alpha.point(lambda value: 255 if value >= 128 else 0)
    target = target_alpha.point(lambda value: 255 if value >= 128 else 0)
    source_pixels = source.load()
    current_pixels = current.load()
    target_pixels = target.load()
    repaired = Image.new("RGBA", source.size, (0, 0, 0, 0))
    repaired_pixels = repaired.load()
    interior_offset = max(2, min(6, round(source.width * 0.008)))
    for y in range(source.height):
        valid = [
            x
            for x in range(source.width)
            if current_pixels[x, y] >= 128 and target_pixels[x, y] >= 128
        ]
        if not valid:
            continue
        for x in range(source.width):
            if target_pixels[x, y] < 128:
                continue
            if current_pixels[x, y] >= 128:
                red, green, blue, _ = source_pixels[x, y]
            else:
                insertion = bisect_left(valid, x)
                if insertion == 0:
                    sample_index = min(len(valid) - 1, interior_offset)
                elif insertion == len(valid):
                    sample_index = max(0, len(valid) - 1 - interior_offset)
                else:
                    left_index = insertion - 1
                    right_index = insertion
                    if x - valid[left_index] <= valid[right_index] - x:
                        sample_index = max(0, left_index - interior_offset)
                    else:
                        sample_index = min(
                            len(valid) - 1, right_index + interior_offset
                        )
                red, green, blue, _ = source_pixels[valid[sample_index], y]
            repaired_pixels[x, y] = (red, green, blue, 255)
    return repaired

def _repair_portrait_head_shoulders_silhouette(
    portrait: Any,
    portrait_alpha: Any,
    source_alpha: Any | None,
    *,
    protected_bounds: tuple[int, int, int, int] | None = None,
) -> tuple[Any, Any, dict[str, Any]]:
    """Remove exterior checkerboard remnants and close shoulder/neck notches.

    The compact portrait is intentionally a single frontal bust silhouette.
    This lets us use three independent pieces of evidence before a paid task:
    the generated hard alpha, the composition-locked source-photo silhouette,
    and real sculptural texture (as opposed to near-white checkerboard RGB).
    Each accepted scanline is made continuous only between its trusted left and
    right edges.  Thus exterior ghosts disappear, interior square cutouts close,
    and every already-valid identity RGB pixel remains untouched.
    """

    try:
        from PIL import Image, ImageChops, ImageFilter
    except ImportError:
        raise ModelInputImageQualityError(
            "Pillow is required to repair the portrait shoulder silhouette."
        ) from None

    report: dict[str, Any] = {
        "version": "portrait-head-shoulders-silhouette-v1",
        "status": "unverified",
        "source_mask_used": False,
        "removed_external_pixels": 0,
        "removed_light_edge_pixels": 0,
        "filled_notch_pixels": 0,
        "remaining_row_gap_pixels": 0,
        "protected_pixels_removed": 0,
        "protected_removed_ratio": 0.0,
        "central_identity_pixels_removed": 0,
        "identity_rgb_pixels_changed": 0,
    }
    current = portrait_alpha.point(lambda value: 255 if value >= 128 else 0)
    current_area = sum(value >= 128 for value in current.tobytes())
    if current_area <= 0:
        report["reason"] = "empty_portrait_silhouette"
        return portrait, current, report
    if source_alpha is None or source_alpha.size != portrait.size:
        report["reason"] = "source_portrait_silhouette_unavailable"
        return portrait, current, report

    source = source_alpha.point(lambda value: 255 if value >= 128 else 0)
    # Allow a few pixels of generator/source alignment drift while still using
    # the source to reject large attached remnants. MaxFilter requires an odd
    # kernel and is bounded so it cannot turn the source into a broad halo.
    alignment_margin = max(3, min(11, round(min(portrait.size) * 0.008) | 1))
    source = source.filter(ImageFilter.MaxFilter(alignment_margin))
    bounded = ImageChops.darker(current, source)
    bounded_pixels = bounded.load()
    portrait_pixels = portrait.convert("RGBA").load()
    target = Image.new("L", portrait.size, 0)
    target_pixels = target.load()
    trusted_rows = 0
    fallback_rows = 0
    removed_light_edge_pixels = 0
    edge_margin = max(1, round(min(portrait.size) * 0.003))
    proposed_bounds: dict[int, tuple[int, int]] = {}
    for y in range(portrait.height):
        candidate = [x for x in range(portrait.width) if bounded_pixels[x, y] >= 128]
        if not candidate:
            continue
        textured: list[int] = []
        for x in candidate:
            red, green, blue, _ = portrait_pixels[x, y]
            # Generated checkerboards and their white matte are nearly neutral
            # and much brighter than even the light jacket's shaded edge.
            light_neutral_matte = (
                min(red, green, blue) >= 242
                and max(red, green, blue) - min(red, green, blue) <= 12
            )
            if not light_neutral_matte:
                textured.append(x)
        if textured:
            left = max(candidate[0], textured[0] - edge_margin)
            right = min(candidate[-1], textured[-1] + edge_margin)
            removed_light_edge_pixels += sum(
                x < left or x > right for x in candidate
            )
            trusted_rows += 1
        else:
            # Keep a very bright hair/highlight scanline when both independent
            # masks agree; adjacent textured rows still constrain its extent.
            left, right = candidate[0], candidate[-1]
            fallback_rows += 1
        proposed_bounds[y] = (left, right)

    # Below the narrowest neck row, a frontal bust must widen smoothly toward
    # the shoulders. Collar/background fragments can otherwise create a sudden
    # square inward step even when each scanline is individually continuous.
    neck_search_top = round(portrait.height * 0.38)
    neck_search_bottom = round(portrait.height * 0.62)
    neck_candidates = [
        y for y in proposed_bounds if neck_search_top <= y < neck_search_bottom
    ]
    neck_row = (
        min(
            neck_candidates,
            key=lambda y: proposed_bounds[y][1] - proposed_bounds[y][0],
        )
        if neck_candidates
        else None
    )
    maximum_expansion = max(2, round(portrait.width * 0.006))
    boundary_adjustment_pixels = 0
    if neck_row is not None:
        previous_left, previous_right = proposed_bounds[neck_row]
        for y in range(neck_row + 1, portrait.height):
            if y not in proposed_bounds:
                continue
            raw_left, raw_right = proposed_bounds[y]
            left = max(
                min(raw_left, previous_left),
                previous_left - maximum_expansion,
            )
            right = min(
                max(raw_right, previous_right),
                previous_right + maximum_expansion,
            )
            boundary_adjustment_pixels += abs(left - raw_left) + abs(right - raw_right)
            proposed_bounds[y] = (left, right)
            previous_left, previous_right = left, right
    for y, (left, right) in proposed_bounds.items():
        for x in range(left, right + 1):
            target_pixels[x, y] = 255

    target_area = sum(value >= 128 for value in target.tobytes())
    current_pixels = current.load()
    removed_external = 0
    filled_notches = 0
    remaining_gaps = 0
    for y in range(portrait.height):
        row = [x for x in range(portrait.width) if target_pixels[x, y] >= 128]
        if row:
            remaining_gaps += sum(
                target_pixels[x, y] < 128 for x in range(row[0], row[-1] + 1)
            )
        for x in range(portrait.width):
            was_subject = current_pixels[x, y] >= 128
            is_subject = target_pixels[x, y] >= 128
            removed_external += int(was_subject and not is_subject)
            filled_notches += int(is_subject and not was_subject)

    protected_count = 0
    protected_removed = 0
    central_identity_removed = 0
    central_removed_left = portrait.width
    central_removed_top = portrait.height
    central_removed_right = -1
    central_removed_bottom = -1
    if protected_bounds is not None:
        left, top, right, bottom = protected_bounds
        left = max(0, min(portrait.width, left))
        right = max(left, min(portrait.width, right))
        top = max(0, min(portrait.height, top))
        bottom = max(top, min(portrait.height, bottom))
        protected_width = right - left
        protected_height = bottom - top
        central_left = left + round(protected_width * 0.12)
        central_right = right - round(protected_width * 0.12)
        central_top = top + round(protected_height * 0.08)
        # Protect the eyes/nose/mouth interior absolutely. The lower part of the
        # detector's broad warm-skin box includes neck and collar pixels, where
        # source-aligned trimming is precisely what removes the square tabs.
        central_bottom = top + round(protected_height * 0.58)
        for y in range(top, bottom):
            for x in range(left, right):
                if current_pixels[x, y] >= 128:
                    protected_count += 1
                    protected_removed += int(target_pixels[x, y] < 128)
                    if (
                        central_left <= x < central_right
                        and central_top <= y < central_bottom
                        and target_pixels[x, y] < 128
                    ):
                        central_identity_removed += 1
                        central_removed_left = min(central_removed_left, x)
                        central_removed_top = min(central_removed_top, y)
                        central_removed_right = max(central_removed_right, x)
                        central_removed_bottom = max(central_removed_bottom, y)

    area_ratio = target_area / max(1, current_area)
    protected_removed_ratio = protected_removed / max(1, protected_count)
    verified = (
        trusted_rows >= max(16, round(portrait.height * 0.45))
        and fallback_rows <= round(portrait.height * 0.20)
        and 0.72 <= area_ratio <= 1.10
        and removed_external <= current_area * 0.22
        and filled_notches <= current_area * 0.12
        and protected_removed_ratio <= 0.04
        and central_identity_removed == 0
        and remaining_gaps == 0
    )
    report.update({
        "status": "pass" if verified else "unverified",
        "reason": "continuous_source_aligned_silhouette" if verified else "unsafe_repair_extent",
        "source_mask_used": True,
        "source_alignment_margin_px": alignment_margin,
        "trusted_rows": trusted_rows,
        "fallback_rows": fallback_rows,
        "neck_row": neck_row,
        "maximum_shoulder_expansion_px": maximum_expansion,
        "shoulder_boundary_adjustment_pixels": boundary_adjustment_pixels,
        "current_area": current_area,
        "target_area": target_area,
        "target_to_current_ratio": round(area_ratio, 6),
        "removed_external_pixels": removed_external,
        "removed_light_edge_pixels": removed_light_edge_pixels,
        "filled_notch_pixels": filled_notches,
        "remaining_row_gap_pixels": remaining_gaps,
        "protected_pixels_removed": protected_removed,
        "protected_removed_ratio": round(protected_removed_ratio, 6),
        "central_identity_pixels_removed": central_identity_removed,
        "central_identity_removed_bounds": (
            [
                central_removed_left,
                central_removed_top,
                central_removed_right + 1,
                central_removed_bottom + 1,
            ]
            if central_identity_removed
            else None
        ),
    })
    if not verified:
        return portrait, current, report
    repaired = _copy_portrait_into_continuous_silhouette(
        portrait, current, target
    )
    return repaired, target, report

def _prepare_portrait_geometry_provider_reference(job: Job) -> Path:
    """Build an identity-preserving sculptural portrait on a safe square canvas.

    The portrait Image2 endpoint returns a 2:3 image.  In real Tripo runs that
    tall transparent canvas repeatedly produced a thin loop behind the head and
    a full-height rear plate, even after every hidden transparent RGB pixel was
    cleared. A subsequent 1.89M-face real run proved that a colour-photo-like
    half-body input can still collapse a faithful 2D face into a generic 3D one.
    Geometry therefore follows the relief-rich sculptural reference; the natural
    colour reference remains the authority for post-generation materials.

    When reliable face and base bounds are available, crop at the shoulders and
    preserve every remaining portrait pixel at native resolution. The provider
    input deliberately contains no generated display base: a base can be added
    later in Orca's prepare workflow from the user's selected presets. Center
    the result on transparent black so no portrait frame can be interpreted as
    geometry.

    The original ``geometry-reference.png`` remains untouched after alpha
    sanitization.  Keeping the provider-specific derivative separate makes the
    operation idempotent and lets restored jobs rebuild it safely.
    """

    source = job.geometry_reference_path
    if source is None or not source.is_file():
        raise ModelInputImageQualityError("The sculptural portrait reference is unavailable.")
    try:
        from PIL import Image, UnidentifiedImageError
    except ImportError:
        raise ModelInputImageQualityError(
            "Pillow is required to prepare the portrait geometry provider image."
        ) from None

    destination = job.directory / PORTRAIT_GEOMETRY_PROVIDER_FILENAME
    temporary = destination.with_name(
        f"{destination.name}.{uuid.uuid4().hex}.tmp"
    )
    previous_canvas = job.image_metrics.get("geometry_provider_canvas", {})
    # A provider image is part of a paid request's immutable evidence.  Status
    # polling and restart recovery call this chooser again, so a newer layout
    # strategy must never rewrite the image that an existing Tripo task saw.
    if job.attempts and destination.is_file() and isinstance(previous_canvas, Mapping):
        frozen_size = previous_canvas.get("output_size")
        if (
            isinstance(frozen_size, list)
            and len(frozen_size) == 2
            and all(isinstance(value, int) and value > 0 for value in frozen_size)
        ):
            try:
                with Image.open(destination) as frozen:
                    if frozen.size == tuple(frozen_size) and "A" in frozen.getbands():
                        return destination
            except (OSError, UnidentifiedImageError, Image.DecompressionBombError):
                pass
    try:
        with Image.open(source) as opened:
            sculptural_geometry = opened.convert("RGBA")
        alpha = sculptural_geometry.getchannel("A")
        geometry = sculptural_geometry
        appearance_source = "sculptural_geometry_reference"
        appearance_path = source
        original_size = list(geometry.size)
        compaction: dict[str, Any] = {
            "applied": False,
            "reason": "not_eligible",
            "original_size": original_size,
        }
        cleanup = job.image_metrics.get("portrait_skin_cleanup", {})
        face_bounds = cleanup.get("face_bounds") if isinstance(cleanup, Mapping) else None
        base_bounds = cleanup.get("base_bounds") if isinstance(cleanup, Mapping) else None
        if isinstance(face_bounds, Mapping) and isinstance(base_bounds, Mapping):
            try:
                face_left = int(face_bounds["left"])
                face_right = int(face_bounds["right"])
                face_top = int(face_bounds["top"])
                face_bottom = int(face_bounds["bottom"])
                base_top = int(base_bounds["top"])
            except (KeyError, TypeError, ValueError, OverflowError):
                face_left = face_right = face_top = face_bottom = base_top = -1
            width, height = geometry.size
            face_width = face_right - face_left
            skin_component_height = face_bottom - face_top
            subject_bbox = alpha.getbbox()
            # A connected warm component can extend through the neck. Face
            # width is the more stable scale cue, so cap the estimated head at
            # a conservative real-portrait aspect before choosing a shoulder
            # cut. No source portrait pixel is resampled.
            head_height = min(skin_component_height, round(face_width * 1.55))
            if (
                subject_bbox is not None
                and face_width >= round(width * 0.16)
                and head_height >= round(height * 0.22)
                and 0 <= face_left < face_right <= width
                and 0 <= face_top < face_bottom <= base_top <= height
            ):
                center_x = (face_left + face_right) // 2
                crop_top = max(
                    subject_bbox[1],
                    face_top - round(head_height * 0.14),
                )
                head_bottom = min(face_bottom, face_top + head_height)
                crop_bottom = min(
                    base_top,
                    head_bottom + round(head_height * 0.50),
                )
                crop_width = min(
                    width,
                    max(face_width * 2, round(head_height * 1.66)),
                )
                crop_left = max(0, min(width - crop_width, center_x - crop_width // 2))
                crop_right = crop_left + crop_width
                if crop_bottom <= crop_top + head_height:
                    compaction["reason"] = "portrait_shoulders_not_available"
                else:
                    portrait = geometry.crop((crop_left, crop_top, crop_right, crop_bottom))
                    portrait_source_alpha = portrait.getchannel("A").point(
                        lambda value: 255 if value >= 128 else 0
                    )
                    source_portrait_alpha = None
                    if job.input_path is not None and job.input_path.is_file():
                        source_mask_data = _portrait_source_subject_mask(
                            width, height, job.input_path
                        )
                        if source_mask_data:
                            source_portrait_alpha = Image.frombytes(
                                "L", (width, height), source_mask_data
                            ).crop((crop_left, crop_top, crop_right, crop_bottom))
                    portrait, portrait_alpha, shoulder_silhouette = (
                        _repair_portrait_head_shoulders_silhouette(
                            portrait,
                            portrait_source_alpha,
                            source_portrait_alpha,
                            protected_bounds=(
                                face_left - crop_left,
                                face_top - crop_top,
                                face_right - crop_left,
                                face_bottom - crop_top,
                            ),
                        )
                    )
                    # Keep the repaired head-and-shoulders silhouette exactly
                    # at its cropped native size. A display base belongs to the
                    # later Orca prepare flow and must not be baked into a paid
                    # image-to-3D request.
                    geometry = portrait.copy()
                    geometry.putalpha(portrait_alpha)
                    alpha = geometry.getchannel("A")
                    compaction = {
                        "applied": True,
                        "reason": "portrait_head_shoulders_identity",
                        "original_size": original_size,
                        "prepared_size": list(geometry.size),
                        "crop_bounds": [crop_left, crop_top, crop_right, crop_bottom],
                        "removed_original_base": True,
                        "head_height": head_height,
                        "head_bounds": [face_left, crop_top, face_right, head_bottom],
                        "base_source": "none",
                        "generated_display_base": False,
                        "shoulder_silhouette": shoulder_silhouette,
                        "face_canvas_ratio_before": round(
                            head_height / max(1, height), 6
                        ),
                        "face_prepared_ratio": round(
                            head_height / max(1, geometry.height), 6
                        ),
                        "identity_pixels_resampled": False,
                    }
            else:
                compaction["reason"] = "portrait_not_safely_head_croppable"
        subject_bbox = alpha.getbbox()
        if subject_bbox is None:
            raise ModelInputImageQualityError(
                "The sculptural portrait reference has no visible subject."
            )
        subject_width = subject_bbox[2] - subject_bbox[0]
        subject_height = subject_bbox[3] - subject_bbox[1]
        width, height = geometry.size
        maximum_occupancy = (
            PORTRAIT_HEAD_GEOMETRY_MAX_SUBJECT_OCCUPANCY
            if compaction["applied"]
            else PORTRAIT_GEOMETRY_MAX_SUBJECT_OCCUPANCY
        )
        target_side = max(
            width,
            height,
            int(math.ceil(subject_width / maximum_occupancy)),
            int(math.ceil(subject_height / maximum_occupancy)),
        )
        offset_x = (target_side - width) // 2
        offset_y = (target_side - height) // 2
        canvas_version = (
            "square-transparent-black-head-shoulders-v10"
            if compaction["applied"]
            else "square-transparent-black-v2"
        )
        if compaction["applied"]:
            compaction["face_provider_ratio"] = round(
                int(compaction["head_height"]) / max(1, target_side), 6
            )
        job.image_metrics["geometry_provider_canvas"] = {
            "version": canvas_version,
            "source_size": original_size,
            "prepared_size": [width, height],
            "output_size": [target_side, target_side],
            "subject_bbox": list(subject_bbox),
            "subject_occupancy": round(
                max(subject_width, subject_height) / max(1, target_side), 6
            ),
            "offset": [offset_x, offset_y],
            "appearance_source": appearance_source,
            "portrait_compaction": compaction,
        }
        if compaction["applied"] and job.preview_path is not None:
            printable_destination = job.directory / PORTRAIT_HEAD_PREVIEW_FILENAME
            preview_source = job.preview_path
            previous_provider_preview = job.image_metrics.get(
                "portrait_provider_preview", {}
            )
            if preview_source.resolve() == printable_destination.resolve():
                # A restored older job points preview_path at the already-cropped
                # derivative. Rebuild v6 from the pipeline's lossless full-size
                # clean preview instead of trying to crop the crop or silently
                # leaving its old shoulder defect in place.
                for candidate in (
                    job.directory / "clean_preview.png",
                    job.strict_preview_path,
                ):
                    if candidate is None or not candidate.is_file():
                        continue
                    try:
                        with Image.open(candidate) as candidate_opened:
                            if candidate_opened.size == tuple(original_size):
                                preview_source = candidate
                                break
                    except (OSError, UnidentifiedImageError, Image.DecompressionBombError):
                        continue
            if preview_source.resolve() != printable_destination.resolve():
                with Image.open(preview_source) as preview_opened:
                    printable = preview_opened.convert("RGBA")
                if printable.size != tuple(original_size):
                    raise ModelInputImageQualityError(
                        "The printable portrait preview does not match the sculptural reference."
                    )
                crop_left, crop_top, crop_right, crop_bottom = (
                    int(value) for value in compaction["crop_bounds"]
                )
                printable_portrait = printable.crop(
                    (crop_left, crop_top, crop_right, crop_bottom)
                )
                # Geometry alpha remains the silhouette authority. The exact
                # printable colors are clipped to that silhouette so the image
                # users approve describes the same head-and-shoulders object.
                printable_portrait = _copy_portrait_into_continuous_silhouette(
                    printable_portrait,
                    portrait_source_alpha,
                    portrait_alpha,
                )
                printable_prepared = Image.new(
                    "RGBA", (width, height), (0, 0, 0, 0)
                )
                printable_prepared.paste(
                    printable_portrait,
                    (0, 0),
                    printable_portrait.getchannel("A"),
                )
                printable_canvas = Image.new(
                    "RGBA", (target_side, target_side), (0, 0, 0, 0)
                )
                printable_canvas.paste(
                    printable_prepared,
                    (offset_x, offset_y),
                    printable_prepared.getchannel("A"),
                )
                printable_temporary = printable_destination.with_name(
                    f"{printable_destination.name}.{uuid.uuid4().hex}.tmp"
                )
                try:
                    printable_canvas.save(printable_temporary, format="PNG")
                    os.replace(printable_temporary, printable_destination)
                finally:
                    printable_temporary.unlink(missing_ok=True)
            elif (
                not printable_destination.is_file()
                or not isinstance(previous_provider_preview, Mapping)
                or previous_provider_preview.get("version")
                != "portrait-head-shoulders-preview-v7"
            ):
                raise ModelInputImageQualityError(
                    "The prepared portrait preview is unavailable."
                )
            job.preview_path = printable_destination
            job.image_metrics["portrait_provider_preview"] = {
                "version": "portrait-head-shoulders-preview-v7",
                "path": printable_destination.name,
                "output_size": [target_side, target_side],
                "matches_geometry_crop": True,
                "single_material_base": False,
                "generated_display_base": False,
                "continuous_silhouette": bool(
                    isinstance(compaction.get("shoulder_silhouette"), Mapping)
                    and compaction["shoulder_silhouette"].get("status") == "pass"
                ),
            }
        if (
            width == target_side
            and height == target_side
            and appearance_source == "sculptural_geometry_reference"
        ):
            destination.unlink(missing_ok=True)
            return source

        # Public status is polled every few seconds during a long remote job.
        # Re-encoding the unchanged 1–2K portrait on every GET adds avoidable
        # CPU, disk churn and file-lock exposure.  A provider derivative newer
        # than its immutable sanitized source and with the expected canvas is
        # already authoritative for this job.
        if destination.is_file():
            try:
                source_revision = max(
                    source.stat().st_mtime_ns,
                    appearance_path.stat().st_mtime_ns,
                )
                cache_matches = (
                    isinstance(previous_canvas, Mapping)
                    and previous_canvas.get("version") == canvas_version
                    and previous_canvas.get("appearance_source") == appearance_source
                )
                if cache_matches and destination.stat().st_mtime_ns >= source_revision:
                    with Image.open(destination) as cached:
                        if cached.size == (target_side, target_side) and "A" in cached.getbands():
                            return destination
            except (OSError, UnidentifiedImageError, Image.DecompressionBombError):
                pass

        canvas = Image.new("RGBA", (target_side, target_side), (0, 0, 0, 0))
        canvas.paste(geometry, (offset_x, offset_y), geometry.getchannel("A"))
        canvas.save(temporary, format="PNG")
        os.replace(temporary, destination)
        return destination
    except ModelInputImageQualityError:
        raise
    except (OSError, UnidentifiedImageError, Image.DecompressionBombError):
        raise ModelInputImageQualityError(
            "The sculptural portrait reference could not be normalized for image-to-3D."
        ) from None
    finally:
        try:
            temporary.unlink(missing_ok=True)
        except OSError:
            pass
