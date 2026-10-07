import copy
import unittest
from types import SimpleNamespace
from unittest.mock import patch

import numpy as np

from beauty_leaf_domain import LeafKey, digest, domain
from portrait_surface_ownership import build, partition, trusted_regions, validate


class ParentOwnershipTests(unittest.TestCase):
    def fixture(self):
        count, geometry = 1000, 'a' * 64
        leaves = partition(0, {LeafKey(0, 1, 0), LeafKey(0, 2, 4)})
        locks = dict(geometry_id=geometry, source_sha256='b'*64,
                     evidence_sha256='c'*64, face_count=count,
                     locks=[dict(subject_id='person', locked_leaves=[[0, 1, 0]])])
        editing = domain(geometry, count, leaves)
        locks['leaf_domain'] = editing
        document = dict(schema='orca.portrait-surface-ownership/v1',
                        **{k: locks[k] for k in ('geometry_id', 'source_sha256', 'evidence_sha256', 'face_count')},
                        boundary_sha256=digest(locks), policy={}, policy_sha256=digest({}),
                        editing_domain=editing, editing_mapping_sha256=digest(editing),
                        regions=[dict(id='skin', subject_id='person', parent_label='face',
                                      status='CONFIRMED_PARENT', view_ids=['front', 'left'],
                                      leaves=[[0, 2, 4]])])
        return document, locks

    def test_partition_preserves_unevidenced_siblings(self):
        leaves = partition(0, {LeafKey(0, 2, 4)})
        self.assertEqual(sum(4**-k.depth for k in leaves), 1)
        self.assertIn(LeafKey(0, 1, 0), leaves)
        self.assertIn(LeafKey(0, 2, 7), leaves)

    def test_reviewed_lock_cannot_be_subdivided(self):
        with self.assertRaises(ValueError):
            partition(0, {LeafKey(0, 2, 0)}, {LeafKey(0, 1, 0)})

    def test_parent_ownership_rejects_identity_boundary_and_cross_person(self):
        document, locks = self.fixture()
        validate(document, locks)
        for field in ('source_sha256', 'geometry_id', 'evidence_sha256', 'boundary_sha256'):
            changed = copy.deepcopy(document)
            changed[field] = 'd'*64
            with self.assertRaises(ValueError):
                validate(changed, locks)
        for field, value in (('subject_id', 'other'), ('leaves', [[0, 1, 0]]),
                             ('view_ids', ['front', 'front'])):
            changed = copy.deepcopy(document)
            changed['regions'][0][field] = value
            with self.assertRaises(ValueError):
                validate(changed, locks)

    def test_explicit_parent_conflicts_are_not_color_thresholds(self):
        evidence = dict(regions=[dict(label='face', subject_id='person', samples=[[1,.99,.99,4,2]]),
                                dict(label='cloth', subject_id='person', samples=[[1,.99,.99,4,2]]),
                                dict(label='nose', subject_id='person', samples=[[2,.99,.99,4,2]])])
        records, conflicts = trusted_regions(evidence)
        self.assertEqual(conflicts, {1})
        self.assertIn(2, records)

    def test_complete_partition_obeys_total_triangle_budget(self):
        with self.assertRaises(ValueError):
            domain('a'*64, 100, partition(0, {LeafKey(0, 2, 0)}))

    def test_warm_dark_and_unassigned_skin_keep_parent_ownership(self):
        count, geometry = 1000, 'a'*64
        vertices=np.array([[0.,0,0],[1,0,0],[0,1,0]])
        faces=np.tile([0,1,2],(count,1))
        locks=dict(geometry_id=geometry,source_sha256='b'*64,evidence_sha256='c'*64,face_count=count,
                   leaf_domain=domain(geometry,count,[]),locks=[dict(subject_id='person',locked_leaves=[[2,0,0]])])
        evidence=dict(geometry_id=geometry,source_sha256='b'*64,subjects=['person'],
                      regions=[dict(label='face',subject_id='person',samples=[[0,.99,.99,4,2]])])
        native=dict(labels='0'*count,confidence_f32=np.zeros(count,dtype='<f4').tobytes().hex(),
                    subfaces=[[0,1,0,3,np.array([.99],dtype='<f4').tobytes().hex(),4]])
        views=[SimpleNamespace(family=name,head=[0],visible=[0],boundary=None) for name in ('front','left')]
        with patch('portrait_surface_ownership.surface_neighbors',return_value=[[] for _ in range(count)]), \
             patch('portrait_surface_ownership.load_views',return_value=views):
            document,report=build(vertices,faces,evidence,locks,None,native,[[],[],[],[]])
            self.assertEqual(report['accepted_parent_counts']['face'],4)
            self.assertEqual(document['regions'][0]['parent_label'],'face')
            # Old color and confidence cannot veto explicit parent evidence.
            native['labels']='f'*count
            other,_=build(vertices,faces,evidence,locks,None,native,[[],[],[],[]])
            self.assertEqual(other['regions'],document['regions'])
            conflicting=copy.deepcopy(evidence)
            conflicting['regions'].append(dict(label='cloth',subject_id='person',samples=[[0,.99,.99,4,2]]))
            rejected,_=build(vertices,faces,conflicting,locks,None,native,[[]])
            self.assertEqual(rejected['regions'],[])
            no_views=copy.deepcopy(evidence)
            no_views['regions'][0]['samples'][0][-1]=1
            rejected,_=build(vertices,faces,no_views,locks,None,native,[[]])
            self.assertEqual(rejected['regions'],[])


if __name__ == '__main__':
    unittest.main()
