"""Synthetic pipeline regression; no torch import, inference, source files or UI."""
import contextlib
import unittest
from unittest.mock import patch

import numpy as np

import local_semantic_pipeline as pipeline
from local_semantic_views import Camera


class _Tensor:
    def __init__(self, value): self.value = np.asarray(value)
    def __getitem__(self, key): return _Tensor(self.value[key.value if isinstance(key, _Tensor) else key])
    def permute(self, *axes): return _Tensor(self.value.transpose(axes))
    def unsqueeze(self, axis): return _Tensor(np.expand_dims(self.value, axis))
    def detach(self): return self
    def cpu(self): return self
    def numpy(self): return self.value
    def all(self): return self.value.all()
    @property
    def shape(self): return self.value.shape
    def softmax(self, dim):
        values = np.exp(self.value - self.value.max(axis=dim, keepdims=True))
        return _Tensor(values / values.sum(axis=dim, keepdims=True))
    def max(self, dim): return _Tensor(self.value.max(axis=dim)), _Tensor(self.value.argmax(axis=dim))


class _Torch:
    int64 = np.int64
    from_numpy = staticmethod(_Tensor)
    tensor = staticmethod(lambda value, dtype: _Tensor(np.asarray(value, dtype=dtype)))
    inference_mode = staticmethod(contextlib.nullcontext)
    isfinite = staticmethod(lambda value: _Tensor(np.isfinite(value.value)))


def _detection_arrays(count):
    return [np.linspace(1., .91, count), np.tile([0., 0., 4., 4.], (count, 1)),
            np.zeros((count, 5, 2)), np.zeros(count, np.int64)]


