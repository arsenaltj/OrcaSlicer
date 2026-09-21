"""Run an offline, prompt-equivalent MobileSAM/EfficientSAM boundary A/B.

This is an evaluation tool, not a product runtime. It keeps soft masks and
timings so a model can be rejected before an ONNX provider is added to Orca.
"""

from __future__ import annotations

import argparse
import ctypes
import hashlib
import json
import math
import os
import time
from pathlib import Path
from typing import Any

import numpy as np
from PIL import Image, ImageDraw


SCHEMA = "orcaslicer.semantic-boundary-ab/v3"
MOBILE_PREPROCESS = "sam-resize-half-up-rgb8-normalize-zero-pad-v2"
PROMPT_SCHEMA = "orcaslicer.semantic-boundary-prompts/v1"


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def peak_working_set_bytes() -> int | None:
    if os.name == "nt":
        class Counters(ctypes.Structure):
            _fields_ = [
                ("cb", ctypes.c_ulong), ("PageFaultCount", ctypes.c_ulong),
                ("PeakWorkingSetSize", ctypes.c_size_t), ("WorkingSetSize", ctypes.c_size_t),
                ("QuotaPeakPagedPoolUsage", ctypes.c_size_t), ("QuotaPagedPoolUsage", ctypes.c_size_t),
                ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t), ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
                ("PagefileUsage", ctypes.c_size_t), ("PeakPagefileUsage", ctypes.c_size_t),
            ]
        counters = Counters()
        counters.cb = ctypes.sizeof(counters)
        kernel32 = ctypes.windll.kernel32
        psapi = ctypes.windll.psapi
        kernel32.GetCurrentProcess.restype = ctypes.c_void_p
        psapi.GetProcessMemoryInfo.argtypes = [ctypes.c_void_p, ctypes.POINTER(Counters), ctypes.c_ulong]
        psapi.GetProcessMemoryInfo.restype = ctypes.c_int
        process = kernel32.GetCurrentProcess()
        if psapi.GetProcessMemoryInfo(process, ctypes.byref(counters), counters.cb):
            return int(counters.PeakWorkingSetSize)
        return None
    try:
        import resource
        value = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
        return int(value * (1 if os.uname().sysname == "Darwin" else 1024))
    except (ImportError, AttributeError, OSError):
        return None


def _point(value: Any, name: str, width: int, height: int) -> tuple[int, int]:
    if not isinstance(value, list) or len(value) != 2 or any(type(item) is not int for item in value):
        raise ValueError(f"{name} must be [x, y] integers")
    x, y = value
    if x < 0 or y < 0 or x >= width or y >= height:
        raise ValueError(f"{name} is outside the input image")
    return x, y


def load_prompts(path: Path, width: int, height: int) -> dict[str, Any]:
    document = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(document, dict) or document.get("schema") != PROMPT_SCHEMA:
        raise ValueError(f"prompt schema must be {PROMPT_SCHEMA}")
    regions = document.get("regions")
    if not isinstance(regions, list):
        raise ValueError("regions must be an array")
    identifiers: set[str] = set()
    occupied = np.zeros((height, width), dtype=np.uint8)
    normalized = []
    for index, item in enumerate(regions):
        if not isinstance(item, dict):
            raise ValueError(f"regions[{index}] must be an object")
        identifier = item.get("id")
        if not isinstance(identifier, str) or not identifier or identifier in identifiers:
            raise ValueError(f"regions[{index}].id must be unique and non-empty")
        identifiers.add(identifier)
        box = item.get("box")
        if not isinstance(box, list) or len(box) != 4 or any(type(value) is not int for value in box):
            raise ValueError(f"regions[{index}].box must be [left, top, right, bottom]")
        left, top, right, bottom = box
        if left < 0 or top < 0 or right > width or bottom > height or right - left < 2 or bottom - top < 2:
            raise ValueError(f"regions[{index}].box is outside the input image or too small")
        if occupied[top:bottom, left:right].any():
            raise ValueError("evaluation ROIs must not overlap")
        occupied[top:bottom, left:right] = 1
        positive = [_point(value, f"regions[{index}].positive", width, height)
                    for value in item.get("positive", [])]
        negative = [_point(value, f"regions[{index}].negative", width, height)
                    for value in item.get("negative", [])]
        if not positive or not negative:
            raise ValueError(f"regions[{index}] requires positive skin and negative hair prompts")
        if any(not (left <= x < right and top <= y < bottom) for x, y in positive + negative):
            raise ValueError(f"regions[{index}] prompts must lie inside its ROI")
        normalized.append({**item, "id": identifier, "box": (left, top, right, bottom),
                           "positive": positive, "negative": negative})
    return {**document, "regions": normalized}


