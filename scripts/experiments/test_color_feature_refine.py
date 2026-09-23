import unittest
import numpy as np
from color_feature_refine import smooth_surface_signal, detail_colors, refine_details


class FeatureRefineTests(unittest.TestCase):
    def filter(self,rgb,active=None,distance=.1):
        rgb=np.array(rgb,float)
        centers=np.array([[0,0,0],[distance,0,0]],float)
        return smooth_surface_signal(rgb,centers,np.array([[0,0,1]]*2),np.ones(2),
                                     np.array([0]),np.array([1]),np.ones(2,bool) if active is None else active)

    def test_constant_field_and_input_are_preserved(self):
        rgb=np.array([[180,140,110]]*2,float);saved=rgb.copy()
        np.testing.assert_allclose(self.filter(rgb),rgb)
        np.testing.assert_array_equal(rgb,saved)

    def test_separate_windows_and_long_edges_do_not_bleed(self):
        rgb=[[100,80,70],[140,110,90]]
        np.testing.assert_allclose(self.filter(rgb,np.array([True,False])),rgb)
        np.testing.assert_allclose(self.filter(rgb,distance=2),rgb)

    def test_small_noise_is_reduced_without_global_average(self):
        result=self.filter([[120,100,90],[140,120,110]])
        self.assertGreater(result[0,0],120)
        self.assertLess(result[0,0],130)
        self.assertGreater(result[1,0],130)

    def test_high_contrast_boundary_does_not_average_black_and_white(self):
        rgb=np.array([[0,0,0],[255,255,255]],float)
        np.testing.assert_allclose(self.filter(rgb),rgb,atol=.1)

    def test_eye_keeps_black_iris_white_sclera_and_warm_skin(self):
        rgb=np.array([[35,33,40],[85,78,74],[185,175,165],[215,171,143]],float)
        np.testing.assert_array_equal(detail_colors('eye',rgb),[1,5,2,0])

    def test_brows_and_mouth_do_not_become_solid_black(self):
        np.testing.assert_array_equal(detail_colors('brow',np.array([[150,115,95],[215,171,143]],float)),[5,0])
        rgb=np.array([[175,86,75],[230,217,205],[45,25,25],[216,169,143]],float)
        np.testing.assert_array_equal(detail_colors('mouth',rgb),[3,2,1,0])

    def test_refinement_cannot_repaint_clothes_or_edit_geometry(self):
        v=np.array([[0,0,0],[1,0,0],[0,1,0],[1,1,0]],float)
        f=np.array([[0,1,2],[1,3,2]])
        rgb=np.array([[150,115,95],[150,115,95]],float)
        old=np.array([1,2],np.uint8);saved=v.copy()
        output,stats=refine_details(v,f,rgb,old,{'brow':np.array([True,False])},
            {'features':[{'name':'brow','kind':'brow'}]},np.array([0]),np.array([1]))
        np.testing.assert_array_equal(output,[5,2])
        np.testing.assert_array_equal(v,saved)
        np.testing.assert_array_equal(old,[1,2])
        self.assertTrue(stats['outside_feature_windows_unchanged'])


if __name__=='__main__':unittest.main()
