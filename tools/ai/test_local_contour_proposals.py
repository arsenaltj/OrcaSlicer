import unittest
from types import SimpleNamespace
from unittest.mock import patch

import numpy as np

import local_contour_proposals as contour


class Witness:
    def __init__(self, family):
        self.family = family
        self.iris = []

    def supported(self, face, world):
        return True

    def references(self, world, library, prefix, curves=None):
        return dict(family=self.family, polygons=[dict(polygon=contour.ROOT, holes=[])])


class ContourProposals(unittest.TestCase):
    def test_association_rejects_ambiguous_people(self):
        regions = [dict(subject_id=s, label='face', samples=[[f] for f in range(20)])
                   for s in ('a', 'b')]
        self.assertEqual(contour.associate_views([SimpleNamespace(head=np.arange(20))], regions), {})
        regions[1]['samples'] = [[f] for f in range(30, 50)]
        result = contour.associate_views([SimpleNamespace(head=np.arange(20))], regions)
        self.assertEqual(list(result), ['a'])

    def test_bounded_scope_cannot_cross_conflicts_or_start_without_seeds(self):
        neighbors = [[1], [0, 2], [1, 3], [2, 4], [3]]
        self.assertEqual(contour.topology_scope({0}, neighbors, range(5), set(), 2), {0, 1, 2})
        self.assertEqual(contour.topology_scope({0}, neighbors, range(5), {1}, 2), {0})
        self.assertEqual(contour.topology_scope(set(), neighbors, range(5), set(), 2), set())

    def test_runtime_outputs_nested_iris_with_two_distinct_witnesses(self):
        vertices = np.array([[0., 0., 0.], [1., 0., 0.], [0., 1., 0.]])
        faces = np.array([[0, 1, 2]])
        shape = dict(subject_id='a', label='le', status='PROTECTED_SHAPE_UNCERTAIN',
                     accepted_faces=[0], rejected_faces=[], nested_faces=[0], reasons=['FIT_RISK'], metrics={})
        region = dict(subject_id='a', label='le', samples=[[0]])
        with patch.object(contour, 'associate_views', return_value={'a': [object()]}), \
             patch.object(contour, 'independent_views', return_value=[]), \
             patch.object(contour, 'proposals', return_value=([Witness('front'), Witness('oblique')], [])):
            result = contour.build([], [region], [shape], vertices, faces)
        eye = [r for r in result['faces'][0]['layers'] if r['label'] == 'iris-le'][0]
        self.assertEqual(eye['parent_label'], 'le')
        self.assertEqual(len(eye['envelope_views']), 2)
        self.assertEqual({v['family'] for v in eye['views']}, {'front', 'oblique'})
        self.assertNotIn('color', result)

    def test_hard_conflict_does_not_create_a_partition(self):
        shape = dict(subject_id='a', label='le', status='PROTECTED_SHAPE_UNCERTAIN',
                     accepted_faces=[0], rejected_faces=[], reasons=['SHAPE_CROSS_EYE'], metrics={})
        self.assertIsNone(contour.build([], [], [shape], np.empty((0, 3)), np.empty((0, 3), int)))

    def test_unproved_seam_neighbor_does_not_inherit_an_eyebrow(self):
        vertices = np.array([[0., 0., 0.], [1., 0., 0.], [0., 1., 0.], [1., 1., 0.]])
        faces = np.array([[0, 1, 2], [1, 3, 2]])
        shape = dict(subject_id='a', label='rb', status='PROTECTED_SHAPE_UNCERTAIN',
                     accepted_faces=[0], rejected_faces=[], reasons=['FIT_RISK'], metrics={})
        with patch.object(contour, 'associate_views', return_value={'a': [object()]}), \
             patch.object(contour, 'independent_views', return_value=[]), \
             patch.object(contour, 'proposals', return_value=([Witness('front'), Witness('oblique')], [])):
            result = contour.build([], [dict(subject_id='a', label='rb', samples=[[0]])],
                                   [shape], vertices, faces)
        neighbor = next(row for row in result['faces'] if row['source_face_id'] == 1)
        self.assertEqual(neighbor['base'][0]['label'], 'R6')
        self.assertEqual(neighbor['layers'], [])

    def test_old_detail_semantics_without_shape_support_only_preserve_appearance(self):
        vertices = np.array([[0., 0., 0.], [1., 0., 0.], [0., 1., 0.], [1., 1., 0.]])
        faces = np.array([[0, 1, 2], [1, 3, 2]])
        for label in ('lb', 'rb', 'le', 're', 'ulip', 'llip'):
            with self.subTest(label=label):
                shape = dict(subject_id='a', label=label, status='PROTECTED_SHAPE_UNCERTAIN',
                             accepted_faces=[0], rejected_faces=[], nested_faces=[], reasons=['FIT_RISK'], metrics={})
                witnesses=[Witness('front'), Witness('oblique')]
                for view in witnesses:
                    view.supported=lambda face,world: face==0
                with patch.object(contour, 'associate_views', return_value={'a': [object()]}), \
                     patch.object(contour, 'independent_views', return_value=[]), \
                     patch.object(contour, 'proposals', return_value=(witnesses, [])):
                    result=contour.build([], [dict(subject_id='a',label=label,samples=[[0],[1]])],
                                         [shape], vertices, faces)
                neighbor=next(row for row in result['faces'] if row['source_face_id']==1)
                self.assertEqual(neighbor['base'][0]['label'],'R6')
                self.assertEqual(neighbor['layers'],[])
                self.assertIn('UNPROVED_DETAIL_NEIGHBOR',neighbor['reasons'])


if __name__ == '__main__':
    unittest.main()
