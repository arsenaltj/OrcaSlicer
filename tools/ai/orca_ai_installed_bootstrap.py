from __future__ import annotations

import os
from pathlib import Path
import runpy
import sys
from typing import TextIO


MAX_LOG_BYTES = 5 * 1024 * 1024
LOG_BACKUPS = 3


def configure_runtime(data_directory: str) -> tuple[Path, Path]:
    data_dir = Path(data_directory).expanduser().resolve()
    output_dir = data_dir / "generated_models"
    log_dir = data_dir / "log"
    output_dir.mkdir(parents=True, exist_ok=True)
    log_dir.mkdir(parents=True, exist_ok=True)

    os.environ.setdefault("ORCASLICER_AI_OUTPUT_DIR", str(output_dir))
    os.environ.setdefault("PYTHONIOENCODING", "utf-8")
    os.environ.setdefault("PYTHONUTF8", "1")
    return output_dir, log_dir / "orca-ai-sidecar.log"


def redirect_output(log_path: Path) -> TextIO:
    rotate_log(log_path)
    stream = log_path.open("a", encoding="utf-8", buffering=1)
    sys.stdout = stream
    sys.stderr = stream
    return stream


def rotate_log(log_path: Path, max_bytes: int = MAX_LOG_BYTES, backups: int = LOG_BACKUPS) -> None:
    try:
        if backups <= 0 or not log_path.is_file() or log_path.stat().st_size < max_bytes:
            return
        oldest = log_path.with_name(f"{log_path.name}.{backups}")
        oldest.unlink(missing_ok=True)
        for index in range(backups - 1, 0, -1):
            source = log_path.with_name(f"{log_path.name}.{index}")
            if source.exists():
                source.replace(log_path.with_name(f"{log_path.name}.{index + 1}"))
        log_path.replace(log_path.with_name(f"{log_path.name}.1"))
    except OSError:
        # Logging is diagnostic-only; a locked/unwritable log must not stop Orca.
        return


def run_installed_sidecar(data_directory: str) -> None:
    _, log_path = configure_runtime(data_directory)
    stream = redirect_output(log_path)
    script_path = Path(__file__).with_name("orca_ai_sidecar.py").resolve()
    if not script_path.is_file():
        print(f"Installed AI Sidecar is missing: {script_path}", file=stream)
        raise FileNotFoundError(script_path)

    sys.path.insert(0, str(script_path.parent))
    sys.argv = [str(script_path)]
    print(f"Starting installed OrcaSlicer AI Sidecar from {script_path}")
    runpy.run_path(str(script_path), run_name="__main__")


def main(argv: list[str] | None = None) -> int:
    arguments = list(sys.argv[1:] if argv is None else argv)
    if len(arguments) != 1 or not arguments[0].strip():
        print("Usage: orca_ai_installed_bootstrap.py <orcaslicer-data-directory>", file=sys.stderr)
        return 2
    run_installed_sidecar(arguments[0])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
