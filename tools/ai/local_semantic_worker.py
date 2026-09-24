"""Offline CPU semantic worker. Importing this module does not load ML libraries.

The probe exercises both local models; it does not certify mesh correspondence or
recognition accuracy. Mesh requests are enabled separately after binding validation.
No generation/provider client is imported, and no dependencies are installed here.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.metadata
import json
import os
from pathlib import Path
import struct
import sys
import time

PROTOCOL = "orcaslicer.local-semantic-worker.v1"
CONFIG_SCHEMA = "orcaslicer.local-semantic-runtime.v1"
WORKER_VERSION = "local-semantic-cpu-v1"
LABEL_NAMES = ["background", "neck", "face", "cloth", "rr", "lr", "rb", "lb",
               "re", "le", "nose", "imouth", "llip", "ulip", "hair", "eyeg",
               "hat", "earr", "neck_l"]
SUPPORTED_LABELS = ["neck", "face", "rr", "lr", "rb", "lb", "re", "le",
                    "nose", "imouth", "llip", "ulip", "hair", "cloth"]
WEIGHTS = {
    "mobilenet0.25_Final.pth":
        (1789735, "2979b33ffafda5d74b6948cd7a5b9a7a62f62b949cef24e95fd15d2883a65220"),
    "face_parsing.farl.celebm.main_ema_181500_jit.pt":
        (646629586, "bbc1f0e9f68c80eb83a0b23f33850d1e10f2ec1eda96884112d111c2c1f15c79"),
}
MAX_CONFIG_BYTES = 16 * 1024
MAX_RESPONSE_BYTES = 64 * 1024


class WorkerError(Exception):
    """Stable code suitable for UI; exception details and local paths stay private."""


def sha256_file(path: Path, expected_size: int | None = None) -> str:
    if not path.is_file() or (expected_size is not None and path.stat().st_size != expected_size):
        raise WorkerError("invalid_file_size")
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise WorkerError("duplicate_json_key")
        result[key] = value
    return result


def read_json(path: Path, limit: int):
    with path.open("rb") as stream:
        raw = stream.read(limit + 1)
    if len(raw) > limit:
        raise WorkerError("input_too_large")
    try:
        return json.loads(raw, object_pairs_hook=_unique_object,
                          parse_constant=lambda _: (_ for _ in ()).throw(WorkerError("nonfinite_json")))
    except (UnicodeError, ValueError, RecursionError) as error:
        raise WorkerError("invalid_json") from error


def load_config(path: Path) -> dict:
    config = read_json(path, MAX_CONFIG_BYTES)
    keys = {"schema", "enabled", "python_executable", "weights_directory", "cpu_threads",
            "timeout_seconds", "cache_bytes"}
    if not isinstance(config, dict) or set(config) != keys or config["schema"] != CONFIG_SCHEMA:
        raise WorkerError("invalid_config")
    if type(config["enabled"]) is not bool:
        raise WorkerError("invalid_config")
    for key in ("python_executable", "weights_directory"):
        value = config[key]
        if not isinstance(value, str) or not value or len(value) > 4096 or "\0" in value:
            raise WorkerError("invalid_config_path")
        if not Path(value).is_absolute():
            raise WorkerError("config_requires_absolute_path")
    for key, minimum, maximum in (("cpu_threads", 1, 8), ("timeout_seconds", 10, 600),
                                  ("cache_bytes", 0, 4 * 1024**3)):
        value = config[key]
        if type(value) is not int or not minimum <= value <= maximum:
            raise WorkerError("invalid_resource_limit")
    return config


def restrict_network() -> None:
    """Python-level protection against downloads, NOT an OS native network sandbox."""
    os.environ["HF_HUB_OFFLINE"] = "1"
    os.environ["TRANSFORMERS_OFFLINE"] = "1"
    os.environ["HF_HUB_DISABLE_TELEMETRY"] = "1"

    def reject(event, args):
        if event in ("socket.connect", "socket.getaddrinfo", "socket.bind", "socket.sendto"):
            raise WorkerError("network_attempt_rejected")

    sys.addaudithook(reject)


EYE_MODEL = "face_landmarker.task"
EYE_SHA256 = "64184e229b263107bc2b804c6625db1341ff2bb731874b0bcc2fe6544e0bc9ff"


def package_versions(weights):
    names = ["torch", "torchvision", "pyfacer", "numpy", "Pillow"]
    if EYE_MODEL in weights or 'selfie_multiclass_256x256.tflite' in weights:
        names.append("mediapipe")
    return {name: importlib.metadata.version(name) for name in names}


def check_weights(directory: Path) -> dict:
    actual = {}
    for filename, (size, expected) in WEIGHTS.items():
        try:
            actual[filename] = sha256_file(directory / filename, size)
        except (OSError, WorkerError) as error:
            raise WorkerError("weights_missing_or_invalid") from error
        if actual[filename] != expected:
            raise WorkerError("weights_hash_mismatch")
    # Optional, locally installed capability. An absent/incompatible extension
    # keeps the established parser available; no downloads or environment edits.
    try:
        if (importlib.metadata.version("mediapipe") == "1.0.1" and
                sha256_file(directory / EYE_MODEL, 3758596) == EYE_SHA256):
            actual[EYE_MODEL] = EYE_SHA256
    except (OSError, WorkerError, importlib.metadata.PackageNotFoundError):
        pass
    from local_body_regions import MODEL, SIZE, SHA256
    try:
        if importlib.metadata.version('mediapipe') == '1.0.1' and sha256_file(directory / MODEL, SIZE) == SHA256:
            actual[MODEL] = SHA256
    except (OSError, WorkerError, importlib.metadata.PackageNotFoundError):
        pass
    return actual


def load_models(config: dict):
    weights = check_weights(Path(config["weights_directory"]))
    # Deliberately delayed: ordinary product Python can import this module without torch.
    try:
        import torch
        import facer
    except ImportError as error:
        raise WorkerError("semantic_dependencies_unavailable") from error
    torch.set_num_threads(config["cpu_threads"])
    torch.set_num_interop_threads(1)
    directory = Path(config["weights_directory"])
    detector = facer.face_detector("retinaface/mobilenet", device="cpu",
                                  model_path=str(directory / "mobilenet0.25_Final.pth"))
    parser = facer.face_parser("farl/celebm/448", device="cpu", model_path=str(
        directory / "face_parsing.farl.celebm.main_ema_181500_jit.pt"))
    if list(parser.label_names) != LABEL_NAMES:
        raise WorkerError("unsupported_label_schema")
    parser.eye_landmarks = None
    parser.body_regions = None
    from local_body_regions import MODEL, load as load_body
    if MODEL in weights:
        try:
            parser.body_regions = load_body(directory / MODEL)
        except Exception:
            pass  # Optional local body evidence cannot disable face parsing.
    if EYE_MODEL in weights:
        try:
            from local_eye_landmarks import load
            parser.eye_landmarks = load(directory / EYE_MODEL)
        except Exception:
            # Eye hints are optional; the verified FaRL evidence remains usable.
            pass
    return torch, detector, parser, weights


def probe(config: dict, *, identity_only: bool = False) -> dict:
    """Identity mode hashes local files/metadata without importing or running ML.

    Only the explicit full probe certifies synthetic model execution. A mesh
    request certifies its own execution and is independently validated by Orca.
    """
    if not config["enabled"]:
        return {"status": "disabled", "capability_ready": False}
    if not os.path.samefile(config["python_executable"], sys.executable):
        raise WorkerError("interpreter_identity_mismatch")
    if struct.calcsize("P") != 8:
        raise WorkerError("requires_64bit_runtime")
    started = time.monotonic()
    if identity_only:
        weights = check_weights(Path(config["weights_directory"]))
    else:
        torch, detector, parser, weights = load_models(config)
        with torch.inference_mode():
            # Synthetic data tests execution, not face detection quality. Even when the
            # detector correctly finds no face, the parser is always exercised once.
            pixels = torch.full((1, 3, 128, 128), 127, dtype=torch.uint8)
            detection = detector(pixels)
            if "scores" not in detection or not torch.isfinite(detection["scores"]).all():
                raise WorkerError("detector_probe_failed")
            synthetic = {
                "image_ids": torch.tensor([0], dtype=torch.int64),
                "points": torch.tensor([[[42., 48.], [86., 48.], [64., 70.],
                                          [47., 93.], [81., 93.]]]),
                "rects": torch.tensor([[25., 20., 104., 110.]]),
                "scores": torch.tensor([1.]),
            }
            parsed = parser(pixels, synthetic)
            logits = parsed["seg"]["logits"]
            if tuple(logits.shape) != (1, len(LABEL_NAMES), 128, 128) or not torch.isfinite(logits).all():
                raise WorkerError("parser_probe_failed")
            if list(parsed["seg"]["label_names"]) != LABEL_NAMES:
                raise WorkerError("unsupported_label_schema")
    versions = package_versions(weights)
    identity = {"worker_version": WORKER_VERSION, "worker_sha256": sha256_file(Path(__file__)),
                "python": sys.version.split()[0], "python_executable_sha256": sha256_file(Path(sys.executable)),
                "bits": 64, "packages": versions, "weights": weights, "device": "cpu"}
    fingerprint = hashlib.sha256(json.dumps(identity, sort_keys=True, separators=(",", ":")).encode()).hexdigest()
    return {"status": "identity" if identity_only else "ok", "capability_ready": not identity_only, "identity": identity,
            "runtime_fingerprint": fingerprint, "label_schema": "farl-celebm-19-v1",
            "label_names": LABEL_NAMES, "supported_labels": SUPPORTED_LABELS,
            "unsupported": ["teeth", "pupil", "eye_white", "body_instance_segmentation"],
            "network_policy": "python-audit-hook-and-offline-flags; not-native-OS-sandbox",
            "mesh_requests_ready": False, "probe_seconds": round(time.monotonic() - started, 6)}


def write_response(path: Path, value: dict) -> None:
    raw = json.dumps(value, ensure_ascii=True, allow_nan=False, separators=(",", ":")).encode("utf-8")
    if len(raw) > MAX_RESPONSE_BYTES:
        raise WorkerError("response_too_large")
    # Parent owns a unique request directory. Never overwrite an earlier result.
    temporary = path.with_name(path.name + ".partial")
    if path.exists():
        raise WorkerError("response_already_exists")
    with temporary.open("xb") as stream:
        stream.write(raw)
        stream.flush()
        os.fsync(stream.fileno())
    # Hard link publishes atomically and fails if destination already exists.
    os.link(temporary, path)
    temporary.unlink()


def main(argv=None) -> int:
    arguments = argparse.ArgumentParser(description=__doc__)
    arguments.add_argument("--probe", action="store_true", required=True)
    arguments.add_argument("--identity-only", action="store_true",
                           help="Check file and package identities without loading models")
    arguments.add_argument("--config", type=Path, required=True)
    arguments.add_argument("--output", type=Path, required=True)
    options = arguments.parse_args(argv)
    response = {"schema": PROTOCOL, "worker_version": WORKER_VERSION}
    # The native host uses -I. Resolve optional helpers only from this verified
    # installation, never from cwd or an environment-supplied search path.
    directory = str(Path(__file__).resolve().parent)
    if directory not in sys.path:
        sys.path.insert(0, directory)
    restrict_network()
    try:
        response.update(probe(load_config(options.config), identity_only=options.identity_only))
    except WorkerError as error:
        response.update(status="unavailable", capability_ready=False, error_code=str(error))
    except Exception:
        # Do not dump environment, file paths or third-party exception strings.
        response.update(status="unavailable", capability_ready=False, error_code="local_probe_failed")
    try:
        write_response(options.output, response)
    except (OSError, WorkerError):
        return 3
    return 0 if response["status"] in ("ok", "identity", "disabled") else 2


if __name__ == "__main__":
    raise SystemExit(main())
