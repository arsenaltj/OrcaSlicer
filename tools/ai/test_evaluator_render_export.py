"""Contract checks for the colleague-only RGB/normal export."""

import json
import tempfile
import unittest
from pathlib import Path

import numpy as np
from PIL import Image

from evaluator_render_export import BACKGROUND, export_evaluator_views
from test_local_semantic_render import fixture


class EvaluatorRenderExportTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        # A double-sided tetrahedron exercises all eight orbit views.
        fixture(self.root / "model.glb", [{
            "positions": [[0, 0, 0], [0.01, 0, 0], [0, 0.01, 0], [0, 0, 0.01]],
            "indices": [0, 2, 1, 0, 1, 3, 0, 3, 2, 1, 2, 3],
            "material": 0,
        }], materials=[{"doubleSided": True,
                       "pbrMetallicRoughness": {"baseColorFactor": [0.9, 0.3, 0.2, 1]}}])

    def test_eight_paired_opaque_views_and_manifest(self):
        output = self.root / "eval"
        result = export_evaluator_views(self.root / "model.glb", output)
        self.assertEqual(len(result["views"]), 8)
        self.assertEqual([item["azimuth_degrees"] for item in result["views"]], list(range(0, 360, 45)))
        self.assertEqual(len(list((output / "rgb_renders").iterdir())), 8)
        self.assertEqual(len(list((output / "normal_renders").iterdir())), 8)
        self.assertEqual(json.loads((output / "manifest.json").read_text())["source_sha256"], result["source_sha256"])
        for index in range(8):
            name = f"view_{index:02d}.png"
            with Image.open(output / "rgb_renders" / name) as rgb, Image.open(output / "normal_renders" / name) as normal:
                self.assertEqual((rgb.mode, rgb.size), ("RGB", (512, 512)))
                self.assertEqual((normal.mode, normal.size), ("RGB", (512, 512)))
                a, b = np.asarray(rgb), np.asarray(normal)
                self.assertTrue(np.all(a[np.all(b == 0, axis=2)] == BACKGROUND))
                self.assertTrue(np.any(np.any(b != 0, axis=2)))
                self.assertTrue(np.any(np.all(b == 0, axis=2)))

    def test_refuses_to_replace_existing_result(self):
        output = self.root / "eval"
        output.mkdir()
        marker = output / "keep.txt"
        marker.write_text("keep")
        with self.assertRaises(FileExistsError):
            export_evaluator_views(self.root / "model.glb", output)
        self.assertEqual(marker.read_text(), "keep")

    def test_rejects_nonopaque_source_without_partial_output(self):
        fixture(self.root / "invalid.glb", [{
            "positions": [[0, 0, 0], [0.01, 0, 0], [0, 0.01, 0]], "material": 0,
        }], materials=[{"alphaMode": "BLEND"}])
        output = self.root / "eval"
        with self.assertRaisesRegex(ValueError, "OPAQUE"):
            export_evaluator_views(self.root / "invalid.glb", output)
        self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