class PipelineTests(unittest.TestCase):
    def test_coincident_triangles_are_unknown_but_unrelated_faces_remain_available(self):
        vertices = np.array([[0,0,0], [1,0,0], [0,1,0], [0,0,1], [0,0,0], [1,0,0], [0,1,0]], dtype=np.float32)
        faces = np.array([[0,1,2], [0,1,3], [6,5,4]])
        np.testing.assert_array_equal(pipeline.ambiguous_triangles(vertices, faces), [True, False, True])

    def _analyze(self, count=1, budget=None, cancel_at=None, invalid_parser=None, geometry_matches=True):
        ids = np.repeat(np.arange(8, dtype=np.int32), 2).reshape(4, 4)
        camera = Camera('synthetic', np.eye(3), np.zeros(3), 1., 4)
        state = {'cancelled': False, 'stages': []}
        self.state = state

        def stage(name):
            state['stages'].append(name)
            if name == cancel_at: state['cancelled'] = True

        def detector(tensor):
            stage('detector')
            arrays = _detection_arrays(count) if count else [np.array([]) for _ in range(4)]
            result = {key: _Tensor(value) for key, value in zip(('scores', 'rects', 'points', 'image_ids'), arrays)}
            result['indices'] = _Tensor(np.arange(count))
            return result

        def parser(tensor, detected):
            stage('parser')
            indexes = detected['indices'].value
            logits = np.full((len(indexes), 19, 4, 4), -20.)
            logits[:, 0] = 20.
            for j, index in enumerate(indexes):
                selected = ids == (int(index) if index < 8 else 0)
                logits[j, 0, selected] = -20.
                logits[j, 2, selected] = 20.
            names = list(pipeline.LABEL_NAMES)
            if invalid_parser == 'shape': logits = logits[:, :, :3, :]
            if invalid_parser == 'finite': logits.flat[0] = np.nan
            if invalid_parser == 'labels': names[2] = 'teeth'
            return {'seg': {'logits': _Tensor(logits), 'label_names': names}}

        def loader():
            stage('loader')
            return _Torch, detector, parser, {}

        actual_project = pipeline.project
        def aggregate(*args):
            result = actual_project(*args)
            stage('projection')
            return result

        with contextlib.ExitStack() as stack:
            def mocked(obj, name, value): stack.enter_context(patch.object(obj, name, value))
            mocked(pipeline.importlib.metadata, 'version', lambda name: 'test-version')
            mocked(pipeline, 'file_sha256', lambda path: 'source')
            mocked(pipeline, 'ambiguous_triangles', lambda *args: np.zeros(8,dtype=bool))
            mocked(pipeline.geometry, 'read', lambda *args: (np.zeros((3, 3)), np.zeros((8, 3), int), 'geometry'))
            mocked(pipeline.geometry, 'geometry_fingerprint', lambda *args: 'geometry' if geometry_matches else 'other')
            mocked(pipeline.render, 'load', lambda *args: (np.zeros((3, 3)), np.zeros((8, 3), int), None, None, [], []))
            mocked(pipeline.render, 'double_sided_faces', lambda *args: None)
            def raster(*args):
                stage('raster')
                return ids, np.zeros((4, 4)), np.zeros((4, 4, 3))
            mocked(pipeline.render, 'raster', raster)
            mocked(pipeline.render, 'project', lambda *args: None)
            mocked(pipeline.render, 'shade', lambda *args: np.zeros((4, 4, 3), np.uint8))
            mocked(pipeline, 'coarse_cameras', lambda *args: [camera])
            mocked(pipeline, 'project', aggregate)
            if budget is not None: mocked(pipeline, 'MAX_OBSERVATION_PIXELS', budget)
            return pipeline.analyze('unused', 'unused', 'source', loader, cancelled=lambda: state['cancelled'])

    def test_rank_one_and_documented_empty_detector_shapes_are_valid_unknown(self):
        for arrays in ([np.array([]) for _ in range(4)], _detection_arrays(0)):
            rects, keep = pipeline._select_detections(*arrays, 8)
            self.assertEqual(rects.shape, (0, 4))
            self.assertEqual(keep, [])
        result = self._analyze(count=0)
        self.assertEqual(result['projection']['regions'], [])
        self.assertEqual(result['projection']['statistics']['unknown_faces'], 8)
        self.assertNotIn('parser', self.state['stages'])

    def test_mixed_empty_or_invalid_detector_fields_fail(self):
        invalid = []
        for field, value in ((0, np.zeros((1, 1))), (1, np.zeros((1, 3))),
                             (2, np.zeros((1, 4, 2))), (3, np.ones(1)),
                             (0, np.array([1.1])), (0, np.array([-.1])),
                             (1, np.full((1, 4), np.nan)), (2, np.full((1, 5, 2), np.inf))):
            arrays = _detection_arrays(1); arrays[field] = value; invalid.append(arrays)
        arrays = _detection_arrays(0); arrays[2] = np.zeros((1, 5, 2)); invalid.append(arrays)
        invalid.append(_detection_arrays(1025))
        for arrays in invalid:
            with self.subTest(shapes=[a.shape for a in arrays]), self.assertRaises(ValueError):
                pipeline._select_detections(*arrays, 8)

    def test_valid_detection_limit_and_budget_reject_without_top_score_truncation(self):
        for count, available in ((9, 20), (2, 1), (1, 0)):
            with self.assertRaisesRegex(ValueError, 'budget exceeded'):
                pipeline._select_detections(*_detection_arrays(count), available)
        with self.assertRaisesRegex(ValueError, 'budget exceeded'):
            self._analyze(count=9)
        self.assertNotIn('parser', self.state['stages'])
        with self.assertRaisesRegex(ValueError, 'budget exceeded'):
            self._analyze(count=2, budget=16)
        self.assertNotIn('parser', self.state['stages'])

    def test_low_score_detections_are_not_budget_consumers(self):
        arrays = _detection_arrays(9)
        arrays[0][:8] = .2
        _, keep = pipeline._select_detections(*arrays, 1)
        self.assertEqual(keep, [8])

    def test_cancel_during_projection_never_returns_success(self):
        with self.assertRaises(InterruptedError): self._analyze(cancel_at='projection')
        self.assertEqual(self.state['stages'][-1], 'projection')

    def test_cancel_stops_before_next_expensive_stage(self):
        for current, following in (('loader', 'raster'), ('raster', 'detector'),
                                   ('detector', 'parser'), ('parser', 'projection')):
            with self.subTest(stage=current), self.assertRaises(InterruptedError):
                self._analyze(cancel_at=current)
            self.assertIn(current, self.state['stages'])
            self.assertNotIn(following, self.state['stages'])

    def test_actual_geometry_mismatch_stops_before_model_loading(self):
        with self.assertRaisesRegex(ValueError, 'does not match exactly'):
            self._analyze(geometry_matches=False)
        self.assertEqual(self.state['stages'], [])

    def test_parser_shape_schema_and_finiteness_fail_strongly(self):
        for failure in ('shape', 'labels', 'finite'):
            with self.subTest(failure=failure), self.assertRaisesRegex(ValueError, 'parser result'):
                self._analyze(invalid_parser=failure)
            self.assertNotIn('projection', self.state['stages'])

    def test_synthetic_aligned_output_has_expected_face_without_identity_authority(self):
        result = self._analyze()
        self.assertEqual(result['projection']['statistics']['known_faces'], 1)
        region = result['projection']['regions'][0]
        self.assertEqual(region['label'], 'face')
        self.assertEqual(region['samples'][0][0], 0)
        self.assertEqual(set(region), {'subject_id', 'label', 'samples'})


if __name__ == '__main__':
    unittest.main()
