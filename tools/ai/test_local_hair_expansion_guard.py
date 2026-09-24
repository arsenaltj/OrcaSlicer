import unittest
import numpy as np
from local_hair_expansion_guard import guard_expansion


class HairExpansionGuardTests(unittest.TestCase):
    def setUp(self):
        self.patch = np.arange(4, dtype=np.int32)
        self.area = np.ones(4, dtype=np.float64)
        self.base = np.array([7, -1, -1, -1], np.int32)
        self.expanded = np.array([7, 7, 7, -1], np.int32)

    @staticmethod
    def view(family, labels):
        ids = np.arange(4, dtype=np.int32)[None]
        scores = np.full((1, 4, 6), .02, np.float32)
        for i, label in enumerate(labels):
            scores[0, i, label] = .9
        return family, ids, scores

    def test_two_views_veto_only_new_cloth_expansion(self):
        observations = [self.view('front', [1, 4, 1, 4]),
                        self.view('side', [1, 4, 1, 4])]
        result, report = guard_expansion(self.patch, self.area, self.base,
                                         self.expanded, 7, observations)
        np.testing.assert_array_equal(result, [7, -1, 7, -1])
        self.assertEqual(report['removed_patches'], 1)

    def test_one_view_or_conflicting_hair_view_does_not_veto(self):
        one = [self.view('front', [1, 4, 1, 4])]
        mixed = [self.view('front', [1, 4, 1, 4]),
                 self.view('side', [1, 4, 1, 4]),
                 self.view('back', [1, 1, 1, 4])]
        for observations in (one, mixed):
            result, _ = guard_expansion(self.patch, self.area, self.base,
                                        self.expanded, 7, observations)
            np.testing.assert_array_equal(result, self.expanded)

    def test_correlated_or_invalid_scores_rejected(self):
        front = self.view('front', [1, 4, 1, 4])
        with self.assertRaises(ValueError):
            guard_expansion(self.patch, self.area, self.base, self.expanded, 7,
                            [front, front])
        broken = self.view('front', [1, 4, 1, 4])
        broken[2][0, 0, 1] = float('nan')
        with self.assertRaises(ValueError):
            guard_expansion(self.patch, self.area, self.base, self.expanded, 7,
                            [broken])


if __name__ == '__main__':
    unittest.main()
