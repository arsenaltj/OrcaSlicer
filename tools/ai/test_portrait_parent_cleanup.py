import unittest
from copy import deepcopy

import numpy as np

from beauty_leaf_domain import digest
from portrait_parent_cleanup import (POLICY, adjacent_units, apply_plan, build_plans,
                                    histories_for, lab, material_targets, protect_ownership, supported_cleanup)
from portrait_surface_ownership_v2 import SCHEMA, combine_votes, root_id
from surface_detail_freeze import capture, validate as validate_freeze


def palette(count=6):
    return [dict(uid=uid, rgb=np.asarray(rgb).tolist()) for uid, rgb in (
        ('portrait-skin', [.969,.886,.855]), ('portrait-dark', [.157,.149,.161]),
        ('portrait-light', [.965,.969,.976]), ('portrait-lips', [.918,.604,.573]),
        ('portrait-cool', [.40,.55,.71]), ('portrait-mid', [.55,.55,.56]))][:count]


def unit(key, rgb, parent='skin', face=0, subject='person'):
    return dict(id=key, source_rgb=rgb, source_face_id=face, parent_label=parent,
                subject_id=subject, view_ids=['front','side'], anatomical_scope='body-skin',
                implicit_root=True)


class ParentMaterials(unittest.TestCase):
    def test_warm_dark_and_missing_slot_skin_uses_fixed_role_all_counts(self):
        units = {'warm':unit('warm',[.65,.24,.15]), 'dark':unit('dark',[.1,.07,.05])}
        for count in (3,4,5,6):
            targets, _, _ = material_targets(units, {}, palette(count), {})
            self.assertEqual({v[0] for v in targets.values()}, {'portrait-skin'})
        history = histories_for(units, dict(cells=[],palette=palette()), {}, {}, {})
        self.assertTrue(all(r['original_slot'] is None for r in history.values()))
        self.assertTrue(all(r['source']=='SOURCE_TEXTURE' for r in history.values()))

    def test_small_real_pattern_does_not_exempt_entire_white_garment(self):
        units = {'white':unit('white',[.88,.88,.88],'cloth'),
                 'red':unit('red',[.8,.12,.16],'cloth')}
        targets, retained, _ = material_targets(units, {'white':{'red'},'red':{'white'}}, palette(), {})
        self.assertEqual(targets['white'][0], 'portrait-light')
        self.assertNotIn('red', targets)
        self.assertEqual(retained['red'], 'LOCAL_SOURCE_PIGMENT_PATTERN_PRESERVED')

    def test_arbitrary_cell_spanning_legacy_colors_has_no_invented_old_slot(self):
        from beauty_leaf_domain import LeafKey
        key = 'cut'
        cells = {key:dict(polygon=np.eye(3).tolist(),triangles=[np.eye(3).tolist()])}
        colors = {0:[dict(face_id=0,path=dict(depth=1,value=0),color=palette()[3]['rgb'])]}
        result = histories_for({key:unit(key,[.8,.6,.5])},dict(cells=[],palette=palette()),
                               {0:palette()[0]['rgb']},colors,cells)[key]
        self.assertEqual(result['source'],'MIXED_OLD_SUBFACE_COLOR')
        self.assertIsNone(result['original_slot'])
        self.assertAlmostEqual(result['legacy_subface_candidates'][0]['coverage'],.25)
        self.assertEqual(result['legacy_subface_candidates'][0]['leaf'],LeafKey(0,1,0).encode())

    def test_connected_neutral_shadow_is_bounded_to_three_rings(self):
        values = [.65,.55,.50,.45,.40]
        units = {str(i):unit(str(i),[v,v,v],'cloth') for i,v in enumerate(values)}
        neighbors = {str(i):{str(j) for j in (i-1,i+1) if 0<=j<len(values)} for i in range(len(values))}
        targets, _, _ = material_targets(units, neighbors, palette(5), {})
        self.assertIn('0', targets)
        self.assertIn('3', targets)
        self.assertNotIn('4', targets)

    def test_gray_stripe_requires_two_independent_visible_views_and_benefit(self):
        units = {key:unit(key,[.26,.26,.26],'cloth') for key in ('a','b')}
        # Above the neutral core threshold this is white; test a separate gray
        # material below it, isolated from a white source support component.
        for u in units.values():
            u['source_rgb'] = [.2,.2,.2]
        neighbors = {'a':{'b'}, 'b':{'a'}}
        targets, _, _ = material_targets(units, neighbors, palette(),
            {'a':{'front':16,'side':16},'b':{'front':16,'side':15}})
        self.assertNotIn('a', targets)
        targets, _, _ = material_targets(units, neighbors, palette(),
            {'a':{'front':16,'side':16},'b':{'front':16,'side':16}})
        self.assertEqual(targets['a'][0], 'portrait-mid')

    def test_hair_buttons_and_unsupported_pigment_are_preserved(self):
        units = {'hair':unit('hair',[.05,.04,.04],'hair'),
                 'button':unit('button',[.01,.01,.01],'cloth')}
        targets, retained, _ = material_targets(units, {}, palette(), {})
        self.assertEqual(targets, {})
        self.assertEqual(retained['hair'], 'HAIR_BOUNDARY_PROTECTED')

    def test_cleanup_donors_do_not_cascade(self):
        units = {key:unit(key,[.8,.65,.58]) for key in ('d1','d2','a','b')}
        neighbors = {'d1':{'a'},'d2':{'a','b'},'a':{'d1','d2','b'},'b':{'d2','a'}}
        targets = {key:('portrait-skin','source') for key in units}
        histories = {key:dict(original_slot='portrait-skin' if key.startswith('d') else 'portrait-lips') for key in units}
        sources = {key:lab([u['source_rgb']])[0] for key,u in units.items()}
        accepted, _, donors = supported_cleanup(units, neighbors, targets, histories, sources)
        self.assertIn('a', accepted)
        self.assertNotIn('b', accepted)
        self.assertEqual(donors, {'d1','d2'})

    def test_cleanup_does_not_borrow_other_person_or_parent(self):
        units = {'a':unit('a',[.8,.65,.58]),'cloth':unit('cloth',[.8,.65,.58],'cloth'),
                 'other':unit('other',[.8,.65,.58],subject='other-person')}
        targets = {key:('portrait-skin','source') for key in units}
        histories = {key:dict(original_slot=None if key=='a' else 'portrait-skin') for key in units}
        sources = {key:lab([u['source_rgb']])[0] for key,u in units.items()}
        accepted, _, _ = supported_cleanup(units, {'a':{'cloth','other'}}, targets, histories, sources)
        self.assertNotIn('a', accepted)

    def test_associated_crop_cannot_supply_two_votes(self):
        accepted, _ = combine_votes([('front','a','skin',1),('front','a','skin',1)])
        self.assertEqual(accepted,{})
        accepted, _ = combine_votes([('front','a','skin',1),('side','a','skin',1)])
        self.assertIn('a', accepted)


