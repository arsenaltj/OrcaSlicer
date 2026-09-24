import unittest
import numpy as np
from color_cleanup_minimal import cleanup


def grid(spacing=.1):
    vertices = np.array([(x*spacing,y*spacing,0) for y in range(6) for x in range(6)], np.float32)
    faces = []
    for y in range(5):
        for x in range(5):
            a = y*6+x
            faces.extend([(a,a+1,a+7),(a,a+7,a+6)])
    return vertices, np.array(faces, np.int32)


class CleanupTests(unittest.TestCase):
    def test_enclosed_speckle_merges_without_mutating_source(self):
        vertices, faces = grid()
        labels = np.zeros(len(faces), np.uint8)
        labels[24:26] = 1
        before_v, before_f, before_l = vertices.copy(), faces.copy(), labels.copy()
        result, stats = cleanup(vertices, faces, labels)
        self.assertTrue(np.all(result == 0))
        self.assertEqual(stats['merged_regions'], 1)
        self.assertTrue(np.array_equal(vertices,before_v))
        self.assertTrue(np.array_equal(faces,before_f))
        self.assertTrue(np.array_equal(labels,before_l))

    def test_open_surface_edge_is_not_silently_erased(self):
        vertices, faces = grid()
        labels = np.zeros(len(faces), np.uint8)
        labels[:2] = 1
        result, _ = cleanup(vertices, faces, labels)
        self.assertTrue(np.array_equal(result,labels))

    def test_physical_area_prevents_erasing_larger_region(self):
        vertices, faces = grid(1.)
        labels = np.zeros(len(faces), np.uint8)
        labels[24:26] = 1
        result, _ = cleanup(vertices, faces, labels)
        self.assertTrue(np.array_equal(result,labels))


if __name__ == '__main__': unittest.main()
