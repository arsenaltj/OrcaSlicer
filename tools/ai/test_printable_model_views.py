#!/usr/bin/env python3
import json
import tempfile
import unittest
from pathlib import Path
import sys

from PIL import Image

TOOLS_AI = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS_AI))
from printable_model_views import ModelViewSettings, render_model_views


COLORED_TETRAHEDRON = """\
v 0 0 0 1 0 0
v 10 0 0 0 1 0
v 0 10 0 0 0 1
v 0 0 10 1 1 1
f 1 3 2
f 1 2 4
f 1 4 3
f 2 3 4
"""


class PrintableModelViewTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.source = self.root / "model.obj"
        self.source.write_text(COLORED_TETRAHEDRON, encoding="ascii")

    def tearDown(self):
        self.temporary.cleanup()

    def test_renders_five_views_sheet_and_manifest(self):
        result = render_model_views(self.source, self.root, ModelViewSettings(width=160, height=160))

        self.assertEqual(len(result["views"]), 5)
        self.assertEqual(result["face_count"], 4)
        self.assertFalse(result["cached"])
        for relative in result["views"]:
            self.assertTrue((self.root / relative).is_file())
        with Image.open(self.root / result["sheet"]) as sheet:
            self.assertGreater(sheet.width, sheet.height)
        manifest = json.loads((self.root / "model-views" / "manifest.json").read_text(encoding="utf-8"))
        self.assertEqual(manifest["obj_sha256"], result["obj_sha256"])

    def test_reuses_matching_cached_views(self):
        settings = ModelViewSettings(width=128, height=128)
        render_model_views(self.source, self.root, settings)
        cached = render_model_views(self.source, self.root, settings)
        self.assertTrue(cached["cached"])

    def test_obj_change_invalidates_cache(self):
        settings = ModelViewSettings(width=128, height=128)
        first = render_model_views(self.source, self.root, settings)
        self.source.write_text(COLORED_TETRAHEDRON.replace("v 0 0 10", "v 0 0 12"), encoding="ascii")
        second = render_model_views(self.source, self.root, settings)
        self.assertFalse(second["cached"])
        self.assertNotEqual(first["obj_sha256"], second["obj_sha256"])


if __name__ == "__main__":
    unittest.main()
