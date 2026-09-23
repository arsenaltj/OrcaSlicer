"""Exercise development refresh against local Git/runtime fixtures; no real providers."""
from __future__ import annotations

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

import dev_runtime as dev
import dev_command


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
        self.write("resources/i18n/zh_CN/OrcaSlicer.mo", "fixture")
        self.write(".tmp/dev/run/dependency.dll", "dependency fixture")
        self.write(".tmp/dev/run/resources/tools/ai/provider.py", (self.root / "tools/ai/provider.py").read_text())
        self.write(".tmp/build/install_manifest.txt", "\n".join(str(p) for p in self.runtime.rglob("*") if p.is_file()))
        return dev.finish_install(self.root, self.build, dev.preflight(self.root, self.build))

    def test_installed_beauty_runtime_is_checked_against_configured_source(self):
        self.prepare()
        cache = self.build / 'CMakeCache.txt'
        cache.write_text(cache.read_text()+f'\nORCA_BEAUTY_RUNTIME_ROOT:PATH={self.root}/.tmp/beauty\n')
        self.write('.tmp/beauty/python/DLLs/test.dll', 'pinned dependency')
        name = 'resources/beauty-runtime/python/DLLs/test.dll'
        self.write('.tmp/dev/run/'+name, 'pinned dependency')
        self.assertIn(name, dev.verify_runtime(self.root,self.build,[name]))
        self.write('.tmp/dev/run/'+name, 'wrong dependency')
        with self.assertRaisesRegex(ValueError, 'Runtime resource does not match'):
            dev.verify_runtime(self.root,self.build,[name])

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
        with self.assertRaisesRegex(ValueError, "another checkout"):
            dev.setup_identity(self.root, self.build)

    def test_setup_accepts_incomplete_cache_with_matching_source(self):
        self.write(".tmp/build/CMakeCache.txt", f"CMAKE_HOME_DIRECTORY:INTERNAL={self.root}\n")
        self.assertIn("source_identity", dev.setup_identity(self.root, self.build))

    def test_catalog_refresh_uses_content_not_timestamps(self):
        self.write("tools/msgfmt.exe", "compiler fixture")
        self.write("localization/i18n/zh_CN/OrcaSlicer_zh_CN.po", "translation")
        target = self.root / "resources/i18n/zh_CN/OrcaSlicer.mo"
        def compile_catalog(command, **kwargs):
            Path(command[3]).write_bytes(b"compiled fixture")
        with mock.patch.object(dev.subprocess, "run", side_effect=compile_catalog) as compiler:
            self.assertEqual(dev.refresh_catalogs(self.root)["updated_catalogs"], ["zh_CN"])
            self.assertEqual(dev.refresh_catalogs(self.root)["updated_catalogs"], [])
            source = self.root / "localization/i18n/zh_CN/OrcaSlicer_zh_CN.po"
            stamp = source.stat().st_mtime
            source.write_text("new translation")
            os.utime(source, (stamp, stamp))
            self.assertEqual(dev.refresh_catalogs(self.root)["updated_catalogs"], ["zh_CN"])
            target.write_bytes(b"changed generated bytes")
            self.assertEqual(dev.refresh_catalogs(self.root)["updated_catalogs"], ["zh_CN"])
            self.assertEqual(compiler.call_count, 3)

    def test_translation_and_generated_catalog_changes_require_refresh(self):
        for name in ("localization/i18n/zh_CN/OrcaSlicer_zh_CN.po",
                     "resources/i18n/zh_CN/OrcaSlicer.mo",
                     "tools/ai/local_semantic_runtime_files.cmake"):
            with self.subTest(name=name):
                self.prepare()
                self.write(name, "modified input")
                with self.assertRaisesRegex(ValueError, "Native sources"):
                    dev.update_sidecar(self.root, self.build)

    def test_modified_installed_resource_or_dependency_is_rejected(self):
        for name in ("resources/i18n/zh_CN/OrcaSlicer.mo", "dependency.dll"):
            with self.subTest(name=name):
                self.prepare()
                self.write(".tmp/dev/run/" + name, "modified runtime")
                with self.assertRaisesRegex(ValueError, "Installed runtime file changed"):
                    dev.update_sidecar(self.root, self.build)

    def test_finish_rejects_stale_installed_resource(self):
        self.prepare()
        self.write(".tmp/dev/run/resources/i18n/zh_CN/OrcaSlicer.mo", "old resource")
        with self.assertRaisesRegex(ValueError, "Runtime resource"):
            dev.finish_install(self.root, self.build, dev.preflight(self.root, self.build))

    def test_configured_uv_is_verified_against_its_install_source(self):
        self.write(".tmp/build/.uv/uv.exe", "configured tool")
        self.write(".tmp/dev/run/resources/tools/uv/uv.exe", "configured tool")
        self.write(".tmp/build/cmake_install.cmake",
                   'file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/./resources/tools/uv" '
                   'TYPE PROGRAM RENAME "uv.exe" FILES "' +
                   (self.build / ".uv/uv.exe").as_posix() + '")\n')
        self.prepare()
        self.write(".tmp/dev/run/resources/tools/uv/uv.exe", "different tool")
        with self.assertRaisesRegex(ValueError, "Runtime does not match"):
            dev.finish_install(self.root, self.build, dev.preflight(self.root, self.build))

    def test_component_modules_are_refreshed_and_unknown_variables_fail(self):
        self.write("CMakeLists.txt", 'set(ORCA_AI_SIDECAR_RUNTIME_FILES\n'
                   '"${CMAKE_SOURCE_DIR}/tools/ai/provider.py"\n'
                   '${ORCA_LOCAL_SEMANTIC_RUNTIME_FILES}\n)\ninstall(FILES)\n')
        self.write("tools/ai/local_semantic_runtime_files.cmake",
                   'set(ORCA_LOCAL_SEMANTIC_RUNTIME_FILES "${CMAKE_SOURCE_DIR}/tools/ai/worker.py")')
        self.write("tools/ai/worker.py", "original")
        self.write(".tmp/dev/run/resources/tools/ai/worker.py", "original")
        self.prepare()
        self.write("tools/ai/worker.py", "updated")
        result = dev.update_sidecar(self.root, self.build)
        self.assertIn("worker.py", result["updated_modules"])
        self.assertEqual((self.runtime / "resources/tools/ai/worker.py").read_text(), "updated")
        self.write("CMakeLists.txt", 'set(ORCA_AI_SIDECAR_RUNTIME_FILES ${UNKNOWN_COMPONENT})\ninstall(FILES)')
        with self.assertRaisesRegex(ValueError, "Unsupported sidecar runtime variable"):
            dev.modules(self.root)


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


class CommandTests(unittest.TestCase):
    def test_windows_environment_keys_are_unique_without_disclosing_values(self):
        with mock.patch.object(dev_command.os, "name", "nt"):
            result = dev_command.child_environment({"Path": "tool", "PATH": "tool", "fixture": "value"})
        self.assertEqual(result, {"PATH": "tool", "FIXTURE": "value"})

    def test_command_preserves_argument_boundaries_and_exit_status(self):
        runner = Path(dev_command.__file__)
        result = subprocess.run([sys.executable, "-I", str(runner), sys.executable, "-c",
                                 "import sys; assert sys.argv[1] == 'a b'; sys.exit(7)", "a b"],
                                capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 7, result.stderr)


if __name__ == "__main__":
    unittest.main()
