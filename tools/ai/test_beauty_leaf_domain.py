import unittest
import numpy as np
from beauty_leaf_domain import LeafKey, domain, locate, validate_keys


class LeafDomainTests(unittest.TestCase):
    def test_full_partition_and_partial_lock(self):
        leaves = LeafKey(0).children()
        self.assertEqual(len(domain('a'*64, 200, leaves)['split_leaves']), 4)
        self.assertEqual(locate(leaves, 0, [.1,.1,.8]), leaves[2])
        self.assertEqual(locate(leaves, 1, [.1,.1,.8]), LeafKey(1))

    def test_gaps_overlap_range_budget_and_hit(self):
        leaves = LeafKey(0).children()
        for value in (leaves[:-1], sorted(leaves+[LeafKey(0,2,0)]), leaves+leaves):
            with self.assertRaises(ValueError):
                domain('a'*64, 200, value)
        with self.assertRaises(ValueError):
            domain('a'*64, 100, leaves)
        with self.assertRaises(ValueError):
            validate_keys([LeafKey(0,5,0)], 200)
        with self.assertRaises(ValueError):
            locate(leaves, 0, [1,1,1])

    def test_native_corner_two_and_depth_four(self):
        np.testing.assert_array_equal(LeafKey(0,1,2).corners(), [[0,.5,.5],[0,0,1],[.5,0,.5]])
        for path in range(256):
            corners = LeafKey(0,4,path).corners()
            np.testing.assert_array_equal(corners.sum(1), np.ones(3))
            self.assertAlmostEqual(abs(np.linalg.det(corners)), 1/256)


if __name__ == '__main__':
    unittest.main()
