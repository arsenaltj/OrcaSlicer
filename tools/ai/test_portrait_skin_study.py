import unittest
import numpy as np
from portrait_skin_study import skin_plan


class SkinStudyTests(unittest.TestCase):
    def test_shared_skin_preserves_lips_hair_and_ambiguous_piece(self):
        names=['face','nose','neck','lr','ulip','hair']
        rgb=np.tile([.6,.4,.3],(10,1));areas=np.ones(10)
        p=skin_plan(rgb,areas,[1,1,2,3,4,5,6,7,7,8],[0,0,1,2,3,4,5,0,5,-1],names)
        self.assertEqual(p['pieces'],[1,2,3,4])
    def test_dark_skin_not_replaced_by_light_skin_prior(self):
        rgb=np.tile([.24,.12,.07],(10,1));rgb[0]=0;rgb[-1]=1
        p=skin_plan(rgb,np.ones(10),np.ones(10),np.zeros(10,dtype=int),['face'])
        np.testing.assert_allclose(p['target_rgb'],[.24,.12,.07])
    def test_multiface_or_missing_guidance_noop(self):
        self.assertEqual(skin_plan([[.5]*3],[1],[1],[0],['face','face'])['pieces'],[])
        self.assertEqual(skin_plan([[.5]*3],[1],[1],[-1],['face'])['pieces'],[])
    def test_area_weighted_target_independent_of_triangle_density(self):
        p=skin_plan([[.2]*3,[.6]*3,[.9]*3],[1,8,1],[1]*3,[0]*3,['face'])
        q=skin_plan([[.2]*3]+[[.6]*3]*8+[[.9]*3],[1]*10,[1]*10,[0]*10,['face'])
        np.testing.assert_allclose(p['target_rgb'],q['target_rgb'])
    def test_invalid_arrays_rejected(self):
        with self.assertRaises(ValueError):skin_plan([[float('nan')]*3],[1],[1],[0],['face'])


if __name__=='__main__':unittest.main()