def _resize(array: np.ndarray, size: tuple[int, int], resample: int = Image.Resampling.BILINEAR) -> np.ndarray:
    image = Image.fromarray(array)
    return np.asarray(image.resize(size, resample=resample))


def _resize_probability(array: np.ndarray, width: int, height: int) -> np.ndarray:
    image = Image.fromarray(np.asarray(array, dtype=np.float32), mode="F")
    return np.asarray(image.resize((width, height), resample=Image.Resampling.BILINEAR), dtype=np.float32)


def _sigmoid(logits: np.ndarray) -> np.ndarray:
    clipped = np.clip(np.asarray(logits, dtype=np.float32), -30.0, 30.0)
    return 1.0 / (1.0 + np.exp(-clipped))


def _fixed_hw(shape: list[Any], default: int = 1024) -> tuple[int, int]:
    if len(shape) < 4:
        return default, default
    height = shape[-2] if isinstance(shape[-2], int) and shape[-2] > 0 else default
    width = shape[-1] if isinstance(shape[-1], int) and shape[-1] > 0 else default
    return int(width), int(height)


def _session(path: Path, threads: int):
    try:
        import onnxruntime as ort
    except ImportError as error:
        raise RuntimeError("onnxruntime is required only for this offline A/B tool") from error
    options = ort.SessionOptions()
    options.intra_op_num_threads = max(1, threads)
    options.inter_op_num_threads = 1
    options.execution_mode = ort.ExecutionMode.ORT_SEQUENTIAL
    return ort.InferenceSession(str(path), sess_options=options, providers=["CPUExecutionProvider"])


def _select_mask(outputs: list[np.ndarray]) -> np.ndarray:
    arrays = [np.asarray(value) for value in outputs]
    masks = next((value for value in arrays if value.ndim >= 4), None)
    if masks is None:
        raise RuntimeError("model did not return a mask tensor")
    scores = next((value for value in arrays if 1 <= value.ndim <= 3 and value.size > 1), None)
    masks = np.squeeze(masks)
    if masks.ndim == 2:
        return masks
    if masks.ndim != 3:
        raise RuntimeError(f"unsupported mask tensor shape: {masks.shape}")
    index = 0
    if scores is not None:
        flat = np.squeeze(scores).reshape(-1)
        if flat.size == masks.shape[0]:
            index = int(np.argmax(flat))
    return masks[index]


def _prompt_consistent_mask(masks: np.ndarray, scores: np.ndarray,
                            positive: list[tuple[float, float]],
                            negative: list[tuple[float, float]],
                            height: int, width: int) -> tuple[np.ndarray, float, bool]:
    masks, scores = np.asarray(masks), np.asarray(scores).reshape(-1)
    if masks.shape[-2:] != (height, width):
        raise RuntimeError("Pinned decoder did not restore original image dimensions")
    masks = masks.reshape(-1, height, width)
    if len(scores) != len(masks) or not np.isfinite(masks).all() or not np.isfinite(scores).all():
        raise RuntimeError("Invalid mask/quality output from the pinned decoder")
    valid = [i for i, mask in enumerate(masks)
             if all(mask[int(y), int(x)] >= 0 for x, y in positive)
             and all(mask[int(y), int(x)] < 0 for x, y in negative)]
    # Preserve an invalid highest-score candidate for diagnosis, with an
    # explicit rejection flag. Never present it as an accepted refinement.
    selected = max(valid, key=lambda i: scores[i]) if valid else int(np.argmax(scores))
    return masks[selected], float(scores[selected]), bool(valid)


def _efficient_image_tensor(crop: np.ndarray) -> np.ndarray:
    return np.transpose(crop.astype(np.float32) / 255.0, (2, 0, 1))[None]


