import unittest

import numpy as np

from portrait_parent_projection import (SAMPLES, cell_sample_support, coverage_labels,
                                        full_sample_support, independent_votes, root_votes)


class ContinuousParentProjection(unittest.TestCase):
    def test_subpixel_triangle_receives_exact_area_support(self):
        labels=np.zeros((4,4),np.uint8);labels[1,1]=3
        category,coverage=coverage_labels(np.array([[[1.1,1.1],[1.3,1.1],[1.1,1.3]]]),labels,
                                         (labels>0).astype(float))
        self.assertEqual(category[0],3)
        self.assertEqual(coverage[0],1.)

    def test_mixed_triangle_does_not_become_whole_skin(self):
        labels=np.full((4,4),3,np.uint8);labels[:,2:]=4
        category,_=coverage_labels(np.array([[[1,1],[3,1],[1,3]]]),labels,np.ones(labels.shape))
        self.assertEqual(category[0],0)

    def test_projection_cannot_claim_an_occluded_source_face(self):
        camera=dict(center=[0,0,0],basis=np.eye(3).tolist(),half_height=2,width=4,height=4)
        vertices=np.array([[0,0,0],[.2,0,0],[0,.2,0]])
        faces=np.array([[0,1,2]])
        labels=np.full((4,4),3,np.uint8)
        category,_=root_votes(vertices,faces,camera,labels,np.ones(labels.shape),np.array([0],np.uint8),
                              np.array([True]))
        self.assertEqual(category[0],255)
        category,_=root_votes(vertices,faces,camera,labels,np.ones(labels.shape),np.array([127],np.uint8),
                              np.array([True]))
        self.assertEqual(category[0],3)

    def test_crop_refines_the_same_vote_without_doubling_support(self):
        category,conflicts,_=independent_votes([('front',1.,np.array([3,3],np.uint8)),
            ('front',2.,np.array([4,3],np.uint8))])
        np.testing.assert_array_equal(category,[0,0])
        np.testing.assert_array_equal(conflicts,[False,False])
        category,conflicts,_=independent_votes([('front',1.,np.array([3,3],np.uint8)),
            ('front',2.,np.array([4,3],np.uint8)),('side',1.,np.array([4,4],np.uint8))])
        np.testing.assert_array_equal(category,[4,0])
        np.testing.assert_array_equal(conflicts,[False,True])

    def test_partial_visibility_does_not_take_the_occluders_class(self):
        camera=dict(center=[0,0,0],basis=np.eye(3).tolist(),half_height=2,width=4,height=4)
        vertices=np.array([[0,0,0],[.2,0,0],[0,.2,0]])
        faces=np.array([[0,1,2]])
        labels=np.full((4,4),1,np.uint8)
        visibility=np.array([8],np.uint8)
        occluder=np.full(labels.shape,7,np.int64)
        category,_=root_votes(vertices,faces,camera,labels,np.ones(labels.shape),visibility,
                              np.array([True]),occluder)
        self.assertEqual(category[0],0)
        target=np.zeros(labels.shape,np.int64)
        category,_=root_votes(vertices,faces,camera,labels,np.ones(labels.shape),visibility,
                              np.array([True]),target)
        self.assertEqual(category[0],1)
        category,conflicts,_=independent_votes([('front',1.,np.array([3],np.uint8)),
            ('oblique',1.,np.array([3],np.uint8)),('side',1.,category)])
        self.assertEqual(category[0],0)
        self.assertTrue(conflicts[0])

    def test_fully_visible_subpixel_support_does_not_require_a_raster_center(self):
        camera=dict(center=[0,0,0],basis=np.eye(3).tolist(),half_height=2,width=4,height=4)
        vertices=np.array([[.1,.1,0],[.2,.1,0],[.1,.2,0]])
        faces=np.array([[0,1,2]])
        labels=np.full((4,4),3,np.uint8)
        category,_=root_votes(vertices,faces,camera,labels,np.ones(labels.shape),np.array([127],np.uint8),
                              np.array([True]),np.full(labels.shape,7,np.int64))
        self.assertEqual(category[0],3)

    def test_unknown_visible_target_samples_stay_in_the_support_denominator(self):
        camera=dict(center=[0,0,0],basis=np.eye(3).tolist(),half_height=4,width=8,height=8)
        vertices=np.array([[-2,2,0],[2.5,2,0],[-2,-2.5,0]])
        faces=np.array([[0,1,2]])
        labels=np.full((8,8),3,np.uint8)
        labels[2,6]=0
        parent,_=coverage_labels(np.array([[[2,2],[6.5,2],[2,6.5]]]),labels,np.ones(labels.shape))
        self.assertEqual(parent[0],3)
        raster=np.full(labels.shape,7,np.int64);raster[2,2]=0;raster[2,6]=0
        category,_=root_votes(vertices,faces,camera,labels,np.ones(labels.shape),np.array([6],np.uint8),
                              np.array([True]),raster)
        self.assertEqual(category[0],0)

    def test_partial_views_do_not_prove_the_entire_target(self):
        rows=[('front',1.,np.array([3,3,0],np.uint8)),
              ('side',1.,np.array([3,3,3],np.uint8))]
        category,_,_=independent_votes(rows)
        category[~full_sample_support(rows,np.array([[8,127,127],[16,8,127]],np.uint8),category)]=0
        np.testing.assert_array_equal(category,[0,3,0])
        with self.assertRaises(ValueError):
            full_sample_support(rows,np.array([[127]],np.uint8),category)

    def test_partial_root_uses_all_target_pixels_instead_of_only_sample_hits(self):
        camera=dict(center=[0,0,0],basis=np.eye(3).tolist(),half_height=8,width=16,height=16)
        xy=np.array([[.1,.1],[15.9,.1],[.1,15.9]])
        vertices=np.column_stack((xy[:,0]-8,8-xy[:,1],np.zeros(3)))
        faces=np.array([[0,1,2]])
        labels=np.full((16,16),3,np.uint8)
        raster=np.full(labels.shape,7,np.int64)
        raster[5:7,5:9]=0;labels[5:7,5:9]=1;labels[5,5]=3
        parent,_=coverage_labels(xy[None,:,:],labels,np.ones(labels.shape))
        self.assertEqual(parent[0],3)
        category,_=root_votes(vertices,faces,camera,labels,np.ones(labels.shape),np.array([1],np.uint8),
                              np.array([True]),raster)
        self.assertEqual(category[0],0)

    def test_full_support_cannot_borrow_a_replaced_or_different_parent_vote(self):
        rows=[('front',1.,np.array([1,3],np.uint8)),('front',2.,np.array([0,255],np.uint8)),
              ('left',1.,np.array([3,3],np.uint8)),('right',1.,np.array([3,3],np.uint8))]
        category,_,_=independent_votes(rows)
        np.testing.assert_array_equal(category,[3,3])
        complete=full_sample_support(rows,np.array([[127,127],[127,0],[1,1],[1,1]],np.uint8),category)
        np.testing.assert_array_equal(complete,[False,True])

    def test_partial_cell_witness_must_belong_to_the_target_cell(self):
        camera=dict(center=[0,0,0],basis=np.eye(3).tolist(),half_height=2,width=4,height=4)
        triangle=np.array([[0,0,0],[.2,0,0],[0,.2,0]])
        labels=np.full((4,4),3,np.uint8)
        raster=np.zeros(labels.shape,np.int64)
        cell=dict(polygon=[[1,0,0],[.5,.5,0],[.5,0,.5]],holes=[])
        bary=np.zeros((*labels.shape,3));bary[:]=[0,1,0]
        self.assertFalse(cell_sample_support(SAMPLES,triangle,camera,labels,np.ones(labels.shape),8,'skin',
                                            raster,0,bary,cell))
        bary[:]=[.75,.125,.125]
        self.assertTrue(cell_sample_support(SAMPLES,triangle,camera,labels,np.ones(labels.shape),8,'skin',
                                           raster,0,bary,cell))
        cell['holes']=[[[.9,.05,.05],[.6,.35,.05],[.6,.05,.35]]]
        self.assertFalse(cell_sample_support(SAMPLES,triangle,camera,labels,np.ones(labels.shape),8,'skin',
                                            raster,0,bary,cell))

    def test_in_frame_uncertain_crop_vetoes_coarse_but_outside_crop_does_not(self):
        category,_,_=independent_votes([('front',1.,np.array([3,3],np.uint8)),
            ('front',2.,np.array([0,255],np.uint8)),('side',1.,np.array([3,3],np.uint8))])
        np.testing.assert_array_equal(category,[0,3])

    def test_body_skin_is_not_confused_with_clothing(self):
        labels=np.full((4,4),2,np.uint8)
        category,_=coverage_labels(np.array([[[1,1],[2,1],[1,2]]]),labels,np.ones(labels.shape))
        self.assertEqual(category[0],3)

    def test_one_view_and_low_quality_keep_source_appearance(self):
        labels=np.full((4,4),3,np.uint8)
        category,_=coverage_labels(np.array([[[1,1],[2,1],[1,2]]]),labels,np.full(labels.shape,.89))
        self.assertEqual(category[0],0)
        category,_,_=independent_votes([('front',1.,np.array([3],np.uint8))])
        self.assertEqual(category[0],0)


if __name__=='__main__':unittest.main()
