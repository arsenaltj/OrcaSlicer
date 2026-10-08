from copy import deepcopy
import unittest

import numpy as np

from beauty_leaf_domain import digest
from portrait_r9_evidence import camera_family
from portrait_r9_replay import manual_first, repaired_slot
from portrait_surface_ownership_v2 import (POLICY, SCHEMA, analytic_coverage, combine_votes, connected_units,
                                          bound_subject, confirmed_r6_units,parent_semantic_blockers,root_id,
                                          safe_reference, shape_conflicts,source_parent_seeds,validate)


class ParentCells(unittest.TestCase):
    def test_old_parent_predictions_do_not_veto_new_same_source_evidence(self):
        self.assertEqual(parent_semantic_blockers({'face','hair','cloth','nose','neck'}),set())
        self.assertEqual(parent_semantic_blockers({'face','teeth','imouth'}),{'teeth','imouth'})

    def test_old_topology_proposal_is_not_a_fixed_source_parent(self):
        prior=dict(regions=[dict(status='SUPPORTED_PARENT_PROPOSAL',parent_label='face',leaves=[[0,0,0]]),
                            dict(status='CONFIRMED_PARENT',parent_label='face',leaves=[[1,0,0]])])
        units=[dict(id=str(n),source_face_id=n,implicit_root=True,evidence_source='FROZEN_R6_CONFIRMED_PARENT')
               for n in (0,1)]
        self.assertEqual(confirmed_r6_units(prior,dict(faces=[]),dict(regions=[dict(parent_label='skin',units=units)])),{'1'})

    def fixture(self):
        p = dict(source_sha256='a'*64, geometry_id='b'*64, evidence_sha256='c'*64,
                 face_count=12, partition_sha256='d'*64, faces=[dict(source_face_id=0, cells=[
                     dict(id='e'*64, label='le', subject_id='person'),
                     dict(id='f'*64, label='face', subject_id='person')])])
        locks = dict(locks=[dict(subject_id='person', locked_cells=['e'*64], periocular_cells=[])], partition_ref={'sha256':'1'*64})
        unit = dict(id=root_id(p,1), source_face_id=1, implicit_root=True, view_ids=['front','left'])
        region = dict(id='skin', subject_id='person', parent_label='skin', status='CONFIRMED_PARENT',
                      view_ids=['front','left'], units=[unit])
        d = dict(schema=SCHEMA, **{k:p[k] for k in ('source_sha256','geometry_id','evidence_sha256','face_count')},
                 partition_sha256=p['partition_sha256'], partition_ref=locks['partition_ref'],
                 detail_freeze_sha256='2'*64, policy=POLICY, policy_sha256=digest(POLICY), regions=[region])
        return d,p,locks

    def test_body_skin_and_unknown_siblings(self):
        d,p,l = self.fixture()
        validate(d,p,l,'2'*64)
        d['regions'][0]['units'][0] = dict(id='f'*64,source_face_id=0,implicit_root=False,view_ids=['front','left'])
        validate(d,p,l,'2'*64)

    def test_source_geometry_boundary_and_freeze_drift(self):
        d,p,l = self.fixture()
        for field in ('source_sha256','geometry_id','evidence_sha256','partition_sha256','detail_freeze_sha256'):
            bad = deepcopy(d); bad[field]='3'*64
            with self.subTest(field=field), self.assertRaises(ValueError): validate(bad,p,l,'2'*64)

    def test_feature_other_person_and_duplicate_units_rejected(self):
        d,p,l = self.fixture()
        for mutation in ('feature','other_person','duplicate','root_drift','single_camera'):
            bad = deepcopy(d)
            if mutation=='feature': bad['regions'][0]['units'][0].update(id='e'*64, source_face_id=0)
            if mutation=='other_person': bad['regions'][0]['subject_id']='other'
            if mutation=='duplicate': bad['regions'][0]['units'] *= 2
            if mutation=='root_drift': bad['regions'][0]['units'][0]['source_face_id']=2
            if mutation=='single_camera': bad['regions'][0]['units'][0]['view_ids']=['front','front']
            with self.subTest(mutation=mutation), self.assertRaises(ValueError): validate(bad,p,l,'2'*64)

    def test_disjoint_faces_from_another_person_cannot_use_first_lock_subject(self):
        evidence=dict(subjects=['person'],regions=[dict(subject_id='person')])
        locks=dict(locks=[dict(subject_id='person')])
        self.assertEqual(bound_subject(evidence,locks),'person')
        for bad in (dict(subjects=['person','other'],regions=[]),
                    dict(subjects=['person'],regions=[dict(subject_id='other')])):
            with self.assertRaises(ValueError):bound_subject(bad,locks)

    def test_explicit_shape_conflicts_are_not_new_parent_repair_targets(self):
        evidence=dict(shape_details=[dict(status='INVALID_SHAPE_CONFLICT',rejected_faces=[4,8]),
            dict(status='PROTECTED_SHAPE_UNCERTAIN',rejected_faces=[6])])
        self.assertEqual(shape_conflicts(evidence),{4,8})

    def test_original_parent_seeds_require_purity_and_exclude_details_and_conflicts(self):
        evidence=dict(regions=[dict(label='face',samples=[[0,.99,.99,8,2],[1,.99,.9,8,2],
            [2,.99,.99,8,2],[3,.99,.99,8,2],[4,.99,.99,8,2]]),
            dict(label='hair',samples=[[4,.99,.99,8,2]]),dict(label='le',samples=[[2,.99,.99,8,2]])],
            shape_details=[dict(status='INVALID_SHAPE_CONFLICT',rejected_faces=[3])])
        self.assertEqual(set(source_parent_seeds(evidence)),{0})

    def test_correlated_crop_is_not_an_extra_camera(self):
        result,_ = combine_votes([('front','arm','skin',1.),('front','arm','skin',1.)])
        self.assertEqual(result,{})
        result,_ = combine_votes([('front','arm','skin',1.),('left','arm','skin',1.)])
        self.assertEqual(result['arm'],('skin',['front','left']))

    def test_conflicting_crop_or_parent_preserves_unknown(self):
        for votes in ([('front','x','skin',1.),('left','x','hair',1.)],
                      [('front','x','skin',1.),('front','x','hair',1.),('left','x','skin',1.)]):
            result, conflicts = combine_votes(votes)
            self.assertNotIn('x',result); self.assertIn('x',conflicts)

    def test_subpixel_coverage_uses_area(self):
        labels = np.array([[2,4]],dtype=np.uint8)
        quality = np.ones((1,2))
        triangle = np.array([[.1,.1],[.4,.1],[.1,.4]])
        self.assertAlmostEqual(analytic_coverage(triangle,labels,quality,'skin'),1.)
        self.assertEqual(analytic_coverage(triangle,labels,quality,'cloth'),0.)
        crossing = np.array([[.5,.1],[1.5,.1],[.5,.9]])
        self.assertGreater(analytic_coverage(crossing,labels,quality,'skin'),.6)
        self.assertLess(analytic_coverage(crossing,labels,quality,'skin'),.9)

    def test_camera_family_ignores_crop_scale(self):
        a = dict(basis=np.eye(3).tolist(),half_height=1.,width=1024)
        b = dict(a,half_height=.3,width=4096)
        self.assertEqual(camera_family(a),camera_family(b))

    def test_warm_dark_and_missing_previous_slot_still_use_skin(self):
        palette=[dict(uid='portrait-skin',rgb=[.97,.89,.85]),dict(uid='portrait-dark',rgb=[.15,.15,.16]),
                 dict(uid='portrait-light',rgb=[.96,.97,.98]),dict(uid='portrait-lips',rgb=[.92,.60,.57])]
        for rgb in ([.5,.22,.13],[.9,.4,.3],[.3,.22,.18]):
            self.assertEqual(repaired_slot('skin',palette,rgb,{},rgb)[0],'portrait-skin')

    def test_manual_color_has_priority(self):
        row=dict(id='a',slot_uid='portrait-skin')
        self.assertEqual(manual_first(row,{'a':'portrait-lips'})['slot_uid'],'portrait-lips')
        self.assertEqual(row['slot_uid'],'portrait-skin')

    def test_reference_cannot_escape_cache(self):
        ref=dict(schema='orca.portrait-surface-ownership-reference/v2',sha256='a'*64,path='portrait-ownership/'+'a'*64+'.json')
        self.assertTrue(safe_reference(ref))
        for path in ('../external.json','C:/other.json','/other.json'):
            self.assertFalse(safe_reference(dict(ref,path=path)))

    def test_disconnected_cells_in_one_root_are_separate_components(self):
        units={key:dict(source_face_id=0,parent_label='skin') for key in ('a','b')}
        cells={'a':dict(polygon=[[1,0,0],[.8,.2,0],[.8,0,.2]]),
               'b':dict(polygon=[[0,1,0],[.2,.8,0],[0,.8,.2]])}
        self.assertEqual(len(connected_units(units,cells,np.array([[0,1,2]]))),2)

    def test_partial_parent_cell_connects_to_neighbor_on_shared_seam(self):
        units={'a':dict(source_face_id=0,parent_label='skin'),
               'b':dict(source_face_id=1,parent_label='skin')}
        cells={'a':dict(polygon=[[0,.4,.6],[.2,.4,.4],[0,.6,.4]])}
        vertices=np.array([[0,1,2],[2,1,3]])
        self.assertEqual(connected_units(units,cells,vertices),[('skin',['a','b'])])
        units['b']['parent_label']='cloth'
        self.assertEqual(len(connected_units(units,cells,vertices)),2)

    def test_point_contact_does_not_merge_components(self):
        units={key:dict(source_face_id=0,parent_label='skin') for key in ('a','b')}
        cells={'a':dict(polygon=[[1,0,0],[.5,.5,0],[.5,0,.5]]),
               'b':dict(polygon=[[.5,.5,0],[0,1,0],[0,.5,.5]])}
        self.assertEqual(len(connected_units(units,cells,np.array([[0,1,2]]))),2)


if __name__=='__main__': unittest.main()
