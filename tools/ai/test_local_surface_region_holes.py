import unittest

import numpy as np

from local_surface_region_holes import fill_enclosed_holes


class EnclosedSurfaceHoleTests(unittest.TestCase):
    def setUp(self):
        # Four faces of a closed tetrahedron: face 3 is surrounded by hair.
        self.patch = np.arange(4, dtype=np.int32)
        self.neighbors = np.array([[1, 2, 3], [0, 2, 3], [0, 1, 3], [0, 1, 2]], dtype=np.int32)
        self.area = np.array([100., 100., 100., 1.])
        self.rgb = np.full((4, 3), .1, dtype=np.float32)
        self.labels = np.array([0, 0, 0, -1], dtype=np.int32)
        self.proposal = np.array([1, 1, 1, -1], dtype=np.int32)
        self.names = ["hair", "skin"]

    def run_case(self):
        return fill_enclosed_holes(self.patch, self.neighbors, self.area, self.rgb,
                                   self.labels, self.names, self.proposal, 1, "hair")

    def test_matching_enclosed_unknown_joins_one_edit_region(self):
        result, evidence = self.run_case()
        np.testing.assert_array_equal(result, [1, 1, 1, 1])
        np.testing.assert_array_equal(self.proposal, [1, 1, 1, -1])
        self.assertEqual(evidence["filled_holes"], 1)

    def test_open_skin_competing_and_contrast_faces_remain_untouched(self):
        for kind in ("open", "skin", "competing", "contrast"):
            with self.subTest(kind=kind):
                self.setUp()
                if kind == "open":
                    self.neighbors[3, 0] = -1
                elif kind == "skin":
                    self.labels[3] = 1
                elif kind == "competing":
                    self.proposal[3] = 2
                else:
                    self.rgb[3] = .9
                result, evidence = self.run_case()
                np.testing.assert_array_equal(result, self.proposal)
                self.assertEqual(evidence["filled_patches"], 0)


if __name__ == "__main__":
    unittest.main()
