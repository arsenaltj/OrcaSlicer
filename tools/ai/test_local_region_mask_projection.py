import unittest
import numpy as np
from local_region_mask_projection import project_masks


class MaskProjectionTests(unittest.TestCase):
    def setUp(self):
        self.patch=np.array([0,0,1,2]);self.area=np.array([1.,1.,2.,1.])
        self.ids=np.array([[0,0,1,2,3,-1]])
    def run_masks(self,masks):
        return project_masks(self.ids,np.array(masks,bool),self.patch,self.area,np.ones(len(masks)))
    def test_duplicate_pixels_do_not_outvote_other_faces(self):
        self.assertEqual(self.run_masks([[[1,1,0,0,0,0]]]),[])
    def test_partial_visibility_is_not_full_patch_evidence(self):
        self.ids[0,2]=-1
        r=self.run_masks([[[1,1,0,0,0,0]]])[0]
        np.testing.assert_array_equal(r['patches'],[0]);self.assertEqual(r['visible_fraction'][0],.5)
    def test_overlapping_masks_keep_separate_identity(self):
        r=self.run_masks([[[1,1,1,0,0,0]],[[1,1,1,1,0,0]]])
        self.assertEqual([v['mask_id'] for v in r],[0,1]);np.testing.assert_array_equal(r[1]['patches'],[0,1])
    def test_whole_object_and_background_not_internal_parts(self):
        self.assertEqual(self.run_masks([[[1,1,1,1,1,0]],[[0,0,0,0,0,1]]]),[])
    def test_invalid_face_binding_rejected(self):
        self.ids[0,0]=4
        with self.assertRaises(ValueError):self.run_masks([[[1,1,1,0,0,0]]])
    def test_empty_view_has_no_evidence(self):
        self.ids[:]=-1;self.assertEqual(self.run_masks([[[1,1,1,0,0,0]]]),[])


if __name__=='__main__':unittest.main()
