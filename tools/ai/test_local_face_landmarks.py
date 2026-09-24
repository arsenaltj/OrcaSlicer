"""Independent-view geometry, visibility and subject ownership regressions."""
import copy
import unittest
import numpy as np
import local_face_landmarks as face


class FaceLandmarkTests(unittest.TestCase):
    def test_context_paths_recover_an_unknown_part_without_painting_its_skin_anchors(self):
        neighbors=np.array([[1,-1,-1],[0,2,-1],[1,3,4],[2,-1,-1],[2,-1,-1]])
        paths=face.context_paths([0],[3,4],set(),np.arange(5),neighbors)
        self.assertEqual(paths,[[0,1,2,3],[0,1,2,4]])
        for blocked in ({1},{3}):
            self.assertEqual(face.context_paths([0],[3,4],blocked,np.arange(5),neighbors),[])
        self.assertEqual(face.context_paths([0],[3,4],set(),np.array([0,2,3,4]),neighbors),[])

    def test_context_paths_do_not_cross_an_unbounded_unknown_strip(self):
        neighbors=np.full((14,3),-1,dtype=int)
        for i in range(13):neighbors[i,0]=i+1;neighbors[i+1,1]=i
        self.assertEqual(face.context_paths([0],[11,12],set(),np.arange(14),neighbors),[])

    def test_context_edges_cross_uv_seams_but_not_vertex_contacts_or_nonmanifold_edges(self):
        vertices=np.array([[0,0,0],[1,0,0],[0,1,0], [1,0,0],[1,1,0],[0,1,0], [1,2,0]])
        faces=np.array([[0,1,2],[3,4,5],[4,6,0]])
        adjacent=face.surface_neighbors(vertices,faces)
        self.assertIn(1,adjacent[0]);self.assertIn(0,adjacent[1]);self.assertTrue((adjacent[2]<0).all())
        adjacent=face.surface_neighbors(vertices,np.vstack((faces,[1,2,6])))
        self.assertNotIn(1,adjacent[0]);self.assertNotIn(0,adjacent[1])

    def view(self, family, inside=range(20,30), quality=60):
        indices=np.array(list(inside),dtype=np.int64)
        return face.FaceView(family,np.zeros((478,2)),np.zeros((478,3)),np.ones(478,bool),
            100.,.1,np.arange(100),np.ones(100),np.arange(100),
            {'re':(indices,np.ones(len(indices)))},
            {'re':(indices[2:6],np.ones(len(indices[2:6])))},quality)

    def test_repeat_crops_do_not_supply_independent_agreement(self):
        self.assertEqual(face.consensus([self.view('a'),self.view('a',quality=200)],'re'),[])

    def test_geometrically_wrong_high_resolution_view_cannot_outvote_agreement(self):
        a,b,wrong=self.view('a'),self.view('b'),self.view('c',quality=500)
        wrong.world[:]=100
        self.assertEqual({v.family for v in face.consensus([wrong,a,b],'re')},{'a','b'})

    def test_visible_disagreement_is_not_union_and_one_view_cannot_expand_eye(self):
        views=[self.view('a',range(20,35)),self.view('b',range(20,30))]
        np.testing.assert_array_equal(face.fuse_masks(views,'re'),np.arange(20,30))
        views.append(self.view('c',range(40,50),quality=200))
        self.assertEqual(len(face.fuse_masks(views,'re')),0)

    def test_valid_face_anchor_allows_missing_eye_class_without_crossing_hair_or_other_person(self):
        regions=[{'subject_id':'one','label':'face','samples':[[i] for i in range(90)]},
                 {'subject_id':'one','label':'hair','samples':[[29]]},
                 {'subject_id':'two','label':'face','samples':[[i] for i in range(100,200)]}]
        result=face.associate([self.view('a'),self.view('b')],regions)
        self.assertEqual(len(result),1)
        self.assertEqual(result[0]['subject_id'],'one')
        self.assertEqual(result[0]['faces'],list(range(20,29)))
        self.assertTrue(set(result[0]['iris_faces'])<=set(result[0]['faces']))
        self.assertEqual(result[0]['view_support'],2)

    def test_ambiguous_person_association_is_rejected(self):
        regions=[{'subject_id':'one','label':'face','samples':[[i] for i in range(70)]},
                 {'subject_id':'two','label':'face','samples':[[i] for i in range(70,100)]}]
        self.assertEqual(face.associate([self.view('a'),self.view('b')],regions),[])

    def test_closed_polygon_uses_pixel_centers_and_never_wraps_negative_coordinates(self):
        mask=face.polygon_mask([[-2,-2],[2,-2],[2,2],[-2,2]],(5,5))
        expected=np.zeros((5,5),bool);expected[:2,:2]=True
        np.testing.assert_array_equal(mask,expected)
        with self.assertRaises(ValueError):face.polygon_mask([[np.nan,0],[1,0],[0,1]],(5,5))


if __name__=='__main__':
    unittest.main()
