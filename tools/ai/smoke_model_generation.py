#!/usr/bin/env python3
"""Legacy client retained for isolated offline tests; use Orca for real generation."""
import json
import time
import urllib.error
import urllib.request
import uuid
from dataclasses import dataclass
from pathlib import Path
from typing import Callable
from urllib.parse import urlsplit


MAX_IMAGE_BYTES = 20 * 1024 * 1024
MAX_ARTIFACT_BYTES = 512 * 1024 * 1024
SUPPORTED_FORMATS = {"obj"}
DEFAULT_PRINTABLE_PALETTE = ("#FFFFFF", "#000000", "#FF0000", "#00A651")
TERMINAL_FAILURE_STATES = {"failed", "stopped"}


class SmokeError(RuntimeError):
    pass


class PaidCallConfirmationRequired(SmokeError):
    pass


@dataclass(frozen=True)
class SmokeResult:
    source: str
    job_id: str
    artifact_format: str
    artifact_path: Path
    duration_seconds: float
    states: tuple[str, ...]


class ModelGenerationSmokeClient:
    def __init__(
        self,
        endpoint: str,
        *,
        poll_interval: float = 2.0,
        timeout: float = 900.0,
        on_status: Callable[[dict], None] | None = None,
    ):
        parsed = urlsplit(endpoint)
        if (
            parsed.scheme != "http"
            or parsed.hostname not in {"127.0.0.1", "localhost", "::1"}
            or parsed.username is not None
            or parsed.password is not None
            or parsed.query
            or parsed.fragment
        ):
            raise SmokeError("The sidecar endpoint must be a credential-free loopback HTTP URL.")
        if poll_interval <= 0 or timeout <= 0:
            raise SmokeError("Polling interval and timeout must be positive.")
        self.endpoint = endpoint.rstrip("/")
        self.poll_interval = poll_interval
        self.timeout = timeout
        self.on_status = on_status

    def run_text(
        self,
        prompt: str,
        output_dir: Path,
        *,
        confirm_paid_call: bool,
        palette=DEFAULT_PRINTABLE_PALETTE,
    ) -> SmokeResult:
        prompt = prompt.strip()
        if not prompt:
            raise SmokeError("A text prompt is required.")
        return self._run(
            "text",
            output_dir,
            confirm_paid_call,
            lambda: self._request_json(
                "/v1/orcaslicer/model-jobs/text",
                method="POST",
                payload={"request_id": str(uuid.uuid4()), "prompt": prompt, "palette": list(palette)},
            ),
            palette,
        )

    def run_image(
        self,
        image_path: Path,
        instruction: str,
        output_dir: Path,
        *,
        confirm_paid_call: bool,
        palette=DEFAULT_PRINTABLE_PALETTE,
    ) -> SmokeResult:
        instruction = instruction.strip()
        if not instruction:
            raise SmokeError("An image instruction is required.")
        image_path = image_path.resolve()
        try:
            image = image_path.read_bytes()
        except OSError as exc:
            raise SmokeError("The reference image could not be read.") from exc
        if not image or len(image) > MAX_IMAGE_BYTES:
            raise SmokeError("The reference image must be between 1 byte and 20 MB.")
        image_type = self._image_content_type(image)
        body, content_type = self._multipart_body(
            {
                "request_id": str(uuid.uuid4()),
                "instruction": instruction,
                "palette": json.dumps(list(palette)),
            },
            image_path.name,
            image_type,
            image,
        )
        return self._run(
            "image",
            output_dir,
            confirm_paid_call,
            lambda: self._request_json(
                "/v1/orcaslicer/model-jobs/image",
                method="POST",
                body=body,
                content_type=content_type,
            ),
            palette,
        )

    def _run(self, source, output_dir, confirm_paid_call, create_job, palette):
        if not confirm_paid_call:
            raise PaidCallConfirmationRequired("Pass --confirm-paid-call to create a model job.")
        self._assert_generation_ready()
        output_dir.mkdir(parents=True, exist_ok=True)
        start = time.monotonic()
        job_id = ""
        states = []
        try:
            status = self._job(create_job())
            job_id = status["id"]
            self._record_status(status, states)
            status = self._wait_for(job_id, {"awaiting_confirmation"}, states)
            prepared_prompt = status.get("prepared_prompt", "") if source == "text" else ""
            status = self._job(
                self._request_json(
                    f"/v1/orcaslicer/model-jobs/{job_id}/generate",
                    method="POST",
                    payload={"prepared_prompt": prepared_prompt, "palette": list(palette)},
                )
            )
            self._record_status(status, states)
            status = self._wait_for(job_id, {"ready"}, states)
            artifact = status.get("artifact") or {}
            artifact_format = str(artifact.get("format", "")).lower()
            if not artifact.get("ready") or artifact_format not in SUPPORTED_FORMATS:
                raise SmokeError("The sidecar reported an unsupported or unavailable artifact.")
            artifact_path = output_dir / f"{source}-{job_id}.{artifact_format}"
            self._download_artifact(job_id, artifact_path)
            return SmokeResult(
                source=source,
                job_id=job_id,
                artifact_format=artifact_format,
                artifact_path=artifact_path,
                duration_seconds=time.monotonic() - start,
                states=tuple(states),
            )
        finally:
            if job_id:
                self._delete_job(job_id)

    def _assert_generation_ready(self):
        health = self._request_json("/health", native_client=False)
        generation = (health.get("capabilities") or {}).get("model_generation") or {}
        if (
            health.get("ok") is not True
            or health.get("protocol_version") != 2
            or generation.get("available") is not True
            or not {"text", "image"}.issubset(set(generation.get("sources") or []))
            or not SUPPORTED_FORMATS.intersection(generation.get("artifact_formats") or [])
        ):
            raise SmokeError("The sidecar is not ready for protocol-v1 model generation.")

    def _wait_for(self, job_id, expected_states, states):
        deadline = time.monotonic() + self.timeout
        while time.monotonic() < deadline:
            status = self._job(self._request_json(f"/v1/orcaslicer/model-jobs/{job_id}"))
            self._record_status(status, states)
            state = status.get("state", "")
            if state in expected_states:
                return status
            if state in TERMINAL_FAILURE_STATES:
                message = str(status.get("message") or "Model generation failed.")
                raise SmokeError(f"Model job entered {state}: {message}")
            time.sleep(self.poll_interval)
        raise SmokeError(f"Model job did not reach {sorted(expected_states)} before timeout.")

    def _record_status(self, status, states):
        state = str(status.get("state") or "")
        if not state:
            raise SmokeError("The sidecar returned a model job without state.")
        if not states or states[-1] != state:
            states.append(state)
        if self.on_status is not None:
            self.on_status(status)

    @staticmethod
    def _job(response):
        job = response.get("job") if isinstance(response, dict) else None
        if not isinstance(job, dict) or not job.get("id"):
            raise SmokeError("The sidecar returned an invalid model job response.")
        return job

    def _request_json(
        self,
        path,
        *,
        method="GET",
        payload=None,
        body=None,
        content_type=None,
        native_client=True,
    ):
        if payload is not None:
            body = json.dumps(payload).encode("utf-8")
            content_type = "application/json"
        headers = {"Accept": "application/json"}
        if native_client:
            headers["X-OrcaSlicer-Client"] = "native"
        if content_type:
            headers["Content-Type"] = content_type
        request = urllib.request.Request(
            self.endpoint + path,
            data=body,
            headers=headers,
            method=method,
        )
        try:
            with urllib.request.urlopen(request, timeout=min(self.timeout, 30)) as response:
                raw = response.read(1024 * 1024 + 1)
        except urllib.error.HTTPError as exc:
            raw = exc.read(16 * 1024)
            raise SmokeError(self._http_error(exc.code, raw)) from None
        except (OSError, urllib.error.URLError) as exc:
            raise SmokeError("The local AI sidecar request failed.") from exc
        if len(raw) > 1024 * 1024:
            raise SmokeError("The sidecar JSON response exceeded 1 MB.")
        try:
            result = json.loads(raw)
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            raise SmokeError("The sidecar returned invalid JSON.") from exc
        if not isinstance(result, dict):
            raise SmokeError("The sidecar returned an invalid JSON object.")
        return result

    def _download_artifact(self, job_id, destination):
        request = urllib.request.Request(
            self.endpoint + f"/v1/orcaslicer/model-jobs/{job_id}/artifact",
            headers={"X-OrcaSlicer-Client": "native"},
        )
        try:
            with urllib.request.urlopen(request, timeout=min(self.timeout, 60)) as response:
                with destination.open("wb") as output:
                    total = 0
                    while True:
                        chunk = response.read(1024 * 1024)
                        if not chunk:
                            break
                        total += len(chunk)
                        if total > MAX_ARTIFACT_BYTES:
                            raise SmokeError("The generated artifact exceeded 512 MB.")
                        output.write(chunk)
        except urllib.error.HTTPError as exc:
            destination.unlink(missing_ok=True)
            raw = exc.read(16 * 1024)
            raise SmokeError(self._http_error(exc.code, raw)) from None
        except SmokeError:
            destination.unlink(missing_ok=True)
            raise
        except (OSError, urllib.error.URLError) as exc:
            destination.unlink(missing_ok=True)
            raise SmokeError("The generated artifact could not be downloaded.") from exc
        if destination.stat().st_size == 0:
            destination.unlink(missing_ok=True)
            raise SmokeError("The generated artifact was empty.")

    def _delete_job(self, job_id):
        request = urllib.request.Request(
            self.endpoint + f"/v1/orcaslicer/model-jobs/{job_id}",
            headers={"X-OrcaSlicer-Client": "native"},
            method="DELETE",
        )
        try:
            with urllib.request.urlopen(request, timeout=10):
                pass
        except (OSError, urllib.error.URLError):
            pass

    @staticmethod
    def _http_error(status, raw):
        try:
            payload = json.loads(raw)
            error = payload.get("error", {})
            if isinstance(error, dict):
                message = error.get("message")
            else:
                message = error
            if message:
                return f"Sidecar HTTP {status}: {message}"
        except (UnicodeDecodeError, json.JSONDecodeError, AttributeError):
            pass
        return f"Sidecar request failed with HTTP {status}."

    @staticmethod
    def _image_content_type(image):
        if image.startswith(b"\x89PNG\r\n\x1a\n"):
            return "image/png"
        if image.startswith(b"\xff\xd8\xff"):
            return "image/jpeg"
        raise SmokeError("The reference image must be PNG or JPEG.")

    @staticmethod
    def _multipart_body(fields, filename, image_type, image):
        boundary = "----OrcaSlicerSmoke" + uuid.uuid4().hex
        chunks = []
        for name, value in fields.items():
            chunks.extend(
                [
                    f"--{boundary}\r\n".encode(),
                    f'Content-Disposition: form-data; name="{name}"\r\n\r\n'.encode(),
                    str(value).encode("utf-8"),
                    b"\r\n",
                ]
            )
        safe_filename = Path(filename).name.replace('"', "")
        chunks.extend(
            [
                f"--{boundary}\r\n".encode(),
                (
                    f'Content-Disposition: form-data; name="image"; filename="{safe_filename}"\r\n'
                    f"Content-Type: {image_type}\r\n\r\n"
                ).encode(),
                image,
                b"\r\n",
                f"--{boundary}--\r\n".encode(),
            ]
        )
        return b"".join(chunks), f"multipart/form-data; boundary={boundary}"


if __name__ == "__main__":
    raise SystemExit("This command-line entry is retired. Use the Orca main window for real generation.")