def _efficient_prompt_tensors(positive: list[tuple[float, float]],
                              negative: list[tuple[float, float]]) -> tuple[np.ndarray, np.ndarray]:
    # Upstream documents labels 1/2/3 (point/box corners), with -1 padding.
    # Label 0 lacks a dedicated negative-point embedding in the pinned source.
    # We retain the historical label-0 probe to diagnose interface suitability,
    # but report it as an unverified contract, never a validated replacement.
    points = positive + negative
    coords = np.asarray([[points]], dtype=np.float32)
    labels = np.asarray([[[1] * len(positive) + [0] * len(negative)]], dtype=np.float32)
    return coords, labels


class EfficientSamRunner:
    name = "efficient-sam-tiny"

    def __init__(self, model: Path | None, encoder: Path | None,
                 decoder: Path | None, threads: int):
        if model is not None and (encoder is not None or decoder is not None):
            raise ValueError("EfficientSAM accepts either a combined model or an encoder/decoder pair")
        if model is None and (encoder is None or decoder is None):
            raise ValueError("EfficientSAM requires a combined model or both encoder and decoder")
        started = time.perf_counter()
        self.model = model
        self.encoder_path, self.decoder_path = encoder, decoder
        self.session = _session(model, threads) if model is not None else None
        self.encoder = _session(encoder, threads) if encoder is not None else None
        self.decoder = _session(decoder, threads) if decoder is not None else None
        self.load_ms = (time.perf_counter() - started) * 1000.0

    @property
    def hashes(self) -> dict[str, str]:
        paths = [path for path in (self.model, self.encoder_path, self.decoder_path) if path is not None]
        return {path.name: sha256_file(path) for path in paths}

    def infer(self, crop: np.ndarray, positive: list[tuple[float, float]],
              negative: list[tuple[float, float]]) -> tuple[np.ndarray, dict[str, float | None]]:
        if len(positive) + len(negative) > 6:
            raise ValueError("EfficientSAM's pinned decoder supports at most six points; refusing silent truncation")
        source_h, source_w = crop.shape[:2]
        image_tensor = _efficient_image_tensor(crop)
        coords, labels = _efficient_prompt_tensors(positive, negative)
        encoding_ms: float | None = None
        decoding_ms: float | None = None
        if self.session is not None:
            inputs = self.session.get_inputs()
            image_input = next((item for item in inputs if "image" in item.name.lower()), inputs[0])
            point_input = next((item for item in inputs if "coord" in item.name.lower()), None)
            label_input = next((item for item in inputs if "label" in item.name.lower()), None)
            if point_input is None or label_input is None:
                raise RuntimeError("EfficientSAM ONNX inputs must include point coordinates and labels")
            started = time.perf_counter()
            outputs = self.session.run(["output_masks", "iou_predictions"], {image_input.name: image_tensor,
                                              point_input.name: coords,
                                              label_input.name: labels})
            inference_ms = (time.perf_counter() - started) * 1000.0
        else:
            assert self.encoder is not None and self.decoder is not None
            encoder_input = self.encoder.get_inputs()[0]
            started = time.perf_counter()
            embeddings = self.encoder.run(None, {encoder_input.name: image_tensor})[0]
            encoding_ms = (time.perf_counter() - started) * 1000.0
            feed: dict[str, np.ndarray] = {}
            for item in self.decoder.get_inputs():
                name = item.name.lower()
                if "embedding" in name:
                    feed[item.name] = embeddings
                elif "coord" in name:
                    feed[item.name] = coords
                elif "label" in name:
                    feed[item.name] = labels
                elif "orig_im_size" in name or "original_size" in name:
                    feed[item.name] = np.asarray([source_h, source_w], dtype=np.int64)
                else:
                    raise RuntimeError(f"unsupported EfficientSAM decoder input: {item.name}")
            started = time.perf_counter()
            outputs = self.decoder.run(["output_masks", "iou_predictions"], feed)
            decoding_ms = (time.perf_counter() - started) * 1000.0
            inference_ms = encoding_ms + decoding_ms
        mask, model_score, consistent = _prompt_consistent_mask(
            outputs[0], outputs[1], positive, negative, source_h, source_w)
        probability = _sigmoid(mask)
        return probability, {"encoding_ms": encoding_ms, "decoding_ms": decoding_ms,
                             "inference_ms": inference_ms, "model_score": model_score,
                             "prompt_consistent": consistent, "prompt_limit": 6,
                             "prompt_contract_verified": not bool(negative),
                             "negative_prompt_contract": "label-0 probe; pinned source only documents labels 1/2/3" if negative else "not requested"}


