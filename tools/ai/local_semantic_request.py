"""Owned local mesh request entry point; importing it requires only the stdlib.

This is separate from the capability probe. The parent supplies a unique request
directory, isolates/terminates the process, and proves the returned actual mesh.
No credentials, installation, cache promotion, or provider calls belong here.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib
import importlib.metadata
import json
import math
import os
from pathlib import Path
import re
import struct
import sys

REQUEST_SCHEMA = 'orcaslicer.local-semantic-request.v1'
RESPONSE_SCHEMA = 'orcaslicer.local-semantic-response.v1'
WORKER_VERSION = 'local-semantic-request-cpu-v1'
LABEL_SCHEMA = 'farl-celebm-face19-subset-v1'
POLICY_VERSION = 'visible-face-semantic-v5-body-supplement'
MODULES = ('glb_artifact.py', 'local_semantic_worker.py', 'local_semantic_geometry.py',
           'local_semantic_render.py', 'local_semantic_transform.py', 'local_semantic_views.py',
           'local_semantic_projection.py', 'local_semantic_pipeline.py', 'local_semantic_request.py', 'local_eye_landmarks.py', 'local_face_landmarks.py', 'local_body_regions.py')
POLICY_MODULES = ('local_semantic_render.py', 'local_semantic_transform.py', 'local_semantic_views.py',
                  'local_semantic_projection.py', 'local_semantic_pipeline.py', 'local_eye_landmarks.py', 'local_face_landmarks.py', 'local_body_regions.py')
RASTER_ACCELERATOR = 'local_semantic_raster.dll'
PACKAGES = ('torch', 'torchvision', 'pyfacer', 'numpy', 'Pillow')
MAX_RENDER_BYTES = 96_000_184
MAX_EVIDENCE_BYTES = 128 * 1024 * 1024
MAX_RESPONSE_BYTES = 64 * 1024
_IDENTIFIER = re.compile(r'[A-Za-z0-9_.-]{1,96}')
_SHA = re.compile(r'[0-9a-f]{64}')
_WORKER_ERRORS = {'weights_missing_or_invalid', 'weights_hash_mismatch', 'semantic_dependencies_unavailable',
                  'unsupported_label_schema', 'network_attempt_rejected', 'invalid_config',
                  'invalid_config_path', 'config_requires_absolute_path', 'invalid_resource_limit',
                  'input_too_large', 'nonfinite_json', 'invalid_json', 'duplicate_json_key'}


class RequestError(Exception):
    """Fixed public error code, never a third-party message or local path."""


def _require(condition, code):
    if not condition:
        raise RequestError(code)


def _json_bytes(value):
    return json.dumps(value, sort_keys=True, ensure_ascii=True, allow_nan=False,
                      separators=(',', ':')).encode('utf-8')


def canonical_hash(value):
    return hashlib.sha256(_json_bytes(value)).hexdigest()


def _file_sha(path, maximum=None):
    digest = hashlib.sha256()
    with Path(path).open('rb') as stream:
        _require(maximum is None or os.fstat(stream.fileno()).st_size <= maximum, 'input_too_large')
        size = 0
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            size += len(block)
            _require(maximum is None or size <= maximum, 'input_too_large')
            digest.update(block)
    return digest.hexdigest()


def runtime_modules(directory):
    """Actual hashes of the fixed installed module set; never echo a request map."""
    directory = Path(directory).resolve(strict=True)
    result = {}
    names = MODULES + ((RASTER_ACCELERATOR,) if os.name == 'nt' and
                       (directory / RASTER_ACCELERATOR).exists() else ())
    for name in names:
        path = directory / name
        _require(path.is_file() and not path.is_symlink() and path.resolve().parent == directory,
                 'runtime_module_unavailable')
        result[name] = _file_sha(path, 2 * 1024 * 1024 if name == RASTER_ACCELERATOR else None)
    return result


def policy_identity(module_hashes):
    _require(isinstance(module_hashes, dict) and
             set(module_hashes) in (set(MODULES), set(MODULES) | {RASTER_ACCELERATOR}) and
             all(isinstance(v, str) and _SHA.fullmatch(v) for v in module_hashes.values()),
             'invalid_module_identity')
    policy_names = POLICY_MODULES + ((RASTER_ACCELERATOR,) if RASTER_ACCELERATOR in module_hashes else ())
    return {'version': POLICY_VERSION, 'label_schema': LABEL_SCHEMA,
            'modules_sha256': {name: module_hashes[name] for name in policy_names}}


def _bootstrap():
    # -I suppresses cwd/PYTHONPATH in the owned process. Add only our own install
    # directory; no request-specified or validation directories may supply code.
    directory = Path(__file__).resolve().parent
    if str(directory) not in sys.path:
        sys.path.insert(0, str(directory))
    worker = importlib.import_module('local_semantic_worker')
    _require(Path(worker.__file__).resolve() == directory / 'local_semantic_worker.py',
             'runtime_module_unavailable')
    worker.restrict_network()
    return worker, directory


def _absolute(value, code):
    _require(isinstance(value, (str, Path)) and 0 < len(str(value)) <= 4096 and '\0' not in str(value), code)
    path = Path(value)
    _require(path.is_absolute(), code)
    return path


def _validate_request(request):
    keys = {'schema', 'request_id', 'source_path', 'source_sha256', 'geometry_id', 'face_count',
            'runtime_fingerprint', 'policy_sha256'}
    _require(type(request) is dict and set(request) == keys and request['schema'] == REQUEST_SCHEMA,
             'invalid_request')
    _require(isinstance(request['request_id'], str) and _IDENTIFIER.fullmatch(request['request_id']), 'invalid_request')
    for name in ('source_sha256', 'geometry_id', 'runtime_fingerprint', 'policy_sha256'):
        _require(isinstance(request[name], str) and _SHA.fullmatch(request[name]), 'invalid_request')
    _require(type(request['face_count']) is int and 1 <= request['face_count'] <= 2_000_000, 'invalid_request')
    _require(isinstance(request['source_path'], str), 'invalid_source_path')
    return _absolute(request['source_path'], 'invalid_source_path')


def _validate_config(config, worker):
    keys = {'schema', 'enabled', 'python_executable', 'weights_directory', 'cpu_threads',
            'timeout_seconds', 'cache_bytes'}
    _require(type(config) is dict and set(config) == keys and config['schema'] == worker.CONFIG_SCHEMA and
             type(config['enabled']) is bool, 'invalid_config')
    _absolute(config['python_executable'], 'invalid_config')
    _absolute(config['weights_directory'], 'invalid_config')
    for key, low, high in (('cpu_threads', 1, 8), ('timeout_seconds', 10, 600), ('cache_bytes', 0, 4 * 1024**3)):
        _require(type(config[key]) is int and low <= config[key] <= high, 'invalid_config')
    _require(config['enabled'], 'semantic_disabled')
    _require(os.path.samefile(config['python_executable'], sys.executable), 'interpreter_identity_mismatch')
    _require(struct.calcsize('P') == 8, 'requires_64bit_runtime')


def _probe_identity(worker, weights):
    return {'worker_version': worker.WORKER_VERSION, 'worker_sha256': _file_sha(worker.__file__),
            'python': sys.version.split()[0], 'python_executable_sha256': _file_sha(sys.executable),
            'bits': struct.calcsize('P') * 8,
            'packages': worker.package_versions(weights),
            'weights': weights, 'device': 'cpu'}


def _validate_projection(value, face_count, labels):
    _require(type(value) is dict and set(value) == {'subjects', 'regions', 'statistics'}, 'invalid_projection')
    subjects, regions = value['subjects'], value['regions']
    _require(type(subjects) is list and len(subjects) <= 32 and
             all(isinstance(s, str) and _IDENTIFIER.fullmatch(s) for s in subjects) and
             len(set(subjects)) == len(subjects), 'invalid_projection')
    _require(type(regions) is list and len(regions) <= 416, 'invalid_projection')
    seen, pairs = set(), set()
    for region in regions:
        _require(type(region) is dict and set(region) == {'subject_id', 'label', 'samples'}, 'invalid_projection')
        subject, label, samples = region['subject_id'], region['label'], region['samples']
        _require(isinstance(subject, str) and subject in subjects and isinstance(label, str) and label in labels and
                 (subject, label) not in pairs, 'invalid_projection')
        pairs.add((subject, label))
        _require(type(samples) is list and len(samples) <= face_count - len(seen), 'invalid_projection')
        previous = -1
        for sample in samples:
            _require(type(sample) is list and len(sample) == 5, 'invalid_projection')
            face, confidence, dominance, pixels, views = sample
            _require(type(face) is int and previous < face < face_count and face not in seen, 'invalid_projection')
            _require(all(type(n) in (int, float) and math.isfinite(n) and 0 <= n <= 1 for n in (confidence, dominance)),
                     'invalid_projection')
            _require(type(views) is int and 1 <= views <= 16 and type(pixels) is int and
                     views <= pixels <= 16 * 1024**2, 'invalid_projection')
            _require(confidence >= .9 and dominance >= .85 and (pixels >= 2 or views >= 2), 'invalid_projection')
            previous = face
            seen.add(face)
    return len(seen)


def _validate_eye_details(hints, regions):
    _require(type(hints) is list and len(hints) <= 64, 'invalid_eye_details')
    allowed = {(r['subject_id'], r['label']): {s[0] for s in r['samples']} for r in regions}
    seen = set()
    for hint in hints:
        _require(type(hint) is dict and set(hint) == {'subject_id', 'label', 'aperture_faces', 'iris_faces'}, 'invalid_eye_details')
        pair = (hint['subject_id'], hint['label'])
        _require(all(type(v) is str for v in pair) and pair[1] in ('re', 'le') and pair in allowed and pair not in seen, 'invalid_eye_details')
        seen.add(pair)
        parent = allowed[pair]
        for key in ('aperture_faces', 'iris_faces'):
            faces = hint[key]
            _require(type(faces) is list and 0 < len(faces) <= len(parent) and
                     all(type(f) is int and f in parent for f in faces) and faces == sorted(set(faces)), 'invalid_eye_details')
            parent = set(faces)


def _validate_feature_details(hints, regions, face_count, vertices=None, triangles=None):
    # Independent shape aids may recover a missed part, but may never claim a
    # different person's surface or replace hair/neck/clothing evidence.
    head = {'face','nose','re','le','ulip','llip','imouth','rb','lb'}
    allowed = {'re','le','ulip','llip','imouth'}
    owners = {s[0]:(r['subject_id'],r['label']) for r in regions for s in r['samples']}
    subjects = {r['subject_id'] for r in regions if r['label'] in head}
    _require(type(hints) is list and len(hints)<=160, 'invalid_feature_details')
    seen, occupied = set(), set()
    for hint in hints:
        fields = {'subject_id','label','faces','iris_faces','view_support'}
        _require(type(hint) is dict and set(hint) in (fields,fields|{'anchor_paths'}), 'invalid_feature_details')
        sid, label = hint['subject_id'], hint['label']
        _require(type(sid) is str and type(label) is str and sid in subjects and label in allowed and
                 (sid,label) not in seen and type(hint['view_support']) is int and 2<=hint['view_support']<=16, 'invalid_feature_details')
        seen.add((sid,label))
        faces, iris = hint['faces'], hint['iris_faces']
        _require(type(faces) is list and 0<len(faces)<=face_count and
                 all(type(f) is int and 0<=f<face_count for f in faces) and faces==sorted(set(faces)), 'invalid_feature_details')
        _require(not occupied.intersection(faces), 'invalid_feature_details')
        occupied.update(faces)
        face_set = set(faces)
        anchored = False
        for f in faces:
            if f in owners:
                owner, name = owners[f]
                _require(owner==sid and name in head, 'invalid_feature_details')
                anchored = True
        if 'anchor_paths' in hint:
            paths = hint['anchor_paths']
            _require(type(paths) is list and 2<=len(paths)<=4 and vertices is not None and triangles is not None,
                     'invalid_feature_details')
            endpoints = set()
            for path in paths:
                _require(type(path) is list and 2<=len(path)<=9 and
                         all(type(f) is int and 0<=f<face_count for f in path) and len(set(path))==len(path) and
                         path[0] in face_set and path[-1] not in face_set, 'invalid_feature_details')
                _require(path[-1] in owners and owners[path[-1]][0]==sid and owners[path[-1]][1] in head,
                         'invalid_feature_details')
                for f in path:
                    _require(f not in owners or (owners[f][0]==sid and owners[f][1] in head), 'invalid_feature_details')
                for a,b in zip(path,path[1:]):
                    corners_a = set(map(tuple,vertices[triangles[a]]))
                    corners_b = set(map(tuple,vertices[triangles[b]]))
                    _require(len(corners_a & corners_b)==2, 'invalid_feature_details')
                endpoints.add(path[-1])
            _require(len(endpoints)==len(paths), 'invalid_feature_details')
            anchored = True
        _require(anchored and type(iris) is list and len(iris)<=len(faces) and
                 all(type(f) is int and f in face_set for f in iris) and iris==sorted(set(iris)) and
                 (label in ('re','le') or not iris), 'invalid_feature_details')


def _statistics(projection, result, face_count, known):
    limits = {name: face_count for name in ('face_count', 'visible_faces', 'unseen_faces', 'ambiguous_faces',
              'cross_subject_faces', 'below_threshold_faces', 'known_faces', 'unknown_faces')}
    limits.update(observations=128, views=16, view_families=16, raw_pixels=16*1024**2,
                  associated_components=32, ambiguous_components=32)
    stats = projection['statistics']
    _require(type(stats) is dict and set(stats) == set(limits) and
             all(type(stats[k]) is int and 0 <= stats[k] <= limit for k, limit in limits.items()), 'invalid_statistics')
    _require(stats['face_count'] == face_count and stats['known_faces'] == known and
             stats['unknown_faces'] == face_count - known and stats['visible_faces'] + stats['unseen_faces'] == face_count and
             stats['view_families'] <= stats['views'], 'invalid_statistics')
    stats = dict(stats)
    for key in ('render_visible_faces', 'render_unseen_faces'):
        n = result[key]
        _require(type(n) is int and 0 <= n <= face_count, 'invalid_statistics')
        stats[key] = n
    _require(stats['render_visible_faces'] + stats['render_unseen_faces'] == face_count, 'invalid_statistics')
    _require(known <= stats['visible_faces'] <= stats['render_visible_faces'] and
             stats['observations'] <= 8 * stats['views'] and
             stats['ambiguous_components'] <= stats['associated_components'], 'invalid_statistics')
    return stats


def _publish(path, raw, limit):
    _require(type(raw) is bytes and len(raw) <= limit, 'output_too_large')
    partial = path.with_name(path.name + '.partial')
    _require(not path.exists() and not path.is_symlink(), 'output_already_exists')
    with partial.open('xb') as stream:
        stream.write(raw)
        stream.flush()
        os.fsync(stream.fileno())
    # Atomic no-overwrite publication. A killed process may leave a partial or
    # unreferenced payload; only the final successful result grants consumption.
    os.link(partial, path)
    partial.unlink()
    return {'bytes': len(raw), 'sha256': hashlib.sha256(raw).hexdigest()}


def _execute(request, config, request_dir, worker, directory):
    source = _validate_request(request)
    _validate_config(config, worker)
    owned = _absolute(request_dir, 'invalid_request_directory').resolve(strict=True)
    _require(owned.is_dir(), 'invalid_request_directory')
    native_path = owned / 'native.bin'
    _require(native_path.is_file() and not native_path.is_symlink() and native_path.resolve().parent == owned,
             'invalid_native_packet')
    _require(184 <= native_path.stat().st_size <= MAX_RENDER_BYTES, 'invalid_native_packet')
    _require(source.is_file() and 0 < source.stat().st_size <= 512 * 1024**2, 'invalid_source_path')
    for name in ('rendered.bin', 'evidence.json', 'result.json'):
        for suffix in ('', '.partial'):
            path = owned / (name + suffix)
            _require(not path.exists() and not path.is_symlink(), 'output_already_exists')
    modules = runtime_modules(directory)
    policy = canonical_hash(policy_identity(modules))
    _require(policy == request['policy_sha256'], 'policy_identity_mismatch')
    _require(_file_sha(source, 512 * 1024**2) == request['source_sha256'], 'source_identity_mismatch')
    native_sha = _file_sha(native_path, MAX_RENDER_BYTES)
    weights = worker.check_weights(Path(config['weights_directory']))
    before = _probe_identity(worker, weights)
    identity = {'probe_identity': before, 'modules_sha256': modules}
    _require(canonical_hash(identity) == request['runtime_fingerprint'], 'runtime_identity_mismatch')
    # Network restriction has already been installed before either heavy import.
    geometry = importlib.import_module('local_semantic_geometry')
    pipeline = importlib.import_module('local_semantic_pipeline')
    for module in (geometry, pipeline):
        _require(Path(module.__file__).resolve().parent == directory, 'runtime_module_unavailable')
    native_vertices, native_faces, native_id = geometry.read(native_path, request['source_sha256'])
    _require(len(native_faces) == request['face_count'] and native_id == request['geometry_id'], 'native_identity_mismatch')
    del native_vertices, native_faces
    loaded = []
    optional_tasks = []

    def model_loader():
        _require(not loaded, 'model_loader_repeated')
        models = worker.load_models(config)
        optional_tasks.extend(getattr(models[2], name, None) for name in ('eye_landmarks', 'body_regions'))
        actual = _probe_identity(worker, models[3])
        _require(actual == before and models[3] == weights, 'runtime_identity_changed')
        loaded.append(actual)
        return models

    try:
        result = pipeline.analyze(source, native_path, request['source_sha256'], model_loader)
    finally:
        for task in optional_tasks:
            if task is not None:
                try:
                    task.close()
                except Exception:
                    pass  # Cleanup must not replace the recognition result/error.
    _require(len(loaded) == 1, 'models_not_loaded')
    _require(result['source_sha256'] == request['source_sha256'] and result['policy_version'] == POLICY_VERSION and
             result['weights'] == weights, 'pipeline_identity_mismatch')
    vertices, faces = result['vertices'], result['faces']
    actual_geometry = geometry.geometry_fingerprint(vertices, faces)
    _require(len(faces) == request['face_count'] and result['render_geometry_id'] == actual_geometry and
             actual_geometry == request['geometry_id'], 'render_identity_mismatch')
    projection = result['projection']
    known = _validate_projection(projection, request['face_count'], worker.SUPPORTED_LABELS)
    stats = _statistics(projection, result, request['face_count'], known)
    evidence = {'schema': 'orcaslicer.local-semantic-evidence.v1', 'label_schema': LABEL_SCHEMA,
                'request_id': request['request_id'], 'source_sha256': request['source_sha256'],
                'geometry_id': request['geometry_id'], 'render_geometry_id': actual_geometry,
                'face_count': request['face_count'], 'weights_sha256': canonical_hash(weights),
                'runtime_sha256': request['runtime_fingerprint'], 'policy_sha256': policy,
                'subjects': projection['subjects'], 'regions': projection['regions']}
    if result.get('eye_details'):
        _validate_eye_details(result['eye_details'], projection['regions'])
        evidence['eye_details'] = result['eye_details']
    if result.get('feature_details'):
        _validate_feature_details(result['feature_details'], projection['regions'], request['face_count'],
                                  result['vertices'], result['faces'])
        evidence['feature_details'] = result['feature_details']
    rendered = geometry.encode(vertices, faces, request['source_sha256'])
    encoded_evidence = _json_bytes(evidence)
    _require(len(rendered) <= MAX_RENDER_BYTES and len(encoded_evidence) <= MAX_EVIDENCE_BYTES, 'output_too_large')
    # Re-read actual bytes and versions after all work, before publishing anything.
    _require(_file_sha(source, 512 * 1024**2) == request['source_sha256'] and _file_sha(native_path, MAX_RENDER_BYTES) == native_sha,
             'input_identity_changed')
    _require(runtime_modules(directory) == modules and worker.check_weights(Path(config['weights_directory'])) == weights and
             _probe_identity(worker, weights) == before, 'runtime_identity_changed')
    files = {'rendered.bin': _publish(owned/'rendered.bin', rendered, MAX_RENDER_BYTES),
             'evidence.json': _publish(owned/'evidence.json', encoded_evidence, MAX_EVIDENCE_BYTES)}
    response = {'schema': RESPONSE_SCHEMA, 'worker_version': WORKER_VERSION, 'request_id': request['request_id'],
                'status': 'ok', 'identity': identity, 'runtime_fingerprint': canonical_hash(identity),
                'policy_sha256': policy, 'files': files, 'statistics': stats}
    _require(len(_json_bytes(response)) <= MAX_RESPONSE_BYTES, 'response_too_large')
    return response


def execute_request(request, config, request_dir):
    """Publish verified payloads and return response; caller publishes result last.

    Raises RequestError with a fixed code. Existing outputs are never replaced.
    The real model loader is called exactly once, without a redundant probe run.
    """
    try:
        worker, directory = _bootstrap()
        return _execute(request, config, request_dir, worker, directory)
    except RequestError:
        raise
    except ImportError:
        raise RequestError('semantic_dependencies_unavailable') from None
    except Exception as error:
        code = str(error)
        raise RequestError(code if code in _WORKER_ERRORS else 'local_semantic_request_failed') from None


def main(argv=None):
    args = argparse.ArgumentParser(description=__doc__)
    args.add_argument('--request', type=Path, required=True)
    args.add_argument('--config', type=Path, required=True)
    args.add_argument('--output', type=Path, required=True)
    options = args.parse_args(argv)
    request_id, output = '', None
    try:
        request_path = _absolute(options.request, 'invalid_request_path')
        config_path = _absolute(options.config, 'invalid_config_path')
        candidate = _absolute(options.output, 'invalid_output_path')
        _require(request_path.name == 'request.json' and candidate.name == 'result.json' and
                 candidate.parent.resolve(strict=True) == request_path.parent.resolve(strict=True), 'invalid_output_path')
        output = candidate
        worker, _ = _bootstrap()
        request = worker.read_json(request_path, 16 * 1024)
        if isinstance(request, dict) and isinstance(request.get('request_id'), str) and _IDENTIFIER.fullmatch(request['request_id']):
            request_id = request['request_id']
        config = worker.load_config(config_path)
        response = execute_request(request, config, request_path.parent)
    except Exception as error:
        code = str(error) if isinstance(error, RequestError) or str(error) in _WORKER_ERRORS else 'local_semantic_request_failed'
        response = {'schema': RESPONSE_SCHEMA, 'worker_version': WORKER_VERSION, 'request_id': request_id,
                    'status': 'unavailable', 'error_code': code}
    try:
        _require(output is not None, 'invalid_output_path')
        _publish(output, _json_bytes(response), MAX_RESPONSE_BYTES)
    except Exception:
        return 2
    return 0 if response['status'] == 'ok' else 2


if __name__ == '__main__':
    raise SystemExit(main())
