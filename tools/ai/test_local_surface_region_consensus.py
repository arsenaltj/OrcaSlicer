import unittest
import numpy as np
from local_surface_region_consensus import propose


class SurfaceConsensusTests(unittest.TestCase):
    def setUp(self):
        self.patch=np.array([0,0,1,1,2,2]);self.area=np.ones(6)
        self.neighbors=np.array([[1,-1,-1],[0,2,-1],[1,3,-1],[2,4,-1],[3,5,-1],[4,-1,-1]])
        self.normals=np.tile([0.,0.,1.],(6,1));self.rgb=np.full((6,3),.7)
        self.labels=np.array([0,-1,-1,-1,-1,-1]);self.names=['cloth','iris','hair']
        self.scores=np.zeros((1,6,6));self.scores[:,:,4]=.99;self.scores[:,:,0]=.01
    def run_case(self,observations=None):
        obs=observations or [(v,np.arange(6).reshape(1,6),self.scores.copy(),np.array([0.,0.,1.])) for v in ('a','b')]
        return propose(self.patch,self.neighbors,self.area,self.normals,self.rgb,self.labels,self.names,obs)[0]
    def test_connected_observed_patches_fill_holes(self):
        np.testing.assert_array_equal(self.run_case(),np.zeros(6))
        np.testing.assert_array_equal(self.labels,[0,-1,-1,-1,-1,-1])
    def test_facial_feature_blocks_propagation(self):
        self.labels[2]=1;result=self.run_case();self.assertEqual(result[2],1);self.assertEqual(result[4],-1)
    def test_separate_shell_cannot_borrow_an_anchor(self):
        self.neighbors[1,1]=-1;self.neighbors[2,0]=-1;self.assertEqual(self.run_case()[2],-1)
    def test_real_source_color_boundary_is_retained(self):
        self.rgb[2:]=.1;self.assertEqual(self.run_case()[2],-1)
    def test_one_view_is_not_two_independent_views(self):
        obs=[('a',np.arange(6).reshape(1,6),self.scores,np.array([0.,0.,1.]))]
        np.testing.assert_array_equal(self.run_case(obs),self.labels)
        with self.assertRaises(ValueError):self.run_case(obs*2)
    def test_conflict_and_weak_evidence_abstain(self):
        other=self.scores.copy();other[:,:,4]=.01;other[:,:,1]=.98
        obs=[('a',np.arange(6).reshape(1,6),p,np.array([0.,0.,1.])) for p in [self.scores]]
        obs.append(('b',obs[0][1],other,obs[0][3]));np.testing.assert_array_equal(self.run_case(obs),self.labels)
    def test_bad_probabilities_are_rejected(self):
        self.scores[0,0,0]=np.nan
        with self.assertRaises(ValueError):self.run_case()
    def test_backfacing_votes_do_not_create_regions(self):
        self.normals*=-1;np.testing.assert_array_equal(self.run_case(),self.labels)


if __name__=='__main__':unittest.main()