class MobileSamRunner:
    name = "mobile-sam"

    def __init__(self, encoder: Path, decoder: Path, threads: int):
        started = time.perf_counter()
        self.encoder_path, self.decoder_path = encoder, decoder
        self.encoder = _session(encoder, threads)
        self.decoder = _session(decoder, threads)
        self.load_ms = (time.perf_counter() - started) * 1000.0

    @property
    def hashes(self) -> dict[str, str]:
        return {self.encoder_path.name: sha256_file(self.encoder_path),
                self.decoder_path.name: sha256_file(self.decoder_path)}

    def infer(self, crop: np.ndarray, positive: list[tuple[float, float]],
              negative: list[tuple[float, float]]) -> tuple[np.ndarray, dict[str, float | None]]:
        encoder_input = self.encoder.get_inputs()[0]
        target_w, target_h = _fixed_hw(encoder_input.shape)
        source_h, source_w = crop.shape[:2]
        scale = min(target_w / source_w, target_h / source_h)
        resized_w, resized_h = max(1, int(source_w * scale + .5)), max(1, int(source_h * scale + .5))
        resized = _resize(crop, (resized_w, resized_h)).astype(np.float32)
        canvas = np.zeros((target_h, target_w, 3), dtype=np.float32)
        mean = np.asarray([123.675, 116.28, 103.53], dtype=np.float32)
        std = np.asarray([58.395, 57.12, 57.375], dtype=np.float32)
        canvas[:resized_h, :resized_w] = (resized - mean) / std
        tensor = np.transpose(canvas, (2, 0, 1))[None]
        started = time.perf_counter()
        embeddings = self.encoder.run(None, {encoder_input.name: tensor})[0]
        encoding_ms = (time.perf_counter() - started) * 1000.0

        points = positive + negative
        coords = np.asarray([[[x * resized_w / source_w, y * resized_h / source_h]
                              for x, y in points] + [[0.0, 0.0]]], dtype=np.float32)
        labels = np.asarray([[1.0] * len(positive) + [0.0] * len(negative) + [-1.0]], dtype=np.float32)
        feed = _mobile_decoder_feed(self.decoder.get_inputs(), embeddings, coords, labels,
                                    source_h, source_w)
        started = time.perf_counter()
        outputs = self.decoder.run(["masks", "iou_predictions"], feed)
        decoding_ms = (time.perf_counter() - started) * 1000.0
        mask, model_score, consistent = _prompt_consistent_mask(
            outputs[0], outputs[1], positive, negative, source_h, source_w)
        probability = _sigmoid(mask)
        return probability, {"encoding_ms": encoding_ms, "decoding_ms": decoding_ms,
                             "inference_ms": encoding_ms + decoding_ms,
                             "model_score": model_score,
                             "prompt_consistent": consistent, "prompt_contract_verified": True,
                             "preprocess": MOBILE_PREPROCESS}


def _mobile_decoder_feed(inputs: list[Any], embeddings: np.ndarray, coords: np.ndarray,
                         labels: np.ndarray, source_h: int, source_w: int) -> dict[str, np.ndarray]:
        feed: dict[str, np.ndarray] = {}
        for item in inputs:
            name = item.name.lower()
            if "embedding" in name:
                feed[item.name] = embeddings
            elif "coord" in name:
                feed[item.name] = coords
            elif "label" in name:
                feed[item.name] = labels
            elif "has_mask" in name:
                feed[item.name] = np.zeros((1,), dtype=np.float32)
            elif "mask_input" in name:
                feed[item.name] = np.zeros((1, 1, 256, 256), dtype=np.float32)
            elif "orig_im_size" in name or "original_size" in name:
                feed[item.name] = np.asarray([source_h, source_w], dtype=np.float32)
            else:
                raise RuntimeError(f"unsupported MobileSAM decoder input: {item.name}")
        return feed


