#!/usr/bin/env python3
import importlib.util
import os
import tempfile
import unittest
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("orca_ai_sidecar_mock_tests", ROOT / "tools" / "ai_sidecar_mock.py")
MOCK_SIDECAR = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(MOCK_SIDECAR)


class MockArtifactTests(unittest.TestCase):
    def test_empty_palette_enables_natural_color_mode(self):
        self.assertEqual(MOCK_SIDECAR.normalize_palette([]), [])

    def test_default_artifact_remains_tiny_obj(self):
        with mock.patch.dict(os.environ, {}, clear=True):
            self.assertEqual(MOCK_SIDECAR._load_mock_obj(), MOCK_SIDECAR.TINY_OBJ)

    def test_configured_obj_is_loaded_verbatim(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "real.obj"
            payload = b"v 0 0 0 1 0 0\nf 1 1 1\n"
            path.write_bytes(payload)
            with mock.patch.dict(os.environ, {"ORCASLICER_AI_MOCK_OBJ_PATH": str(path)}):
                self.assertEqual(MOCK_SIDECAR._load_mock_obj(), payload)

    def test_invalid_configured_obj_path_fails_fast(self):
        with mock.patch.dict(os.environ, {"ORCASLICER_AI_MOCK_OBJ_PATH": "missing.obj"}):
            with self.assertRaisesRegex(RuntimeError, "could not be read"):
                MOCK_SIDECAR._load_mock_obj()


if __name__ == "__main__":
    unittest.main()
