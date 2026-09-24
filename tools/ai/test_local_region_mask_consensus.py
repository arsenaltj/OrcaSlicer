import unittest
import numpy as np
from local_region_mask_consensus import associate_masks


class MaskConsensusTests(unittest.TestCase):
    def setUp(self):
        self.patch=np.array([0,1,2,3]);self.area=np.ones(4);self.protected=np.zeros(4,bool)
        self.neighbors=np.array([[1,-1,-1],[0,2,-1],[1,3,-1],[2,-1,-1]])
    @staticmethod
    def mask(view,patches):
        return dict(view=view,patches=patches,coverage=[1.]*len(patches),visible_fraction=[1.]*len(patches),score=.99)
    def run_case(self,records,links):
        return associate_masks(records,links,self.area,self.protected,self.neighbors,self.patch)[0]
    def test_two_independent_views_form_connected_region(self):
        records=[self.mask(0,[0,1]),self.mask(1,[0,1])]
        np.testing.assert_array_equal(self.run_case(records,[dict(a=0,b=1,shared_iou=.9)]),[0,0,-1,-1])
    def test_same_view_masks_cannot_be_two_votes(self):
        records=[self.mask(0,[0,1]),self.mask(0,[0,1])]
        np.testing.assert_array_equal(self.run_case(records,[dict(a=0,b=1,shared_iou=.9)]),[-1]*4)
    def test_fine_face_remains_outside_candidate(self):
        self.protected[1]=True;records=[self.mask(0,[0,1]),self.mask(1,[0,1])]
        np.testing.assert_array_equal(self.run_case(records,[dict(a=0,b=1,shared_iou=.9)]),[0,-1,-1,-1])
    def test_disconnected_shells_get_different_ids(self):
        self.neighbors[1,1]=-1;self.neighbors[2,0]=-1
        records=[self.mask(0,[0,1,2,3]),self.mask(1,[0,1,2,3])]
        np.testing.assert_array_equal(self.run_case(records,[dict(a=0,b=1,shared_iou=.9)]),[0,0,1,1])
    def test_conflicting_similar_region_abstains(self):
        records=[self.mask(0,[0,1]),self.mask(1,[0,1]),self.mask(2,[1,2]),self.mask(3,[1,2])]
        links=[dict(a=0,b=1,shared_iou=.9),dict(a=2,b=3,shared_iou=.9)]
        np.testing.assert_array_equal(self.run_case(records,links),[0,-1,1,-1])
    def test_partial_visibility_does_not_vote(self):
        records=[self.mask(0,[0]),self.mask(1,[0])];records[1]['visible_fraction']=[.01]
        np.testing.assert_array_equal(self.run_case(records,[dict(a=0,b=1,shared_iou=.9)]),[-1]*4)

    def test_roundoff_above_one_preserves_identical_view_association(self):
        records=[self.mask(0,[0,1]),self.mask(1,[0,1])]
        exact=self.run_case(records,[dict(a=0,b=1,shared_iou=1.)])
        rounded=self.run_case(records,[dict(a=0,b=1,shared_iou=np.nextafter(1.,np.inf))])
        np.testing.assert_array_equal(rounded,exact)

    def test_non_roundoff_invalid_view_association_still_rejected(self):
        records=[self.mask(0,[0,1]),self.mask(1,[0,1])]
        for value in (1.000000001,-.001,float('nan')):
            with self.subTest(value=value),self.assertRaises(ValueError):
                self.run_case(records,[dict(a=0,b=1,shared_iou=value)])

    def test_isolated_tiny_fragment_becomes_unknown(self):
        self.area=np.array([10000.,10000.,10000.,.1]);self.neighbors[2,1]=-1;self.neighbors[3,0]=-1
        records=[self.mask(0,[0,1,3]),self.mask(1,[0,1,3])]
        np.testing.assert_array_equal(self.run_case(records,[dict(a=0,b=1,shared_iou=.9)]),[0,0,-1,-1])


if __name__=='__main__':unittest.main()