def _mask_path(prompt_path: Path, item: dict[str, Any], key: str) -> Path | None:
    value = item.get(key)
    if value is None:
        return None
    if not isinstance(value, str) or not value:
        raise ValueError(f"{key} must be a non-empty path")
    return (prompt_path.parent / value).resolve()


def _load_mask(path: Path | None, width: int, height: int) -> np.ndarray | None:
    if path is None:
        return None
    with Image.open(path) as image:
        mask = np.asarray(image.convert("L")) >= 128
    if mask.shape != (height, width):
        raise ValueError(f"mask dimensions do not match the input image: {path}")
    return mask


def _boundary(mask: np.ndarray) -> np.ndarray:
    if mask.size == 0:
        return mask
    padded = np.pad(mask, 1, constant_values=False)
    interior = (padded[1:-1, 1:-1] & padded[:-2, 1:-1] & padded[2:, 1:-1] &
                padded[1:-1, :-2] & padded[1:-1, 2:])
    return mask & ~interior


def contour_error(candidate: np.ndarray, reference: np.ndarray,
                  uncertain: np.ndarray | None = None) -> dict[str, float | None]:
    candidate_boundary = _boundary(candidate)
    reference_boundary = _boundary(reference)
    if uncertain is not None:
        candidate_boundary &= ~uncertain
        reference_boundary &= ~uncertain
    a = np.argwhere(candidate_boundary)
    b = np.argwhere(reference_boundary)
    if not len(a) or not len(b):
        return {"contour_p95_px": None, "contour_max_px": None}

    def directed(source: np.ndarray, target: np.ndarray) -> np.ndarray:
        distances = []
        for start in range(0, len(source), 512):
            delta = source[start:start + 512, None, :] - target[None, :, :]
            distances.append(np.sqrt(np.min(np.sum(delta * delta, axis=2), axis=1)))
        return np.concatenate(distances)

    distances = np.concatenate((directed(a, b), directed(b, a)))
    return {"contour_p95_px": float(np.percentile(distances, 95)),
            "contour_max_px": float(np.max(distances))}


def _draw_boundary(draw: ImageDraw.ImageDraw, boundary: np.ndarray, offset: tuple[int, int],
                   color: tuple[int, int, int]) -> None:
    top, left = np.nonzero(boundary)
    ox, oy = offset
    for y, x in zip(top.tolist(), left.tolist()):
        draw.point((ox + x, oy + y), fill=color)


