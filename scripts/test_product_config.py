"""Product version edits must agree across the application and packaging readers."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
import xml.etree.ElementTree as ET

from product_config import load_product


REPO = Path(__file__).resolve().parents[1]


class ProductIdentityTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="honor-product-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / "config").mkdir()
        self.value = {"display_name": "Honor Test Studio", "version": "3.12.7", "build_number": 42}
        self.save()

    def save(self):
        (self.root / "config/product.json").write_text(json.dumps(self.value), encoding="utf-8")

    def test_name_with_spaces_and_version_edit_drive_all_file_names(self):
        product = load_product(self.root)
        self.assertEqual(product["exe"], "Honor Test Studio.exe")
        self.assertEqual(product["dll"], "Honor Test Studio.dll")
        self.assertEqual(product["file_version"], "3.12.7.42")
        self.assertEqual(product["display_version"], "V3.12.7")
        self.assertEqual(product["command"], "honor-test-studio")

    def test_invalid_identity_is_rejected_before_build_or_packaging(self):
        for key, value in (("display_name", "../escape"), ("display_name", "App;command"),
                           ("version", "V1.0.0"), ("version", "01.0.0"), ("version", "1.0"),
                           ("version", "65536.0.0"), ("build_number", True),
                           ("build_number", -1), ("build_number", 65536), ("build_number", 1.5)):
            with self.subTest(key=key, value=value):
                original = self.value.copy()
                self.value[key] = value
                self.save()
                with self.assertRaises(ValueError):
                    load_product(self.root)
                self.value = original

    def test_cmake_uses_the_same_edited_config_and_rejects_invalid_version(self):
        settings = json.loads((REPO / ".tmp/dev/settings.json").read_text(encoding="utf-8-sig")) if (REPO / ".tmp/dev/settings.json").exists() else {}
        cmake = shutil.which("cmake") or settings.get("cmake")
        if not cmake:
            self.skipTest("CMake is unavailable")
        (self.root / "cmake").mkdir()
        shutil.copy2(REPO / "cmake/ProductIdentity.cmake", self.root / "cmake/ProductIdentity.cmake")
        probe = self.root / "probe.cmake"
        probe.write_text('include("${CMAKE_CURRENT_LIST_DIR}/cmake/ProductIdentity.cmake")\n'
                         'file(WRITE "${CMAKE_CURRENT_LIST_DIR}/result.txt" '
                         '"${HONOR_PRODUCT_EXE}|${HONOR_PRODUCT_DLL}|${HONOR_PRODUCT_DISPLAY_VERSION}|${HONOR_PRODUCT_FILE_VERSION}")\n')
        result = subprocess.run([cmake, "-P", str(probe)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((self.root / "result.txt").read_text(), "Honor Test Studio.exe|Honor Test Studio.dll|V3.12.7|3.12.7.42")
        self.value["version"] = "65536.0.0"
        self.save()
        self.assertNotEqual(subprocess.run([cmake, "-P", str(probe)], capture_output=True).returncode, 0)

    @unittest.skipUnless(os.name == "nt", "PowerShell is a Windows packaging consumer")
    def test_msix_layout_uses_the_product_executable_and_display_version(self):
        product = load_product(REPO)
        install = self.root / "installed"
        install.mkdir()
        (install / product["exe"]).write_bytes(b"fixture-only; never executed")
        staging = self.root / "loose"
        shell = shutil.which("pwsh") or "powershell"
        result = subprocess.run([shell, "-NoProfile", "-File", str(REPO / "scripts/msix/build_msix.ps1"),
                                 "-InstallDir", str(install), "-StagingDir", str(staging), "-StageOnly"],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        raw = (staging / "AppxManifest.xml").read_text(encoding="utf-8-sig")
        self.assertNotIn("@PRODUCT_", raw)
        package = ET.fromstring(raw)
        ns = {"p": "http://schemas.microsoft.com/appx/manifest/foundation/windows10"}
        self.assertEqual(package.find("p:Applications/p:Application", ns).get("Executable"), product["exe"])
        self.assertEqual(package.find("p:Properties/p:DisplayName", ns).text, product["display_name"])
        self.assertEqual(package.find("p:Identity", ns).get("Version"), product["version"] + ".0")
        self.assertTrue((staging / product["exe"]).is_file())

    @unittest.skipUnless(os.name == "nt", "PowerShell is a Windows packaging consumer")
    def test_powershell_reads_the_same_identity_and_rejects_invalid_version(self):
        script = self.root / "probe.ps1"
        script.write_text("$ErrorActionPreference='Stop'\n. '" + str(REPO / "scripts/ProductConfig.ps1").replace("'", "''") + "'\n"
                          "Get-ProductIdentity -Root $PSScriptRoot | ConvertTo-Json\n")
        shell = shutil.which("pwsh") or "powershell"
        result = subprocess.run([shell, "-NoProfile", "-File", str(script)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        actual = json.loads(result.stdout)
        self.assertEqual(actual["FileVersion"], "3.12.7.42")
        self.assertEqual(actual["Exe"], "Honor Test Studio.exe")
        self.value["version"] = "65536.0.0"
        self.save()
        self.assertNotEqual(subprocess.run([shell, "-NoProfile", "-File", str(script)], capture_output=True).returncode, 0)


if __name__ == "__main__":
    unittest.main()
