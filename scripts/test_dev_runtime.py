"""Exercise development refresh against local Git/runtime fixtures; no real providers."""
from __future__ import annotations

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

import dev_runtime as dev


class RuntimeTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="orca-dev-runtime-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.build = self.root / ".tmp/build"
        self.runtime = self.root / ".tmp/dev/run"
        self.write(".gitignore", ".tmp/\n")
        self.write("CMakeLists.txt", 'set(ORCA_AI_SIDECAR_RUNTIME_FILES\n"${CMAKE_SOURCE_DIR}/tools/ai/provider.py"\n)\ninstall(FILES)\n')
        self.write("src/main.cpp", "native original")
        self.write("tools/ai/provider.py", "value = 1\n")
        for args in (("init", "-q"), ("config", "user.name", "Local fixture"),
                     ("config", "user.email", "fixture@example.invalid"),
                     ("config", "core.autocrlf", "false"), ("add", "."),
                     ("commit", "-qm", "local fixture")):
            dev.git(self.root, *args)
        self.write(".tmp/build/CMakeCache.txt", "\n".join((
            f"CMAKE_HOME_DIRECTORY:INTERNAL={self.root}",
            f"CMAKE_COMMAND:INTERNAL={sys.executable}",
            f"Python3_EXECUTABLE:FILEPATH={sys.executable}",
            "CMAKE_GENERATOR:INTERNAL=Visual Studio 17 2022",
            "ORCA_AI_WINDOWS_INSTALLER:BOOL=ON")))

    def write(self, name, text):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")

    def prepare(self):
        for name in ("orca-slicer.exe", "OrcaSlicer.dll"):
            self.write(".tmp/build/src/Release/" + name, name)
            self.write(".tmp/dev/run/" + name, name)
        for name in ("orca_ai_build_info.json", "orca_ai_runtime_dependencies.json"):
            self.write(".tmp/build/" + name, "{}")
            self.write(".tmp/dev/run/resources/tools/ai/" + name, "{}")
        for name in ("python/python.exe", "python/pythonw.exe", "resources/i18n/zh_CN/OrcaSlicer.mo"):
            self.write(".tmp/dev/run/" + name, "fixture")
        self.write(".tmp/dev/run/resources/tools/ai/provider.py", (self.root / "tools/ai/provider.py").read_text())
        self.write(".tmp/build/install_manifest.txt", "\n".join(str(p) for p in self.runtime.rglob("*") if p.is_file()))
        return dev.finish_install(self.root, self.build, dev.preflight(self.root, self.build))

    def test_preflight_is_read_only_and_accepts_dirty_detached_checkout(self):
        dev.git(self.root, "checkout", "--detach", "-q")
        self.write("tools/ai/provider.py", "value = 2\n")
        info = dev.preflight(self.root, self.build)
        self.assertFalse(info["source_clean"])
        self.assertFalse((self.root / ".tmp/dev").exists())
        self.assertEqual(info["source_identity"], dev.preflight(self.root, self.build)["source_identity"])

    def test_sidecar_updates_dirty_python_and_preserves_native_and_history(self):
        self.prepare()
        original = dev.digest(self.runtime / "OrcaSlicer.dll")
        self.write(".tmp/dev/data/generated_models/history.json", "existing history")
        self.write("tools/ai/provider.py", "value = 2\n")
        result = dev.update_sidecar(self.root, self.build)
        self.assertEqual(result["updated_modules"], ["provider.py"])
        self.assertEqual((self.runtime / "resources/tools/ai/provider.py").read_text(), "value = 2\n")
        self.assertEqual(original, dev.digest(self.runtime / "OrcaSlicer.dll"))
        self.assertEqual((self.root / ".tmp/dev/data/generated_models/history.json").read_text(), "existing history")
        self.assertEqual(dev.update_sidecar(self.root, self.build)["updated_modules"], [])

    def test_native_changes_require_rebuild_before_python_is_copied(self):
        for name in ("src/main.cpp", "src/new.hpp", "resources/new.json", "CMakeLists.txt"):
            with self.subTest(name=name):
                self.prepare()
                original = dev.digest(self.runtime / "resources/tools/ai/provider.py")
                path = self.root / name
                previous = path.read_text() if path.exists() else None
                self.write(name, (previous or "") + "\nchanged")
                self.write("tools/ai/provider.py", (self.root / "tools/ai/provider.py").read_text() + "# change\n")
                with self.assertRaisesRegex(ValueError, "Native sources"):
                    dev.update_sidecar(self.root, self.build)
                self.assertEqual(original, dev.digest(self.runtime / "resources/tools/ai/provider.py"))
                if previous is None:
                    path.unlink()
                else:
                    path.write_text(previous, encoding="utf-8")

    def test_changed_binary_or_configuration_requires_rebuild(self):
        self.prepare()
        self.write(".tmp/build/src/Release/OrcaSlicer.dll", "new binary")
        with self.assertRaisesRegex(ValueError, "Native binaries"):
            dev.update_sidecar(self.root, self.build)
        self.prepare()
        cache = self.build / "CMakeCache.txt"
        cache.write_text(cache.read_text() + "\nSLIC3R_PCH:BOOL=OFF\n")
        with self.assertRaisesRegex(ValueError, "configuration changed"):
            dev.update_sidecar(self.root, self.build)

    def test_incomplete_runtime_cannot_be_accepted(self):
        self.prepare()
        (self.runtime / "python/pythonw.exe").unlink()
        with self.assertRaisesRegex(ValueError, "incomplete"):
            dev.update_sidecar(self.root, self.build)

    def test_source_change_during_build_cannot_be_accepted(self):
        self.prepare()
        before = dev.preflight(self.root, self.build)
        self.write("src/main.cpp", "changed during build")
        with self.assertRaisesRegex(ValueError, "Source changed"):
            dev.finish_install(self.root, self.build, before)

    def test_foreign_build_cache_is_rejected(self):
        cache = self.build / "CMakeCache.txt"
        cache.write_text(cache.read_text().replace(str(self.root), str(self.build)))
        with self.assertRaisesRegex(ValueError, "another checkout"):
            dev.preflight(self.root, self.build)


class OfflineSelectionTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="orca-offline-selection-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        (self.root / "scripts").mkdir()
        (self.root / "tools/ai").mkdir(parents=True)
        self.runner = self.root / "scripts/run_ai_offline_tests.py"
        shutil.copy2(Path(__file__).with_name("run_ai_offline_tests.py"), self.runner)
        (self.root / "tools/ai/test_unselected.py").write_text("raise RuntimeError('unselected module was imported')\n")
        (self.root / "tools/ai/test_selected.py").write_text(
            "import os, socket, unittest\n"
            "class Selected(unittest.TestCase):\n"
            " def test_credentials_and_network(self):\n"
            "  self.assertNotIn('HY3D_API', os.environ)\n"
            "  self.assertNotIn('HUNYUAN3D_API_KEY', os.environ)\n"
            "  with self.assertRaisesRegex(RuntimeError, 'external networking'):\n"
            "   socket.create_connection(('example.invalid', 443))\n")

    def run_selection(self, pattern):
        env = dict(os.environ, HY3D_API="fixture-only", HUNYUAN3D_API_KEY="fixture-only")
        return subprocess.run([sys.executable, "-I", str(self.runner), "--pattern", pattern],
                              env=env, capture_output=True, text=True, timeout=30)

    def test_selection_retains_network_guard_and_removes_credentials(self):
        result = self.run_selection("test_selected.py")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("Ran 1 test", result.stderr)

    def test_no_matches_is_not_a_pass(self):
        result = self.run_selection("test_missing.py")
        self.assertEqual(result.returncode, 2)
        self.assertIn("No tests matched", result.stderr)

    def test_paths_are_not_test_patterns(self):
        result = self.run_selection("test_../selected.py")
        self.assertEqual(result.returncode, 2)
        self.assertIn("not a path", result.stderr)


if __name__ == "__main__":
    unittest.main()