def evaluate(runner: Any, image: np.ndarray, prompt_path: Path, prompts: dict[str, Any],
             output: Path, threshold: float) -> dict[str, Any]:
    output.mkdir(parents=True, exist_ok=True)
    height, width = image.shape[:2]
    overlay = Image.fromarray(image.copy())
    draw = ImageDraw.Draw(overlay)
    records = []
    total_changed = 0
    compared_regions = 0
    for item in prompts["regions"]:
        left, top, right, bottom = item["box"]
        crop = image[top:bottom, left:right]
        positive = [(float(x - left), float(y - top)) for x, y in item["positive"]]
        negative = [(float(x - left), float(y - top)) for x, y in item["negative"]]
        probability, timings = runner.infer(crop, positive, negative)
        if probability.shape != crop.shape[:2] or not np.isfinite(probability).all():
            raise RuntimeError(f"{runner.name} returned an invalid mask for {item['id']}")
        probability = np.clip(probability.astype(np.float32), 0.0, 1.0)
        candidate = probability >= threshold
        np.save(output / f"{item['id']}.soft-mask.npy", probability, allow_pickle=False)
        Image.fromarray(np.rint(probability * 255).astype(np.uint8)).save(
            output / f"{item['id']}.soft-mask.png")
        coarse_full = _load_mask(_mask_path(prompt_path, item, "coarse_mask"), width, height)
        reference_full = _load_mask(_mask_path(prompt_path, item, "reference_mask"), width, height)
        uncertain_full = _load_mask(_mask_path(prompt_path, item, "uncertain_mask"), width, height)
        coarse = None if coarse_full is None else coarse_full[top:bottom, left:right]
        reference = None if reference_full is None else reference_full[top:bottom, left:right]
        uncertain = None if uncertain_full is None else uncertain_full[top:bottom, left:right]
        changed = None if coarse is None else int(np.count_nonzero(candidate ^ coarse))
        if changed is not None:
            compared_regions += 1
            total_changed += changed
            _draw_boundary(draw, _boundary(coarse), (left, top), (255, 70, 70))
        _draw_boundary(draw, _boundary(candidate), (left, top), (45, 220, 100))
        draw.rectangle((left, top, right - 1, bottom - 1), outline=(255, 210, 40), width=1)
        metrics = contour_error(candidate, reference, uncertain) if reference is not None else {
            "contour_p95_px": None, "contour_max_px": None}
        records.append({"id": item["id"], "box": list(item["box"]),
                        "candidate_status": "prompt_gate_passed" if timings.get("prompt_consistent", True) and timings.get("prompt_contract_verified", True) else "diagnostic_only",
                        "candidate_pixels": int(np.count_nonzero(candidate)),
                        "changed_pixels": changed, **metrics, **timings})
    overlay.save(output / "overlay.png")
    return {"provider": runner.name, "load_ms": runner.load_ms, "model_sha256": runner.hashes,
            "peak_working_set_bytes": peak_working_set_bytes(),
            "changed_pixels": total_changed if compared_regions else None,
            "regions": records, "overlay": "overlay.png"}


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True, type=Path, help="512px face crop or fixed-view PNG")
    parser.add_argument("--prompts", required=True, type=Path, help="validated prompt JSON")
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--providers", nargs="+", choices=("mobile-sam", "efficient-sam-tiny"), required=True)
    parser.add_argument("--mobile-encoder", type=Path)
    parser.add_argument("--mobile-decoder", type=Path)
    parser.add_argument("--efficient-model", type=Path)
    parser.add_argument("--efficient-encoder", type=Path)
    parser.add_argument("--efficient-decoder", type=Path)
    parser.add_argument("--threshold", type=float, default=0.5)
    parser.add_argument("--threads", type=int, default=max(1, min(4, os.cpu_count() or 1)))
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    if not math.isfinite(args.threshold) or not 0.0 < args.threshold < 1.0:
        raise ValueError("threshold must be between zero and one")
    if args.threads < 1:
        raise ValueError("threads must be positive")
    with Image.open(args.input) as source:
        rgb = np.asarray(source.convert("RGB"))
    height, width = rgb.shape[:2]
    prompts = load_prompts(args.prompts, width, height)
    runners = []
    if "mobile-sam" in args.providers:
        if args.mobile_encoder is None or args.mobile_decoder is None:
            raise ValueError("MobileSAM requires --mobile-encoder and --mobile-decoder")
        runners.append(MobileSamRunner(args.mobile_encoder, args.mobile_decoder, args.threads))
    if "efficient-sam-tiny" in args.providers:
        has_combined = args.efficient_model is not None
        has_split = args.efficient_encoder is not None or args.efficient_decoder is not None
        if has_combined == has_split:
            raise ValueError("EfficientSAM-Tiny requires either --efficient-model or the encoder/decoder pair")
        if has_split and (args.efficient_encoder is None or args.efficient_decoder is None):
            raise ValueError("EfficientSAM-Tiny split mode requires both encoder and decoder")
        runners.append(EfficientSamRunner(args.efficient_model, args.efficient_encoder,
                                          args.efficient_decoder, args.threads))
    args.output.mkdir(parents=True, exist_ok=True)
    results = [evaluate(runner, rgb, args.prompts.resolve(), prompts,
                        args.output / runner.name, args.threshold) for runner in runners]
    card = {"schema": SCHEMA, "case_id": prompts.get("case_id", args.input.stem),
            "input": {"path": str(args.input.resolve()), "sha256": sha256_file(args.input),
                      "width": width, "height": height},
            "prompts": {"path": str(args.prompts.resolve()), "sha256": sha256_file(args.prompts)},
            "threshold": args.threshold, "threads": args.threads, "results": results}
    (args.output / "result-card.json").write_text(
        json.dumps(card, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(card, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
