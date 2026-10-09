"""Read actual CMake target records without building or packaging software."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest


@unittest.skipUnless(os.name == "nt" and (shutil.which("pwsh") or shutil.which("powershell")), "Windows PowerShell")
class PackageArchitectureTests(unittest.TestCase):
    def resolve(self, platform="", compiler=None, processor="AMD64"):
        with tempfile.TemporaryDirectory(prefix="orca-package-architecture-") as temporary:
            build = Path(temporary)
            (build / "CMakeCache.txt").write_text("\n".join([
                f"CMAKE_GENERATOR_PLATFORM:INTERNAL={platform}",
                f"CMAKE_SYSTEM_PROCESSOR:INTERNAL={processor}",
                "CMAKE_CACHE_MAJOR_VERSION:INTERNAL=4",
                "CMAKE_CACHE_MINOR_VERSION:INTERNAL=4",
                "CMAKE_CACHE_PATCH_VERSION:INTERNAL=3", ""]), encoding="utf-8")
            if compiler is not None:
                record = build / "CMakeFiles/4.4.3/CMakeCXXCompiler.cmake"
                record.parent.mkdir(parents=True)
                record.write_text(f'set(CMAKE_CXX_COMPILER_ARCHITECTURE_ID "{compiler}")\n', encoding="utf-8")
            helper = Path(__file__).with_name("package_windows_architecture.ps1")
            quote = lambda path: "'" + str(path).replace("'", "''") + "'"
            command = f"$ErrorActionPreference='Stop'; . {quote(helper)}; Resolve-PackageWindowsArchitecture -BuildDir {quote(build)}"
            return subprocess.run([shutil.which("pwsh") or shutil.which("powershell"), "-NoProfile", "-Command", command],
                                  capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=30)

    def test_default_platform_uses_actual_x64_compiler(self):
        result = self.resolve(compiler="x64")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), "x64")

    def test_cross_target_uses_compiler_not_host_processor(self):
        result = self.resolve(compiler="ARM64", processor="AMD64")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), "arm64")

    def test_explicit_supported_platform_remains_readable(self):
        result = self.resolve(platform="x64")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), "x64")

    def test_host_architecture_cannot_replace_missing_target(self):
        result = self.resolve()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Unsupported or missing", result.stderr)

    def test_unsupported_explicit_platform_is_not_replaced(self):
        result = self.resolve(platform="Win32", compiler="x64")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Unsupported or missing", result.stderr)

    def test_unknown_default_compiler_is_rejected(self):
        result = self.resolve(compiler="unknown")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Unsupported or missing", result.stderr)

    def test_platform_and_compiler_conflict_is_rejected(self):
        result = self.resolve(platform="x64", compiler="ARM64")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("conflicts", result.stderr)


if __name__ == "__main__":
    unittest.main()