class CellComposition(unittest.TestCase):
    def fixture(self):
        eye, skin = digest('eye'), digest('skin')
        document = dict(schema='orca.surface-partition/v1', geometry_id='a'*64, source_sha256='b'*64,
                        evidence_sha256='c'*64, face_count=3, partition_sha256='d'*64, faces=[])
        left = [[1,0,0],[.5,.5,0],[.5,0,.5]]
        right = [[.5,.5,0],[0,1,0],[0,0,1],[.5,0,.5]]
        document['faces'] = [dict(source_face_id=0, cells=[
            dict(id=eye,label='le',subject_id='person',parent_label='le',polygon=left,holes=[]),
            dict(id=skin,label='face',subject_id='person',parent_label='face',polygon=right,holes=[])])]
        locks = dict(partition_ref=dict(path='surface-partitions/x.json'),locks=[
            dict(label='le',parent_label='le',subject_id='person',status='VALID_SHAPE',view_support=2,
                 locked_cells=[eye],nested_cells=[],periocular_cells=[],reasons=[])])
        plan = dict(schema='orca.portrait-color-plan/v1', palette=palette(), weights={'source_error':1},cells=[
            dict(id=eye,source_face_id=0,label='le',slot_uid='portrait-light',slot=2,color_source='FIXED_ROLE'),
            dict(id=skin,source_face_id=0,label='face',slot_uid='portrait-lips',slot=3,color_source='OLD_COLOR')])
        freeze = capture(document, locks, {6:plan}, labels=('le',))
        u = unit(skin,[.8,.6,.5],face=0)
        u['implicit_root'] = False
        own = dict(schema=SCHEMA,source_sha256=document['source_sha256'],geometry_id=document['geometry_id'],
                   face_count=3,evidence_sha256=document['evidence_sha256'],partition_sha256=document['partition_sha256'],
                   partition_ref=locks['partition_ref'],detail_freeze_sha256=freeze['fingerprint'],
                   policy=POLICY,policy_sha256=digest(POLICY),regions=[
                   dict(id=digest('region'),subject_id='person',parent_label='skin',status='CONFIRMED_PARENT',
                        view_ids=['front','side'],units=[u])])
        return document,locks,plan,freeze,own,eye,skin

    def test_confirmed_mixed_sibling_changes_without_modifying_frozen_eye(self):
        document,locks,old,freeze,own,eye,skin = self.fixture()
        histories = {skin:dict(source='OLD_SUBFACE_COLOR',original_slot='portrait-lips')}
        plans,_ = build_plans(old,document,locks,own,freeze['fingerprint'],histories,{skin:set()}, {})
        result = plans['combined']
        self.assertTrue(validate_freeze(document,locks,{6:result},freeze))
        mapping = np.array([[0,1,-1]])
        lookup = [dict(id=eye,source_face_id=0),dict(id=skin,source_face_id=0)]
        base = np.array([[[246,247,249],[234,154,146],[33,22,11]]],np.uint8)
        colored,_ = apply_plan(base,np.array([[0,0,2]]),mapping,lookup,result,document)
        np.testing.assert_array_equal(colored[0,0],base[0,0])
        np.testing.assert_array_equal(colored[0,2],base[0,2])
        self.assertFalse(np.array_equal(colored[0,1],base[0,1]))

    def test_whole_root_repair_on_mixed_face_fails_closed(self):
        document,*_ = self.fixture()
        key = root_id(document,0)
        plan = dict(palette=palette(),cells=[dict(id=key,source_face_id=0,implicit_root=True,
                    parent_label='skin',slot_uid='portrait-skin',color_source='PARENT_UNIFORM')])
        with self.assertRaisesRegex(ValueError,'mixed root'):
            apply_plan(np.zeros((1,1,3),np.uint8),np.array([[0]]),np.array([[-1]]),[],plan,document)

    def test_manual_color_overrides_parent_repair(self):
        document,locks,old,freeze,own,_,skin = self.fixture()
        plans,_ = build_plans(old,document,locks,own,freeze['fingerprint'],
                             {skin:dict(source='SOURCE_TEXTURE',original_slot=None)}, {skin:set()}, {},{skin:'portrait-cool'})
        row = next(r for r in plans['combined']['cells'] if r['id']==skin)
        self.assertEqual(row['slot_uid'],'portrait-cool')
        self.assertEqual(row['color_source'],'MANUAL')

    def test_inherited_manual_color_is_not_overwritten_by_automatic_skin(self):
        document,locks,old,freeze,own,_,skin = self.fixture()
        plans,_ = build_plans(old,document,locks,own,freeze['fingerprint'],
                             {skin:dict(source='MANUAL_COLOR',original_slot='portrait-cool')}, {skin:set()}, {})
        for plan in plans.values():
            row = next(r for r in plan['cells'] if r['id']==skin)
            self.assertEqual(row['slot_uid'],'portrait-cool')
            self.assertEqual(row['color_source'],'MANUAL')

    def test_single_view_and_identity_drift_rejected(self):
        document,locks,old,freeze,own,_,skin = self.fixture()
        for drift in ('source_sha256','geometry_id','partition_sha256'):
            changed = deepcopy(own)
            changed[drift] = 'e'*64
            with self.assertRaises(ValueError):
                build_plans(old,document,locks,changed,freeze['fingerprint'],{skin:dict(original_slot=None)}, {}, {})
        own['regions'][0]['units'][0]['view_ids'] = ['front']
        with self.assertRaises(ValueError):
            build_plans(old,document,locks,own,freeze['fingerprint'],{skin:dict(original_slot=None)}, {}, {})

    def test_proposed_parent_keeps_inherited_color_until_confirmed(self):
        document,locks,old,freeze,own,_,skin = self.fixture()
        own['regions'][0]['status'] = 'SUPPORTED_PARENT_PROPOSAL'
        plans,_ = build_plans(old,document,locks,own,freeze['fingerprint'],
                             {skin:dict(source='BASELINE_CELL_COLOR',original_slot='portrait-lips')}, {}, {})
        for plan in plans.values():
            row = next(r for r in plan['cells'] if r['id']==skin)
            self.assertEqual(row['slot_uid'],'portrait-lips')
            self.assertEqual(row['retain_reason'],'PARENT_OWNERSHIP_NOT_CONFIRMED')

    def test_native_oral_and_accessories_are_not_repainted_as_parent_skin(self):
        document,locks,_,freeze,own,_,_ = self.fixture()
        for label in (6,8):
            native = dict(labels=[format(label,'x'),'0','0'],confidence_f32=np.ones(3,dtype='<f4').tobytes().hex(),subfaces=[])
            safe = protect_ownership(own,document,native)
            self.assertEqual(safe['regions'],[])
            self.assertEqual(safe['preserved'][0]['reason'],'SOURCE_ORAL_OR_ACCESSORY_PROTECTION')
            self.assertEqual(own['regions'][0]['units'][0]['parent_label'],'skin')

    def test_shared_root_and_vertex_do_not_create_connectivity(self):
        document,_,_,_,_,eye,skin = self.fixture()
        units = {eye:unit(eye,[1,1,1],face=0), skin:unit(skin,[1,1,1],face=0)}
        cells = {c['id']:c for c in document['faces'][0]['cells']}
        graph = adjacent_units(units,cells,np.array([[0,1,2]]))
        self.assertEqual(graph[eye],{skin})
        # Different subjects cannot acquire donor support through a shared edge.
        units[skin]['subject_id'] = 'different-person'
        graph = adjacent_units(units,cells,np.array([[0,1,2]]))
        self.assertEqual(graph[eye],set())


if __name__=='__main__':
    unittest.main()
