"""Owned request protocol tests with synthetic geometry and mocked models only."""
import contextlib
import copy
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

import local_semantic_request as request
import local_semantic_worker as worker


class RequestTests(unittest.TestCase):
    def test_unknown_feature_requires_two_distinct_connected_subject_context_anchors(self):
        import numpy as np
        vertices=np.array([[1,1,1],[-1,-1,1],[-1,1,-1],[1,-1,-1]])
        triangles=np.array([[0,2,1],[0,1,3],[0,3,2],[1,2,3]])
        regions=[{'subject_id':'a','label':'face','samples':[[0],[1]]}]
        hint={'subject_id':'a','label':'re','faces':[2,3],'iris_faces':[2],
              'view_support':2,'anchor_paths':[[2,0],[3,1]]}
        request._validate_feature_details([hint],regions,4,vertices,triangles)
        for paths in ([],[[2,0]],[[2,0],[3,0]],[[0,1],[3,0]],[[2,0,2,1],[3,0]],[[2,4],[3,0]]):
            with self.subTest(paths=paths),self.assertRaises(request.RequestError):
                request._validate_feature_details([dict(hint,anchor_paths=paths)],regions,4,vertices,triangles)
        for label,owner in [('hair','a'),('face','b')]:
            wrong=copy.deepcopy(regions);wrong.append({'subject_id':owner,'label':label,'samples':[[1]]})
            with self.assertRaises(request.RequestError):request._validate_feature_details([hint],wrong,4,vertices,triangles)
        # Nearby in space is insufficient: the context must share an edge.
        separate=np.vstack((vertices,vertices[triangles[0]]+[.01,0,0]))
        mesh=triangles.copy();mesh[0]=[4,5,6]
        with self.assertRaises(request.RequestError):request._validate_feature_details([hint],regions,4,separate,mesh)

    def test_shape_hints_can_recover_missing_eye_but_not_claim_other_surfaces(self):
        regions=[{'subject_id':'a','label':'face','samples':[[0],[1]]},
                 {'subject_id':'a','label':'hair','samples':[[2]]},
                 {'subject_id':'b','label':'face','samples':[[3]]}]
        hint={'subject_id':'a','label':'re','faces':[0,4],'iris_faces':[4],'view_support':2}
        request._validate_feature_details([hint],regions,6)
        invalid=[dict(hint,faces=[0,2]),dict(hint,faces=[0,3]),dict(hint,faces=[4,5]),
                 dict(hint,view_support=1),dict(hint,iris_faces=[1]),dict(hint,faces=[0,6]),
                 dict(hint,confirmed=True),dict(hint,label='nose')]
        for value in invalid:
            with self.subTest(value=value),self.assertRaises(request.RequestError):
                request._validate_feature_details([value],regions,6)

    def setUp(self):
        import numpy as np
        import local_semantic_geometry as geometry
        import local_semantic_pipeline as pipeline
        self.np, self.geometry, self.pipeline = np, geometry, pipeline
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        self.source = self.directory/'source.glb'
        self.source.write_bytes(b'synthetic source; parser is mocked, no actual GLB or model')
        self.source_sha = hashlib.sha256(self.source.read_bytes()).hexdigest()
        self.vertices = np.array([[0, 0, 0], [1, 0, 0], [0, 1, 0]], dtype=np.float32)
        self.faces = np.array([[0, 1, 2]], dtype=np.uint32)
        self.geometry_id = geometry.geometry_fingerprint(self.vertices, self.faces)
        geometry.write_new(self.directory/'native.bin', self.vertices, self.faces, self.source_sha)
        self.weights = {'mobilenet0.25_Final.pth': '1'*64,
                        'face_parsing.farl.celebm.main_ema_181500_jit.pt': '2'*64}
        self.modules = {name: hashlib.sha256(name.encode()).hexdigest() for name in request.MODULES}
        self.config = {'schema': worker.CONFIG_SCHEMA, 'enabled': True, 'python_executable': sys.executable,
                       'weights_directory': str(self.directory), 'cpu_threads': 1, 'timeout_seconds': 600, 'cache_bytes': 0}
        with patch.object(request.importlib.metadata, 'version', return_value='synthetic-version'):
            self.probe = request._probe_identity(worker, self.weights)
        self.identity = {'probe_identity': self.probe, 'modules_sha256': self.modules}
        self.spec = {'schema': request.REQUEST_SCHEMA, 'request_id': 'test-request-1', 'source_path': str(self.source),
                     'source_sha256': self.source_sha, 'geometry_id': self.geometry_id, 'face_count': 1,
                     'runtime_fingerprint': request.canonical_hash(self.identity),
                     'policy_sha256': request.canonical_hash(request.policy_identity(self.modules))}
        self.projection = {
            'subjects': ['surface-one'], 'regions': [{'subject_id': 'surface-one', 'label': 'face',
                                                     'samples': [[0, .99, 1., 2, 1]]}],
            'statistics': {'face_count': 1, 'observations': 1, 'views': 1, 'view_families': 1,
                           'raw_pixels': 2, 'visible_faces': 1, 'unseen_faces': 0, 'associated_components': 1,
                           'ambiguous_components': 0, 'ambiguous_faces': 0, 'cross_subject_faces': 0,
                           'below_threshold_faces': 0, 'known_faces': 1, 'unknown_faces': 0}}

    @contextlib.contextmanager
    def mocked(self, mutation=None):
        state = {'loads': 0, 'analysis': 0}
        def load_models(config):
            state['loads'] += 1
            return object(), object(), object(), self.weights.copy()
        def analyze(source, native, sha, loader):
            state['analysis'] += 1
            models = loader()
            value = {'source_sha256': sha, 'policy_version': request.POLICY_VERSION, 'weights': models[3],
                     'vertices': self.vertices.copy(), 'faces': self.faces.copy(), 'render_geometry_id': self.geometry_id,
                     'projection': copy.deepcopy(self.projection), 'render_visible_faces': 1, 'render_unseen_faces': 0}
            if mutation is not None: mutation(value, loader)
            return value
        with contextlib.ExitStack() as stack:
            for target, name, value in ((worker, 'restrict_network', lambda: None),
                                        (worker, 'check_weights', lambda directory: self.weights.copy()),
                                        (worker, 'load_models', load_models),
                                        (request, 'runtime_modules', lambda directory: self.modules.copy()),
                                        (request.importlib.metadata, 'version', lambda name: 'synthetic-version'),
                                        (self.pipeline, 'analyze', analyze)):
                stack.enter_context(patch.object(target, name, value))
            yield state

    def test_module_import_in_isolated_python_loads_only_stdlib(self):
        code = ('import importlib.util,sys; '
                f's=importlib.util.spec_from_file_location("entry", {str(Path(request.__file__).resolve())!r}); '
                'm=importlib.util.module_from_spec(s); s.loader.exec_module(m); '
                'assert not any(n in sys.modules for n in ("numpy","torch","facer","local_semantic_pipeline","local_semantic_worker"))')
        result = subprocess.run([sys.executable, '-I', '-c', code], capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_canonical_runtime_and_policy_use_actual_fixed_module_set(self):
        module_dir = self.directory/'modules'; module_dir.mkdir()
        for name in request.MODULES: (module_dir/name).write_bytes(name.encode())
        actual = request.runtime_modules(module_dir)
        self.assertEqual(actual, self.modules)
        policy = request.policy_identity(actual)
        self.assertEqual(set(policy['modules_sha256']), set(request.POLICY_MODULES))
        self.assertEqual(policy['version'], 'visible-face-semantic-v5-body-supplement')
        self.assertEqual(policy['version'], self.pipeline.POLICY_VERSION)
        self.assertEqual(request.canonical_hash({'b': 1, 'a': 2}), hashlib.sha256(b'{"a":2,"b":1}').hexdigest())
        (module_dir/'local_semantic_views.py').write_bytes(b'changed')
        self.assertNotEqual(request.runtime_modules(module_dir), actual)
        with self.assertRaises(ValueError): request.canonical_hash(float('nan'))

    @unittest.skipUnless(sys.platform == 'win32', 'Windows accelerator identity')
    def test_optional_raster_binary_joins_runtime_and_policy_identity(self):
        module_dir = self.directory/'modules'; module_dir.mkdir()
        for name in request.MODULES: (module_dir/name).write_bytes(name.encode())
        binary = module_dir/request.RASTER_ACCELERATOR
        without = request.runtime_modules(module_dir)
        binary.write_bytes(b'first-native-binary')
        with_binary = request.runtime_modules(module_dir)
        self.assertEqual(with_binary[request.RASTER_ACCELERATOR], hashlib.sha256(binary.read_bytes()).hexdigest())
        self.assertEqual(set(with_binary), set(without) | {request.RASTER_ACCELERATOR})
        self.assertNotEqual(request.canonical_hash(request.policy_identity(without)),
                            request.canonical_hash(request.policy_identity(with_binary)))
        binary.write_bytes(b'changed-native-binary')
        changed = request.runtime_modules(module_dir)
        self.assertNotEqual(request.canonical_hash(request.policy_identity(with_binary)),
                            request.canonical_hash(request.policy_identity(changed)))
        binary.write_bytes(b'x' * (2 * 1024 * 1024 + 1))
        with self.assertRaisesRegex(request.RequestError, 'input_too_large'):
            request.runtime_modules(module_dir)

    def test_success_binds_real_geometry_and_publishes_payloads_before_result(self):
        with self.mocked() as state:
            response = request.execute_request(self.spec, self.config, self.directory)
        self.assertEqual(state, {'loads': 1, 'analysis': 1})
        self.assertFalse((self.directory/'result.json').exists())
        self.assertEqual(response['identity'], self.identity)
        self.assertEqual(response['runtime_fingerprint'], self.spec['runtime_fingerprint'])
        for name, descriptor in response['files'].items():
            raw = (self.directory/name).read_bytes()
            self.assertEqual(descriptor, {'bytes': len(raw), 'sha256': hashlib.sha256(raw).hexdigest()})
        vertices, faces, geom = self.geometry.read(self.directory/'rendered.bin', self.source_sha)
        self.np.testing.assert_array_equal(vertices, self.vertices)
        self.np.testing.assert_array_equal(faces, self.faces)
        self.assertEqual(geom, self.geometry_id)
        evidence = json.loads((self.directory/'evidence.json').read_text())
        self.assertEqual(set(evidence), {'schema','label_schema','request_id','source_sha256','geometry_id','render_geometry_id',
                                        'face_count','weights_sha256','runtime_sha256','policy_sha256','subjects','regions'})
        self.assertEqual(evidence['weights_sha256'], request.canonical_hash(self.weights))
        self.assertEqual(evidence['regions'], self.projection['regions'])
        self.assertFalse(list(self.directory.glob('*.partial')))

    def test_cli_publishes_success_result_last_with_exact_response_fields(self):
        spec_path, config_path = self.directory/'request.json', self.directory/'config.json'
        spec_path.write_text(json.dumps(self.spec)); config_path.write_text(json.dumps(self.config))
        with self.mocked():
            code = request.main(['--request', str(spec_path), '--config', str(config_path), '--output', str(self.directory/'result.json')])
        self.assertEqual(code, 0)
        response = json.loads((self.directory/'result.json').read_text())
        self.assertEqual(set(response), {'schema','worker_version','request_id','status','identity','runtime_fingerprint',
                                        'policy_sha256','files','statistics'})

    def test_disabled_config_does_not_analyze_or_load_models(self):
        config = dict(self.config, enabled=False)
        with self.mocked() as state, self.assertRaisesRegex(request.RequestError, '^semantic_disabled$'):
            request.execute_request(self.spec, config, self.directory)
        self.assertEqual(state, {'loads': 0, 'analysis': 0})
        self.assertFalse((self.directory/'rendered.bin').exists())

    def test_real_isolated_cli_disabled_path_requires_no_models(self):
        spec_path, config_path = self.directory/'request.json', self.directory/'config.json'
        spec_path.write_text(json.dumps(self.spec))
        config_path.write_text(json.dumps(dict(self.config, enabled=False)))
        process = subprocess.run([sys.executable, '-I', str(Path(request.__file__).resolve()),
                                  '--request', str(spec_path), '--config', str(config_path),
                                  '--output', str(self.directory/'result.json')], capture_output=True, text=True, timeout=20)
        self.assertEqual(process.returncode, 2, process.stderr)
        result = json.loads((self.directory/'result.json').read_text())
        self.assertEqual(result['error_code'], 'semantic_disabled')
        self.assertEqual(process.stdout, '')
        self.assertEqual(process.stderr, '')
        self.assertFalse((self.directory/'rendered.bin').exists())

    def test_unknown_fields_ranges_and_relative_paths_fail_before_models(self):
        for update in ({'other': 1}, {'face_count': True}, {'face_count': 2_000_001}, {'source_path': 'relative.glb'},
                       {'source_sha256': 'A'*64}, {'request_id': '../unsafe'}):
            with self.mocked() as state, self.assertRaises(request.RequestError):
                request.execute_request(dict(self.spec, **update), self.config, self.directory)
            self.assertEqual(state['loads'], 0)

    def test_input_and_expected_identity_mismatch_stop_before_models(self):
        for key in ('source_sha256', 'geometry_id', 'runtime_fingerprint', 'policy_sha256'):
            with self.mocked() as state, self.assertRaises(request.RequestError):
                request.execute_request(dict(self.spec, **{key: 'f'*64}), self.config, self.directory)
            self.assertEqual(state['loads'], 0)
        with self.mocked() as state, self.assertRaisesRegex(request.RequestError, 'native_identity_mismatch'):
            request.execute_request(dict(self.spec, face_count=2), self.config, self.directory)
        self.assertEqual(state['loads'], 0)

    def test_changed_source_after_analysis_never_publishes_payloads(self):
        def changed(value, loader): self.source.write_bytes(b'changed during analysis')
        with self.mocked(changed), self.assertRaisesRegex(request.RequestError, 'input_identity_changed'):
            request.execute_request(self.spec, self.config, self.directory)
        self.assertFalse((self.directory/'rendered.bin').exists())

    def test_changed_modules_weights_and_python_identity_never_publish(self):
        for what in ('modules', 'weights', 'python'):
            def changed(value, loader):
                if what == 'modules': self.modules = dict(self.modules, **{'local_semantic_views.py': 'f'*64})
                elif what == 'weights': self.weights = dict(self.weights, **{'mobilenet0.25_Final.pth': 'f'*64})
            with self.mocked(changed):
                if what == 'python':
                    altered = dict(self.probe, python_executable_sha256='f'*64)
                    with patch.object(request, '_probe_identity', side_effect=[self.probe, self.probe, altered]), self.assertRaisesRegex(request.RequestError, 'runtime_identity_changed'):
                        request.execute_request(self.spec, self.config, self.directory)
                else:
                    with self.assertRaisesRegex(request.RequestError, 'runtime_identity_changed'):
                        request.execute_request(self.spec, self.config, self.directory)
            self.assertFalse((self.directory/'rendered.bin').exists())
            # Restore independently computed expected maps between scenarios.
            self.modules = self.identity['modules_sha256'].copy()
            self.weights = self.probe['weights'].copy()

    def test_false_render_identity_and_user_authority_are_rejected(self):
        def changed_geometry(value, loader): value['vertices'][1, 0] += .1
        def injected_lock(value, loader): value['projection']['regions'][0]['locked_physical_slot'] = 2
        def false_quality(value, loader): value['projection']['statistics']['known_faces'] = 0
        def unsupported(value, loader): value['projection']['regions'][0]['label'] = 'teeth'
        def low_confidence(value, loader): value['projection']['regions'][0]['samples'][0][1] = .1
        for mutation in (changed_geometry, injected_lock, false_quality, unsupported, low_confidence):
            with self.mocked(mutation), self.assertRaises(request.RequestError):
                request.execute_request(self.spec, self.config, self.directory)
            self.assertFalse((self.directory/'rendered.bin').exists())

    def test_repeated_loader_is_rejected_not_run_twice(self):
        with self.mocked(lambda value, loader: loader()) as state, self.assertRaisesRegex(request.RequestError, 'model_loader_repeated'):
            request.execute_request(self.spec, self.config, self.directory)
        self.assertEqual(state['loads'], 1)

    def test_real_small_weight_files_are_rehashed_after_analysis(self):
        pinned = {}
        actual = {}
        for name in self.weights:
            raw = name.encode()
            (self.directory/name).write_bytes(raw)
            actual[name] = hashlib.sha256(raw).hexdigest()
            pinned[name] = (len(raw), actual[name])
        self.weights = actual
        with patch.object(request.importlib.metadata, 'version', return_value='synthetic-version'):
            self.probe = request._probe_identity(worker, actual)
        self.identity = {'probe_identity': self.probe, 'modules_sha256': self.modules}
        self.spec['runtime_fingerprint'] = request.canonical_hash(self.identity)
        real_checker = worker.check_weights
        def mutate(value, loader):
            path = self.directory/'mobilenet0.25_Final.pth'
            path.write_bytes(b'x' * path.stat().st_size)
        with self.mocked(mutate), patch.object(worker, 'WEIGHTS', pinned), patch.object(worker, 'check_weights', real_checker):
            with self.assertRaisesRegex(request.RequestError, '^weights_hash_mismatch$'):
                request.execute_request(self.spec, self.config, self.directory)
        self.assertFalse((self.directory/'rendered.bin').exists())

    def test_native_packet_mutation_is_not_published(self):
        def mutate(value, loader):
            path = self.directory/'native.bin'
            data = bytearray(path.read_bytes()); data[-1] ^= 1; path.write_bytes(data)
        with self.mocked(mutate), self.assertRaisesRegex(request.RequestError, 'input_identity_changed'):
            request.execute_request(self.spec, self.config, self.directory)
        self.assertFalse((self.directory/'rendered.bin').exists())

    def test_missing_local_dependency_is_unavailable_without_downloading(self):
        with self.mocked(), patch.object(worker, 'load_models', side_effect=worker.WorkerError('semantic_dependencies_unavailable')):
            with self.assertRaisesRegex(request.RequestError, '^semantic_dependencies_unavailable$'):
                request.execute_request(self.spec, self.config, self.directory)
        self.assertFalse((self.directory/'rendered.bin').exists())

    def test_existing_output_and_partial_are_never_replaced(self):
        for name in ('rendered.bin', 'evidence.json.partial', 'result.json'):
            path = self.directory/name; path.write_bytes(b'keep-existing')
            with self.mocked() as state, self.assertRaisesRegex(request.RequestError, 'output_already_exists'):
                request.execute_request(self.spec, self.config, self.directory)
            self.assertEqual(path.read_bytes(), b'keep-existing')
            self.assertEqual(state['loads'], 0)
            path.unlink()

    def test_output_limit_and_partial_publication_failure_cannot_be_success(self):
        with self.mocked(), patch.object(request, 'MAX_EVIDENCE_BYTES', 5), self.assertRaisesRegex(request.RequestError, 'output_too_large'):
            request.execute_request(self.spec, self.config, self.directory)
        self.assertFalse((self.directory/'rendered.bin').exists())
        with self.mocked(), patch.object(request.os, 'link', side_effect=OSError('sensitive path and details')), self.assertRaisesRegex(request.RequestError, '^local_semantic_request_failed$'):
            request.execute_request(self.spec, self.config, self.directory)
        self.assertFalse((self.directory/'result.json').exists())

    def test_cli_failure_is_bounded_and_does_not_leak_exception_details(self):
        spec_path, config_path = self.directory/'request.json', self.directory/'config.json'
        spec_path.write_text(json.dumps(self.spec)); config_path.write_text(json.dumps(self.config))
        with self.mocked(), patch.object(worker, 'load_models', side_effect=RuntimeError('secret value and private path')):
            code = request.main(['--request', str(spec_path), '--config', str(config_path), '--output', str(self.directory/'result.json')])
        self.assertEqual(code, 2)
        raw = (self.directory/'result.json').read_bytes()
        self.assertNotIn(b'secret', raw)
        response = json.loads(raw)
        self.assertEqual(set(response), {'schema','worker_version','request_id','status','error_code'})
        self.assertEqual(response['status'], 'unavailable')
        self.assertLessEqual(len(raw), 64*1024)

    def test_cli_rejects_output_escape_and_duplicate_json(self):
        spec_path, config_path = self.directory/'request.json', self.directory/'config.json'
        spec_path.write_text('{"request_id":"x","request_id":"y"}')
        config_path.write_text(json.dumps(self.config))
        with self.mocked():
            code = request.main(['--request', str(spec_path), '--config', str(config_path), '--output', str(self.directory/'other.json')])
            self.assertEqual(code, 2)
            self.assertFalse((self.directory/'other.json').exists())
            code = request.main(['--request', str(spec_path), '--config', str(config_path), '--output', str(self.directory/'result.json')])
        self.assertEqual(code, 2)
        self.assertEqual(json.loads((self.directory/'result.json').read_text())['error_code'], 'duplicate_json_key')


if __name__ == '__main__':
    unittest.main()
