"""Independent-view geometry, visibility and subject ownership regressions."""
import copy
import unittest
import numpy as np
import local_face_landmarks as face
from types import SimpleNamespace
from unittest.mock import patch


class FaceLandmarkTests(unittest.TestCase):
    def test_projected_sides_survive_crop_mirror_and_oblique_projection(self):
        size = 160
        points = np.full((478, 2), [80., 80.])
        angles = np.linspace(0, 2*np.pi, len(face.OVAL), endpoint=False)
        points[face.OVAL] = np.column_stack((80+65*np.cos(angles), 80+65*np.sin(angles)))
        for label, cx in (('le', 50), ('re', 110)):
            contour, center, rim = face.EYE_BY_LABEL[label]
            angles = np.linspace(np.pi, 3*np.pi, len(contour), endpoint=False)
            points[contour] = np.column_stack((cx+16*np.cos(angles), 70+6*np.sin(angles)))
            points[center] = [cx, 70]
            points[rim] = np.array([[cx-5,70],[cx,65],[cx+5,70],[cx,75]])
        for label, cx in (('lb', 50), ('rb', 110)):
            angles = np.linspace(0, 2*np.pi, len(face.EYEBROWS[label]), endpoint=False)
            points[face.EYEBROWS[label]] = np.column_stack((cx+16*np.cos(angles), 52+3*np.sin(angles)))
        yy, xx = np.indices((size, size))
        ids = np.arange(size*size).reshape(size, size)
        vertices = np.column_stack((xx.ravel(), yy.ravel(), np.zeros(size*size)))
        triangles = np.repeat(np.arange(size*size)[:,None], 3, axis=1)
        camera = SimpleNamespace(half_height=80., size=size)

        def project(p, raster, family):
            bary = np.zeros((*raster.shape, 3)); bary[...,0] = 1
            result = face.from_points(p, raster, bary, vertices, triangles, camera, family)
            self.assertIsNotNone(result)
            for label, cx in (('le',50),('re',110),('lb',50),('rb',110)):
                selected = result.parts[label][0]
                self.assertGreater(len(selected), 0)
                self.assertLess(abs(vertices[selected,0].mean()-cx), 2)
            for label in ('le','re'):
                self.assertGreater(len(result.irises[label][0]), 0)
                self.assertTrue(set(result.irises[label][0]) <= set(result.parts[label][0]))
            return result

        reference = project(points, ids, 'front')
        cropped = project(points-[10,10], ids[10:-10,10:-10], 'crop')
        for label in ('le','re','lb','rb'):
            np.testing.assert_array_equal(reference.parts[label][0], cropped.parts[label][0])
        mirrored = points.copy(); mirrored[:,0] = size-points[:,0]
        project(mirrored, ids[:,::-1], 'mirror')
        oblique = points.copy(); oblique[:,0] = 80+.75*(points[:,0]-80)
        source_x = np.clip(np.floor(80+(np.arange(size)+.5-80)/.75).astype(int),0,size-1)
        project(oblique, ids[:,source_x], 'oblique')

    def test_farl_side_mapping_uses_the_corresponding_iris_and_brow_landmarks(self):
        self.assertEqual(face.EYE_BY_LABEL['le'][1], 468)
        self.assertEqual(face.EYE_BY_LABEL['re'][1], 473)
        self.assertEqual(face.CONTOURS['le'][0], 33)
        self.assertEqual(face.CONTOURS['re'][0], 362)
        self.assertEqual(face.CONTOURS['lb'][0], 46)
        self.assertEqual(face.CONTOURS['rb'][0], 276)

    def test_same_side_semantics_keep_the_full_eye_and_nested_iris(self):
        views=[self.view('front'),self.view('oblique')]
        regions=[{'subject_id':'one','label':'re','samples':[[i] for i in range(20,30)]},
                 {'subject_id':'one','label':'le','samples':[[i] for i in range(50,60)]},
                 {'subject_id':'one','label':'face','samples':[[i] for i in range(100) if i not in range(20,30) and i not in range(50,60)]}]
        _,shapes=face.associate(views,regions,with_shapes=True)
        eye=next(item for item in shapes if item['label']=='re')
        self.assertEqual(eye['accepted_faces'],list(range(20,30)))
        self.assertEqual(eye['nested_faces'],list(range(22,26)))

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

    def test_rejected_overlap_does_not_erase_other_accepted_seed(self):
        first=self.view('re',range(20,23))
        second=self.view('le',range(22,25))
        first.parts['le']=(np.array([22,23,24]),np.ones(3))
        second.parts['le']=(np.array([22,23,24]),np.ones(3))
        second.parts['re']=(np.array([20,21,22]),np.ones(3))
        regions=[{'subject_id':'one','label':'face','samples':[[i] for i in range(90)]}]
        records={
            're': {'subject_id':'one','label':'re','status':'PROTECTED_SHAPE_UNCERTAIN',
                   'accepted_faces':[20],'rejected_faces':[21],'view_support':2,
                   'metrics':{},'reasons':['SHAPE_COMPONENT_DISCONNECTED']},
            'le': {'subject_id':'one','label':'le','status':'PROTECTED_SHAPE_UNCERTAIN',
                   'accepted_faces':[22],'rejected_faces':[21],'view_support':2,
                   'metrics':{},'reasons':['SHAPE_COMPONENT_DISCONNECTED']},
        }
        def evaluate(label,*args,**kwargs):
            item=dict(records[label])
            return item,np.asarray(item['accepted_faces'],dtype=np.int64),[]
        with patch.object(face,'CONTOURS',{'re':face.CONTOURS['re'],'le':face.CONTOURS['le']}), \
             patch.object(face.shape_constraints,'evaluate',side_effect=evaluate):
            result, details=face.associate([first,second],regions,with_shapes=True)
        self.assertEqual({item['label'] for item in result},{'re','le'})
        self.assertEqual({item['label']:item['faces'] for item in result}, {'re':[20],'le':[22]})
        self.assertEqual({item['label']:item['accepted_faces'] for item in details}, {'re':[20],'le':[22]})

    def test_empty_shape_proposal_is_invalid_instead_of_uncertain(self):
        view_a=self.view('a',range(20,23))
        view_b=self.view('b',range(20,23))
        regions=[{'subject_id':'one','label':'face','samples':[[i] for i in range(90)]}]
        def evaluate(label,*args,**kwargs):
            return ({'subject_id':'one','label':label,
                     'status':face.shape_constraints.INVALID_SHAPE_CONFLICT,
                     'accepted_faces':[],'rejected_faces':[20,21,22],
                     'view_support':2,'metrics':{},
                     'reasons':['SHAPE_ORAL_BOUNDARY_MIX']},
                    np.asarray([],dtype=np.int64),[])
        with patch.object(face,'CONTOURS',{'re':face.CONTOURS['re']}), \
             patch.object(face.shape_constraints,'evaluate',side_effect=evaluate):
            _, details=face.associate([view_a,view_b],regions,with_shapes=True)
        self.assertEqual(len(details),1)
        self.assertEqual(details[0]['status'],face.shape_constraints.INVALID_SHAPE_CONFLICT)
        self.assertEqual(details[0]['accepted_faces'],[])


if __name__=='__main__':
    unittest.main()
