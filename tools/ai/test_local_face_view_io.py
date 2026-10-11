import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest

import numpy as np
from local_face_view_io import load_views


class FaceViewIOTests(unittest.TestCase):
    def test_keeps_best_observation_per_real_camera_family(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for index, (family, quality) in enumerate((("front", 0.5), ("front", 0.9), ("side", 0.7))):
                metadata = {"family": family, "quality": quality, "parts": [], "irises": [],
                            "boundary": False, "scale": 1, "pixel_size": 1}
                np.savez(root / f"{index}.npz", metadata=json.dumps(metadata),
                         **{name: np.array([index]) for name in ("points", "world", "valid", "visible", "counts", "head")})
            def view(*args):
                return SimpleNamespace(family=args[0], points=args[1], quality=args[-2], boundary=args[-1])
            result = load_views(root, view_factory=view, boundary_factory=lambda *args: args)
            self.assertEqual([(item.family, item.quality) for item in result], [("front", 0.9), ("side", 0.7)])
            self.assertEqual(result[0].points.tolist(), [1])


if __name__ == "__main__":
    unittest.main()
