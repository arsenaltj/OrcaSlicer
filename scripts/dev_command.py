"""Run a visible development command with a Windows-compatible environment.

Some hosts supply both Path and PATH. .NET Framework MSBuild then throws
MSB6001 before it can start CL.exe. Normalize child environment key casing;
do not change values, shell-encode commands, or print environment contents.
"""
from __future__ import annotations

import os
import subprocess
import sys


def child_environment(environment: dict[str, str]) -> dict[str, str]:
    return {key.upper(): value for key, value in environment.items()} if os.name == "nt" else dict(environment)


def main() -> int:
    if len(sys.argv) < 2:
        raise SystemExit("Usage: dev_command.py executable [arguments ...]")
    return subprocess.call(sys.argv[1:], env=child_environment(dict(os.environ)))


if __name__ == "__main__":
    raise SystemExit(main())
