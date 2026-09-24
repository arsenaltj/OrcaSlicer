import unittest
import numpy as np
from local_body_regions import supplement


class BodyRegionsTests(unittest.TestCase):
    def setUp(self):
        self.v = np.array([[0,0,z] for z in (8.,10.,4.,.1,7.3,6.,9.)])
        self.f = np.repeat(np.arange(7)[:,None],3,axis=1)
        self.p = dict(subjects=['one'],regions=[dict(subject_id='one',label='face',samples=[[0,.99,1.,2,2],[1,.99,1.,2,2]])],statistics={})
    def obs(self, family, values):
        return family,np.arange(7).reshape(1,-1),np.array([values],np.uint8),np.full((1,7),.99)
    def run_case(self, values, second=None, **kwargs):
        return supplement(self.p,[self.obs('a',values),self.obs('b',second or values)],self.v,self.f,**kwargs)
    def labels(self,p):
        return {s[0]:r['label'] for r in p['regions'] for s in r['samples']}
    def test_supplements_and_preserves_face_and_base(self):
        result=self.run_case([4,1,4,4,2,2,1])
        self.assertEqual(self.labels(result),{0:'face',1:'face',2:'cloth',4:'neck',6:'hair'})
        self.assertEqual(len(self.p['regions']),1)
    def test_conflicting_views_do_not_expand(self):
        self.assertNotIn(2,self.labels(self.run_case([0,0,4,0,0,0,0],[0,0,1,0,0,0,0])))
    def test_one_view_and_low_confidence_abstain(self):
        o=self.obs('a',[0,0,4,0,0,0,0])
        self.assertEqual(self.labels(supplement(self.p,[o],self.v,self.f)),self.labels(self.p))
        a,b=self.obs('a',[4]*7),self.obs('b',[4]*7)
        a[3][:]=.8;b[3][:]=.8
        self.assertEqual(self.labels(supplement(self.p,[a,b],self.v,self.f)),self.labels(self.p))
    def test_face_skin_is_not_ear_expansion(self):
        self.assertEqual(self.labels(self.run_case([3]*7)),{0:'face',1:'face'})
    def test_protected_shape_and_no_multi_person(self):
        self.assertNotIn(2,self.labels(self.run_case([4]*7,blocked=[2])))
        self.p['subjects'].append('two')
        self.assertIs(self.run_case([4]*7),self.p)
    def test_duplicate_family_rejected(self):
        o=self.obs('a',[4]*7)
        with self.assertRaises(ValueError):supplement(self.p,[o,o],self.v,self.f)
    def test_invalid_scores_rejected(self):
        o=self.obs('a',[4]*7);o[3][:]=np.nan
        with self.assertRaises(ValueError):supplement(self.p,[o],self.v,self.f)


if __name__=='__main__':unittest.main()
