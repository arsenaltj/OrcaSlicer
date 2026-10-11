import shutil
import tempfile
from pathlib import Path
import unittest

from scripts.verify_ai_integration import validate_atomic_capabilities, validate_native_model_capabilities
from tools.ai.capability_catalog import ATOMIC_MODULES, CAPABILITIES


ROOT = Path(__file__).resolve().parents[1]


class AtomicArchitectureTests(unittest.TestCase):
    def test_current_capability_graph_and_packaging_are_valid(self):
        self.assertEqual(validate_atomic_capabilities(ROOT), [])
        self.assertEqual(validate_native_model_capabilities(ROOT), [])

    def test_native_ui_leak_and_missing_build_component_fail_closed(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = ROOT / "src/slic3r/GUI/AI"
            shutil.copytree(source, root / "src/slic3r/GUI/AI")
            shutil.copyfile(ROOT / "src/slic3r/CMakeLists.txt", root / "src/slic3r/CMakeLists.txt")
            session = root / "src/slic3r/GUI/AI/ColorMatching/ColorMatchingSession.hpp"
            session.write_text(session.read_text(encoding="utf-8") + '\n#include "slic3r/GUI/ModelGenerationPanel.hpp"\n#include "wx/panel.h"\n', encoding="utf-8")
            component = root / "src/slic3r/GUI/AI/ModelGeneration/ModelWorkspaceSources.cmake"
            component.write_text(component.read_text(encoding="utf-8").replace("GUI/AI/ColorMatching/ColorMatchingSession.hpp", ""), encoding="utf-8")
            codes = {error["code"] for error in validate_native_model_capabilities(root)}
            self.assertIn("capability.legacy_panel", codes)
            self.assertIn("capability.native_ui", codes)
            self.assertIn("capability.native_package", codes)

    def test_host_dependency_and_missing_package_fail_closed(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            paths = {"tools/ai/" + name + ".py" for name in ATOMIC_MODULES}
            paths.update(item.source for item in CAPABILITIES)
            paths.update({"tools/ai/capability_catalog.py", "tools/ai/model_runtime_files.cmake", "CMakeLists.txt"})
            for name in paths:
                source = ROOT / name
                target = root / name
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(source, target)
            source = root / "tools/ai/model_request.py"
            source.write_text(source.read_text(encoding="utf-8") + "\nimport orca_ai_sidecar\n", encoding="utf-8")
            runtime = root / "tools/ai/model_runtime_files.cmake"
            runtime.write_text(runtime.read_text(encoding="utf-8").replace('"${CMAKE_SOURCE_DIR}/tools/ai/model_request.py"', ''), encoding="utf-8")
            errors = validate_atomic_capabilities(root)
            self.assertIn("capability.host_dependency", {item["code"] for item in errors})
            self.assertIn("capability.package", {item["code"] for item in errors})


if __name__ == "__main__":
    unittest.main()
