from __future__ import annotations

import ast
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest


REPO_ROOT = Path(__file__).resolve().parents[2]
TOOLS_AI = REPO_ROOT / "tools" / "ai"


def packaged_runtime_files(portrait_enabled: bool = False,
                           legacy_farl_enabled: bool = False) -> list[Path]:
    from scripts.dev_runtime import modules
    # Source-only CI has no configured application build. Specify the tested
    # capability explicitly without weakening the runtime's cache requirement.
    with tempfile.TemporaryDirectory(prefix="orca-packaged-config-") as directory:
        build = Path(directory)
        (build / "CMakeCache.txt").write_text(
            f"ORCA_AI_PORTRAIT_RECOGNITION:BOOL={'ON' if portrait_enabled else 'OFF'}\n"
            f"ORCA_AI_LEGACY_FARL_RUNTIME:BOOL={'ON' if legacy_farl_enabled else 'OFF'}\n",
            encoding="utf-8",
        )
        return [TOOLS_AI / name for name in modules(REPO_ROOT, build)]


class PackagedSidecarTests(unittest.TestCase):
    def test_retired_portrait_modules_follow_only_the_legacy_research_capability(self) -> None:
        for portrait_enabled, legacy_enabled in ((False, False), (True, False), (True, True)):
            with self.subTest(portrait_enabled=portrait_enabled, legacy_enabled=legacy_enabled):
                names = {path.name for path in packaged_runtime_files(portrait_enabled, legacy_enabled)}
                for name in ("portrait_parent_cleanup_catalog.json",
                             "portrait_parent_cleanup_fangfei.json",
                             "portrait_parent_cleanup_partition.json",
                             "portrait_parent_cleanup_locks.json"):
                    self.assertEqual(name in names, legacy_enabled)
                self.assertEqual("local_semantic_worker.py" in names, legacy_enabled)

    def test_runtime_list_contains_all_local_imports(self) -> None:
        files = packaged_runtime_files()
        included = {path.stem for path in files}
        self.assertIn("orca_ai_installed_bootstrap", included)
        self.assertIn("orca_ai_sidecar", included)
        for path in files:
            self.assertFalse(path.name.startswith("test_"))
            for node in ast.walk(ast.parse(path.read_text(encoding="utf-8"))):
                names = []
                if isinstance(node, ast.Import):
                    names = [alias.name for alias in node.names]
                elif isinstance(node, ast.ImportFrom) and node.module:
                    names = [node.module]
                for name in names:
                    module = name.split(".")[0]
                    if (TOOLS_AI / f"{module}.py").is_file():
                        with self.subTest(source=path.name, dependency=module):
                            self.assertIn(module, included, f"Missing packaged dependency: {module}.py")

    def test_only_packaged_files_import_in_isolated_python(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            runtime = Path(directory)
            for path in packaged_runtime_files():
                shutil.copyfile(path, runtime / path.name)
            # Never inherit a developer's live providers into a packaging check.
            environment = {name: value for name, value in os.environ.items()
                           if not name.upper().startswith(("OPENAI_", "TRIPO_", "TRIPO3D_", "ORCASLICER_AI_"))}
            code = """
import pathlib, socket, sys
def deny_network(*args, **kwargs):
    raise AssertionError('Packaging import check must not connect to a service')
socket.socket.connect = deny_network
socket.create_connection = deny_network
sys.path.insert(0, sys.argv[1])
import orca_ai_sidecar
assert pathlib.Path(orca_ai_sidecar.__file__).parent == pathlib.Path(sys.argv[1])
print('Packaged sidecar import passed')
"""
            result = subprocess.run(
                [sys.executable, "-I", "-B", "-c", code, str(runtime)], cwd=runtime,
                env=environment, capture_output=True, text=True, timeout=30,
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("Packaged sidecar import passed", result.stdout)


if __name__ == "__main__":
    unittest.main()
