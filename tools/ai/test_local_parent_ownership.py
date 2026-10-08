import unittest
from unittest.mock import patch
from types import SimpleNamespace
import numpy as np
from local_parent_ownership import build, bounded_scope, family_votes, mapped_labels, observe, whole_root_labels
from local_parent_projection import coverage_labels
from local_semantic_projection import LABEL_NAMES


class CurrentParentOwnership(unittest.TestCase):
    def test_correlated_crop_is_one_vote_and_unknown_remains_unknown(self):
        records=[dict(family='a',resolution=1,root_labels=np.array([3,3,0],np.uint8)),
                 dict(family='a',resolution=2,root_labels=np.array([3,255,0],np.uint8))]
        accepted,_,_=family_votes(records,3)
        self.assertFalse(accepted.any())
        records.append(dict(family='b',resolution=1,root_labels=np.array([3,4,0],np.uint8)))
        accepted,conflict,_=family_votes(records,3)
        np.testing.assert_array_equal(accepted,[3,0,0])
        self.assertTrue(conflict[1])

    def test_completion_cannot_chain_new_seeds_or_cross_barriers(self):
        neighbors=np.array([[1,-1,-1],[0,2,-1],[1,3,-1],[2,4,-1],[3,-1,-1]])
        self.assertEqual(bounded_scope({0},neighbors,(),2),{0,1,2})
        self.assertEqual(bounded_scope({0},neighbors,{2},3),{0,1})

    def test_shape_and_accessory_pixels_are_not_skin(self):
        labels=np.array([[LABEL_NAMES.index(x) for x in ('face','nose','le','imouth','hair','earr')]],np.uint8)
        np.testing.assert_array_equal(mapped_labels(labels,np.ones(labels.shape)),[[3,3,5,5,1,5]])

    def test_subpixel_skin_is_analytically_covered_and_mixed_root_is_not_whole_skin(self):
        labels=np.full((4,4),3,np.uint8);labels[:,2:]=4
        triangles=np.array([[[1.1,1.1],[1.2,1.1],[1.1,1.2]],[[1,1],[3,1],[1,3]]])
        classes,_=coverage_labels(triangles,labels,np.ones(labels.shape))
        np.testing.assert_array_equal(classes,[3,0])

    def test_small_foreign_corner_never_grants_whole_root_authority(self):
        labels=np.full((4,4),3,np.uint8);labels[:,2:]=4
        edge=1/(1-np.sqrt(.02))
        triangles=np.array([[[1,1],[1+edge,1],[1,1+edge]],
                            [[1.1,1.1],[1.2,1.1],[1.1,1.2]]])
        categories,coverage=coverage_labels(triangles,labels,np.ones(labels.shape))
        self.assertAlmostEqual(coverage[0],.98,delta=1e-6)
        np.testing.assert_array_equal(categories,[3,3])
        np.testing.assert_array_equal(whole_root_labels(categories,coverage),[0,3])
        records=[dict(family=f,resolution=1,root_labels=whole_root_labels(categories,coverage))
                 for f in ('front','oblique')]
        accepted,_,_=family_votes(records,2)
        np.testing.assert_array_equal(accepted,[0,3])

    def test_face_semantics_override_body_and_preserve_low_confidence(self):
        ids=np.zeros((1,3),np.int32)
        raw=np.array([[LABEL_NAMES.index('face'),LABEL_NAMES.index('le'),LABEL_NAMES.index('face')]],np.uint8)
        body=('a',ids,np.full(ids.shape,4,np.uint8),np.full(ids.shape,.95))
        row=observe(None,'a',np.zeros((1,3,3),np.uint8),ids,np.zeros(ids.shape),np.zeros((1,3,3)),[raw],[np.array([[.99,.99,.2]])],body)
        np.testing.assert_array_equal(row['labels'],[[3,5,4]])

    def test_current_source_build_covers_subpixel_roots_without_counting_crops_twice(self):
        vertices=np.array([[1.1,1.1,0],[1.2,1.1,0],[1.1,1.2,0]])
        faces=np.array([[0,1,2]])
        rows=[]
        for family in ('front','front','oblique'):
            camera=SimpleNamespace(name=family,basis=np.eye(3),center=np.zeros(3),half_height=2,size=4)
            ids=np.zeros((4,4),np.int32)
            rows.append(dict(camera=camera,family=family,ids=ids,rgb=np.full((4,4,3),150,np.uint8),
                labels=np.full((4,4),3,np.uint8),quality=np.ones((4,4))))
        regions=[dict(subject_id='person',label='face',samples=[[0,.99,.99,4,2]])]
        with patch('local_parent_ownership.project',return_value=vertices), \
             patch('local_parent_ownership.refine',side_effect=lambda rgb,allowed,labels,quality,known,**kw:(labels,quality,{})), \
             patch('local_parent_ownership.source_colors',side_effect=lambda ids,*args:{f:np.full((7,3),.6) for f in ids}):
            proposal=build(rows,regions,[],[],vertices,faces,None,None,None,None)
        self.assertEqual(proposal['roots'][0][0:2],[0,3])
        self.assertEqual(len(proposal['roots'][0][2]),2)
        self.assertEqual(proposal['audit']['added_by_color_only'],0)
        self.assertEqual(proposal['mixed'],[])


if __name__=='__main__':unittest.main()
