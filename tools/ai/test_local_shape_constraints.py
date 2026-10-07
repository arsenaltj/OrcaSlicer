"""Shape-gate unit tests use synthetic landmark and surface evidence only."""
import types
import unittest

import numpy as np

import local_shape_constraints as shape
from local_eye_landmarks import EYES


def _view(label='re', rim=(10.0, 10.0, 10.0, 10.0)):
    points = np.zeros((478, 2), dtype=float)
    world = np.zeros((478, 3), dtype=float)
    points[468] = [50.0, 50.0]
    world[468] = [0.0, 0.0, 0.0]
    for index, (x, y) in zip((469, 470, 471, 472),
                             ((50 + rim[0], 50), (50, 50 + rim[1]),
                              (50 - rim[2], 50), (50, 50 - rim[3]))):
        points[index] = [x, y]
    for index, value in zip((469, 470, 471, 472),
                            ((rim[0], 0, 0), (0, rim[1], 0),
                             (-rim[2], 0, 0), (0, -rim[3], 0))):
        world[index] = value
    faces = np.arange(8, dtype=np.int64)
    return types.SimpleNamespace(points=points, world=world,
                                 parts={label: (faces, np.ones(len(faces)))})


class ShapeConstraintTests(unittest.TestCase):
    def test_near_round_pupil_and_eye_socket_pass(self):
        views = [_view(), _view()]
        record, accepted, nested = shape.evaluate(
            're', views, np.arange(8), np.arange(2, 5),
            neighbors=np.array([[1, -1, -1], [0, 2, -1], [1, 3, -1], [2, 4, -1],
                                [3, 5, -1], [4, 6, -1], [5, 7, -1], [6, -1, -1]]),
            eye=EYES[0], subject_id='one')
        self.assertEqual(record['status'], shape.VALID_SHAPE)
        self.assertEqual(accepted.tolist(), list(range(8)))
        self.assertEqual(nested, [2, 3, 4])
        self.assertGreaterEqual(record['metrics']['iris_axis_ratio'], .72)

    def test_elongated_pupil_is_protected_but_remains_colorable(self):
        record, accepted, _ = shape.evaluate(
            're', [_view(rim=(20, 4, 20, 4)), _view(rim=(20, 4, 20, 4))],
            np.arange(8), np.arange(2, 5), eye=EYES[0], subject_id='one')
        self.assertEqual(record['status'], shape.PROTECTED_SHAPE_UNCERTAIN)
        self.assertIn('PUPIL_NOT_ROUND', record['reasons'])
        self.assertGreater(accepted.size, 0)
        self.assertEqual(record['accepted_faces'], accepted.tolist())

    def test_eye_or_lip_outside_envelope_is_risk_marked(self):
        views = [_view('ulip'), _view('ulip')]
        views[1].parts['ulip'] = (np.arange(4), np.ones(4))
        record, accepted, _ = shape.evaluate('ulip', views, np.arange(8), subject_id='one')
        self.assertEqual(record['status'], shape.PROTECTED_SHAPE_UNCERTAIN)
        self.assertIn('SHAPE_BOUNDARY_OUTSIDE_ENVELOPE', record['reasons'])
        self.assertGreater(accepted.size, 0)

    def test_eyebrow_bands_are_independent_shape_labels(self):
        left = _view('lb')
        right = _view('rb')
        for label, views in (('lb', [left, left]), ('rb', [right, right])):
            record, accepted, _ = shape.evaluate(
                label, views, np.arange(8), neighbors=np.array([
                    [1, -1, -1], [0, 2, -1], [1, 3, -1], [2, 4, -1],
                    [3, 5, -1], [4, 6, -1], [5, 7, -1], [6, -1, -1]]),
                subject_id='one')
            self.assertIn(record['status'], (shape.VALID_SHAPE, shape.PROTECTED_SHAPE_UNCERTAIN))
            self.assertGreater(accepted.size, 0)
            self.assertEqual(record['label'], label)

    def test_shape_record_validation_rejects_overlap_and_accepts_valid_record(self):
        record = {
            'subject_id': 'one', 'label': 'ulip', 'status': 'VALID_SHAPE',
            'accepted_faces': [0, 1, 2, 3], 'rejected_faces': [4],
            'view_support': 2, 'metrics': {'envelope_coverage': .95}, 'reasons': []}
        self.assertTrue(shape.validate_shape_record(record, 8))
        invalid = dict(record, accepted_faces=[0, 1], rejected_faces=[1])
        self.assertFalse(shape.validate_shape_record(invalid, 8))
        invalid = dict(record, nested_faces=[4])
        self.assertFalse(shape.validate_shape_record(invalid, 8))
        invalid = dict(record, nested_faces=[1])
        self.assertFalse(shape.validate_shape_record(invalid, 8))
        eye = dict(record, label='le', nested_faces=[1])
        self.assertTrue(shape.validate_shape_record(eye, 8))

    def test_upper_and_lower_lip_overlap_is_protected(self):
        upper = _view('ulip')
        lower = _view('llip')
        upper.parts['llip'] = (np.arange(4), np.ones(4))
        lower.parts['ulip'] = (np.arange(4), np.ones(4))
        record, accepted, _ = shape.evaluate('ulip', [upper, lower], np.arange(8), subject_id='one')
        self.assertEqual(record['status'], shape.INVALID_SHAPE_CONFLICT)
        self.assertIn('SHAPE_LIP_CROSSING', record['reasons'])
        self.assertEqual(accepted.size, 0)


if __name__ == '__main__':
    unittest.main()
