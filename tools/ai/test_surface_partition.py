import copy
import unittest
from types import SimpleNamespace

import numpy as np

from local_brow_boundary import Projection
from local_face_landmarks import EYEBROWS
from local_eye_landmarks import EYE_BY_LABEL
from local_leaf_boundaries import source_brow_contour
from local_surface_contours import eye_accessory, iris_curve
from portrait_r7_replay import topology_scope
from surface_partition import inside, pixel_cells, color_plan, apply


class SurfacePartitionTests(unittest.TestCase):
    def test_narrow_continuous_cells_not_midpoint_majority(self):
        strip=[[.65,.25,.10],[.64,.26,.10],[.14,.26,.60],[.15,.25,.60]]
        points=np.array([[.40,.255,.345],[.40,.27,.33],[.74,.255,.005]])
        np.testing.assert_array_equal(inside(strip,points),[True,False,False])

    def test_holes_remove_iris_from_eye_white(self):
        eye=[[1,0,0],[0,1,0],[0,0,1]]
        iris=[[.6,.2,.2],[.5,.3,.2],[.5,.2,.3]]
        p=np.array([[.55,.225,.225],[.8,.1,.1]])
        self.assertTrue(inside(eye,p).all())
        np.testing.assert_array_equal(inside(eye,p)&~inside(iris,p),[False,True])

    def test_ring_limits_and_hard_blocked_roots(self):
        neighbors=np.array([[1,-1,-1],[0,2,-1],[1,3,-1],[2,4,-1],[3,-1,-1]])
        self.assertEqual(topology_scope({0},neighbors,set(range(5)),set(),2),{0,1,2})
        self.assertEqual(topology_scope({0},neighbors,set(range(5)),{2},2),{0,1})
        self.assertEqual(topology_scope({0},neighbors,set(range(5)),{1},2),{0})

    def test_all_legitimate_brow_seed_components_are_preserved(self):
        rgb=np.full((64,100,3),[230,185,165],dtype=np.uint8)
        rgb[24:31,20:48]=[40,32,28]
        rgb[24:29,56:81]=[40,32,28]
        ids=np.full((64,100),2,dtype=int)
        ids[24:31,20:48]=0; ids[24:29,56:81]=1
        projection=Projection(rgb,ids,np.zeros((64,100,3)),np.zeros((64,100,2)),
            np.ones((64,100),dtype=bool),np.zeros(2),np.zeros((478,2)))
        band=np.array([[18,22],[82,22],[82,33],[18,33]],dtype=float)
        curves,audit=source_brow_contour(projection,{0,1},{0,1},{2},{0,1,2},band,all_components=True)
        self.assertEqual(audit['retained_contours'],2)
        self.assertEqual(len(curves),2)
        old,_=source_brow_contour(projection,{0,1},{0,1},{2},{0,1,2},band)
        self.assertIsInstance(old,np.ndarray)

    def landmarks(self):
        p=np.zeros((478,2))
        upper=np.column_stack((np.linspace(20,80,9),30-8*np.sin(np.linspace(0,np.pi,9))))
        lower=np.column_stack((np.linspace(20,80,9),30+8*np.sin(np.linspace(0,np.pi,9))))
        contour=np.vstack((upper,lower[-2:0:-1]))
        indices,center,rim=EYE_BY_LABEL['le']
        p[indices]=contour; p[center]=[50,30]
        p[rim]=[[60,30],[50,40],[40,30],[50,20]]
        return p

    def test_ellipse_curve_is_side_specific_and_chord_error_is_bounded(self):
        p=self.landmarks()
        curve,report=iris_curve(p,'le',1.)
        radial=((curve[:,0]-50)/10)**2+((curve[:,1]-30)/10)**2
        np.testing.assert_allclose(radial,1.,atol=1e-10)
        chord=40*(1-np.cos(np.pi/len(curve)))
        self.assertLessEqual(chord,.20)
        with self.assertRaises(ValueError): iris_curve(p,'re',1.)

    def test_eye_line_requires_source_pigment_and_does_not_draw_a_black_ring(self):
        rgb=np.full((64,100,3),[230,185,165],dtype=np.uint8)
        p=self.landmarks()
        projection=Projection(rgb,np.zeros((64,100),dtype=int),np.zeros((64,100,3)),np.zeros((64,100,2)),
            np.ones((64,100),dtype=bool),np.zeros(2),p)
        with self.assertRaisesRegex(ValueError,'NO_SOURCE_SUPPORTED'):
            eye_accessory(projection,'le',{0})
        rgb[20:24,39:62]=[30,26,26]
        curves,holes,audit=eye_accessory(projection,'le',{0})
        self.assertFalse(audit['black_ring_drawn'])
        self.assertGreater(len(curves),0)

    def test_source_eye_ring_preserves_the_uncolored_white_hole(self):
        p=self.landmarks()
        rgb=np.full((64,100,3),[230,185,165],dtype=np.uint8)
        cv2=__import__('cv2')
        aperture=np.zeros((64,100),dtype=np.uint8)
        from local_face_landmarks import polygon_mask
        from local_leaf_boundaries import smooth_lid
        aperture[:]=polygon_mask(smooth_lid(p[EYE_BY_LABEL['le'][0]]),aperture.shape)
        exterior=cv2.dilate(aperture,np.ones((3,3),np.uint8)).astype(bool)
        rgb[exterior & ~aperture.astype(bool)]=[30,26,26]
        projection=Projection(rgb,np.zeros((64,100),dtype=int),np.zeros((64,100,3)),np.zeros((64,100,2)),
            np.ones((64,100),dtype=bool),np.zeros(2),p)
        curves,holes,audit=eye_accessory(projection,'le',{0})
        self.assertGreater(audit['source_holes'],0)
        center=np.array([50,30])*1024/100
        self.assertTrue(any(cv2.pointPolygonTest(c.astype(np.float32),tuple(center),False)>0 for c in holes))

    def test_manual_color_priority_and_non_target_inheritance(self):
        palette=[{'uid':'portrait-skin','rgb':[1,.8,.7]},
                 {'uid':'portrait-dark','rgb':[.1,.1,.1]}, {'uid':'portrait-light','rgb':[1,1,1]}]
        cell=dict(id='x',label='periocular-le',kind='SOURCE_RESTORED_EYE_LINE')
        document=dict(geometry_id='a',source_sha256='b',partition_sha256='c',
                      faces=[dict(source_face_id=0,status='CONTOUR_CLIPPED',cells=[cell])])
        plan=color_plan(document,palette)
        old=np.full((1,3,3),200,dtype=np.uint8)
        mapping=np.array([[0,-1,0]])
        manual=np.array([[False,False,True]])
        rgb=np.full_like(old,[1,2,3])
        new=apply(old,mapping,[cell],plan,np.zeros((1,3),dtype=int),manual=(manual,rgb))
        np.testing.assert_array_equal(new[0,0],[26,26,26])
        np.testing.assert_array_equal(new[0,1],old[0,1])
        np.testing.assert_array_equal(new[0,2],[1,2,3])

    def test_oral_passthrough_never_gets_an_automatic_palette_slot(self):
        palette=[{'uid':'portrait-skin','rgb':[1,.8,.7]},
                 {'uid':'portrait-dark','rgb':[.1,.1,.1]}, {'uid':'portrait-light','rgb':[1,1,1]},
                 {'uid':'portrait-lips','rgb':[.8,.4,.4]}]
        cell=dict(id='oral',label='imouth',kind='R6_ORAL_PASSTHROUGH')
        document=dict(geometry_id='a',source_sha256='b',partition_sha256='c',
                      faces=[dict(source_face_id=0,status='CONTOUR_CLIPPED',cells=[cell])])
        plan=color_plan(document,palette)
        self.assertIsNone(plan['cells'][0]['slot_uid'])
        original=np.array([[[163,84,85]]],dtype=np.uint8)
        np.testing.assert_array_equal(apply(original,np.array([[0]]),[cell],plan,np.zeros((1,1),dtype=int)),original)

    def test_permission_leaf_edges_are_reported_as_retained_boundaries(self):
        from portrait_r7_audit import follows_permission_edge
        polygon=dict(polygon=[[1,0,0],[.5,.5,0],[.5,0,.5]],holes=[])
        layers=[dict(parent_polygons=[polygon])]
        self.assertTrue(follows_permission_edge(np.array([.5,.25,.25]),np.array([.5,.4,.1]),layers))
        self.assertFalse(follows_permission_edge(np.array([.6,.2,.2]),np.array([.7,.2,.1]),layers))


if __name__=='__main__': unittest.main()
