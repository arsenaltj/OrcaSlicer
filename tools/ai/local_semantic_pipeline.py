"""Local original-texture views, face parsing and conservative surface projection.

No provider calls, configuration discovery, model installation, CLI or file output.
The process host supplies verified model loading and owns cancellation/output.
"""
import hashlib
import importlib.metadata
from pathlib import Path
import struct
import sys

import numpy as np

import local_eye_landmarks as eye_landmarks
import local_face_landmarks as face_landmarks
import local_body_regions as body_regions
import local_semantic_geometry as geometry
import local_semantic_render as render
from local_semantic_views import coarse_cameras, focus_camera
from local_semantic_projection import Observation, project, LABEL_NAMES, SUPPORTED_LABELS

POLICY_VERSION = 'visible-face-semantic-v5-body-supplement'
MAX_OBSERVATION_PIXELS = 16 * 1024 * 1024


def ambiguous_triangles(vertices, faces):
    """Exact coincident surfaces cannot prove which texture supplied a pixel."""
    _, vertex_ids = np.unique(vertices, axis=0, return_inverse=True)
    corners = np.sort(vertex_ids[faces], axis=1)
    _, groups, counts = np.unique(corners, axis=0, return_inverse=True, return_counts=True)
    return counts[groups] > 1


def _select_detections(scores, rects, points, image_ids, available):
    if scores.ndim != 1 or len(scores) > 1024:
        raise ValueError('Invalid local detector result')
    count = len(scores)
    # pyfacer returns rank-one empty tensors, despite its documented Nx4/Nx5x2
    # shapes. An empty detector result is valid unknown evidence, not an error.
    if count == 0 and rects.size == points.size == image_ids.size == 0:
        return rects.reshape(0, 4), []
    if (rects.shape != (count, 4) or points.shape != (count, 5, 2) or image_ids.shape != (count,) or
            any(not np.isfinite(a).all() for a in (scores, rects, points, image_ids)) or
            np.any(image_ids != 0) or np.any(scores < 0) or np.any(scores > 1)):
        raise ValueError('Invalid local detector result')
    keep = [i for i in range(count) if scores[i] >= .9 and
            rects[i, 2] > rects[i, 0] and rects[i, 3] > rects[i, 1]]
    if len(keep) > 8 or len(keep) > available:
        # Discarding competing detections could falsely attribute a shared
        # surface to the remaining person. Fail to ordinary matching instead.
        raise ValueError('Semantic detection or pixel budget exceeded')
    keep.sort(key=lambda i: (-float(scores[i]), *rects[i].tolist()))
    return rects, keep


def file_sha256(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1024*1024), b''):
            digest.update(block)
    return digest.hexdigest()


def analyze(source, native_packet, source_sha256, model_loader, on_view=None, cancelled=None):
    """Return raw regions and the actual render mesh for subsequent host proof.

    The preliminary exact fingerprint match is intentionally narrower than the
    C++ tolerance proof. It avoids inference on known mismatches; it does not grant
    a host binding or authorize painting. Both source and rendered packet still
    undergo independent C++ verification after the owned worker exits.
    """
    def checkpoint():
        if cancelled is not None and cancelled():
            raise InterruptedError('Local semantic analysis cancelled')

    checkpoint()
    if file_sha256(source) != source_sha256:
        raise ValueError('Semantic source changed')
    native_vertices, native_faces, native_id = geometry.read(native_packet, source_sha256)
    vertices, faces, uv, colors, materials, material_ids = render.load(source)
    rendered_id = geometry.geometry_fingerprint(vertices, faces)
    if len(faces) != len(native_faces) or rendered_id != native_id or file_sha256(source) != source_sha256:
        raise ValueError('Native/render surface does not match exactly')
    del native_vertices, native_faces
    checkpoint()
    torch, detector, parser, weights = model_loader()
    checkpoint()
    supported = np.array([i for i, name in enumerate(LABEL_NAMES) if name in SUPPORTED_LABELS])
    cameras = coarse_cameras(vertices)
    observations, reports, candidates = [], [], []
    eye_observations, face_observations = [], []
    body_observations = []
    body_failed = False
    observation_pixels = 0
    visible_surface = np.zeros(len(faces), dtype=bool)
    ambiguous = ambiguous_triangles(vertices, faces)
    sided = render.double_sided_faces(materials, material_ids)

    def process_view(camera, allow_focus, family):
        nonlocal observation_pixels, body_failed
        checkpoint()
        ids, depths, bary = render.raster(vertices, faces, camera.basis, camera.center,
                                           camera.half_height, camera.size, sided)
        visible = ids >= 0
        ids[visible & ambiguous[np.maximum(ids, 0)]] = -1
        visible_surface[ids[ids >= 0]] = True
        checkpoint()
        projected = render.project(vertices, camera.basis, camera.center, camera.half_height, camera.size)
        rgb = render.shade(faces, uv, colors, materials, material_ids, ids, bary, projected)
        if allow_focus and not body_failed and getattr(parser, 'body_regions', None) is not None:
            try:
                body_observations.append(body_regions.observe(parser.body_regions, rgb, ids, family))
            except Exception:
                body_failed = True
                body_observations.clear()  # Never fuse a partial failed run.
        checkpoint()
        tensor = torch.from_numpy(rgb.copy()).permute(2, 0, 1).unsqueeze(0)
        with torch.inference_mode():
            detected = detector(tensor)
            checkpoint()
            scores = detected['scores'].detach().cpu().numpy()
            rects = detected['rects'].detach().cpu().numpy()
            remaining = max(0, (MAX_OBSERVATION_PIXELS-observation_pixels)//ids.size)
            rects, keep = _select_detections(scores, rects, detected['points'].detach().cpu().numpy(),
                                            detected['image_ids'].detach().cpu().numpy(), remaining)
            count = len(scores)
            report = {'camera': camera.describe(), 'view_family': family, 'detected': count, 'accepted': len(keep),
                      'rgb_sha256': hashlib.sha256(rgb.tobytes()).hexdigest(),
                      'face_ids_sha256': hashlib.sha256(ids.astype('<i4', copy=False).tobytes()).hexdigest(),
                      'visible_faces': int(len(np.unique(ids[ids >= 0]))), 'detections': []}
            if keep:
                chosen = torch.tensor(keep, dtype=torch.int64)
                selected_faces = {key: value[chosen] for key, value in detected.items()}
                parsed = parser(tensor, selected_faces)
                checkpoint()
                logits = parsed['seg']['logits']
                if (list(parsed['seg']['label_names']) != list(LABEL_NAMES) or
                        tuple(logits.shape) != (len(keep), len(LABEL_NAMES), camera.size, camera.size) or
                        not torch.isfinite(logits).all()):
                    raise ValueError('Invalid local parser result')
                confidence, labels = logits.softmax(dim=1).max(dim=1)
                confidence = confidence.detach().cpu().numpy().astype(np.float32)
                labels = labels.detach().cpu().numpy().astype(np.uint8)
                for j, i in enumerate(keep):
                    observations.append(Observation(camera.name, ids, labels[j], confidence[j], float(scores[i]), view_family=family))
                    observation_pixels += ids.size
                    observed = (ids >= 0) & np.isin(labels[j], supported) & (confidence[j] >= .9)
                    report['detections'].append({'score': float(scores[i]), 'rectangle': rects[i].tolist(),
                                                  'supported_pixels': int(observed.sum())})
                    if allow_focus and observed.any():
                        surface = np.unique(ids[observed]).astype('<i4')
                        name = 'focus-'+camera.name+'-'+hashlib.sha256(surface.tobytes()).hexdigest()[:16]
                        crop = focus_camera(camera, rects[i], ids, depths, observed, name)
                        if crop is not None:
                            candidates.append((float(scores[i]), name, crop, family))
            else:
                labels = np.empty((0, camera.size, camera.size), dtype=np.uint8)
                confidence = np.empty(labels.shape, dtype=np.float32)
        if keep and getattr(parser, 'eye_landmarks', None) is not None:
            try:
                eye_observations.extend(eye_landmarks.observe(parser.eye_landmarks, rgb, ids))
                face_observations.extend(face_landmarks.observe(parser.eye_landmarks, rgb, ids, bary, vertices, faces, camera, family))
            except Exception:
                pass  # Missing eye hints must not discard valid raw semantics.
        if on_view is not None:
            on_view(report, rgb, ids, depths, bary, labels, confidence)
        reports.append(report)

    for camera in cameras:
        process_view(camera, True, camera.name)
    unique = {}
    for score, name, camera, family in candidates:
        unique.setdefault(name, (score, name, camera, family))
    for _, _, camera, family in sorted(unique.values(), key=lambda c: (-c[0], c[1]))[:8]:
        process_view(camera, False, family)
    checkpoint()
    if file_sha256(source) != source_sha256:
        raise ValueError('Semantic source changed during analysis')
    result = project(observations, len(faces))
    # Parsing and shape inference remain separate. Extra crops do not add FaRL
    # confidence or multiply a parent crop's semantic votes.
    if getattr(parser, 'eye_landmarks', None) is not None:
        for camera in face_landmarks.detail_cameras(vertices, faces, result['regions']):
            checkpoint()
            ids, depths, bary = render.raster(vertices, faces, camera.basis, camera.center,
                                            camera.half_height, camera.size, sided)
            ids[(ids>=0) & ambiguous[np.maximum(ids,0)]] = -1
            projected = render.project(vertices, camera.basis, camera.center, camera.half_height, camera.size)
            rgb = render.shade(faces, uv, colors, materials, material_ids, ids, bary, projected)
            checkpoint()
            try:
                face_observations.extend(face_landmarks.observe(parser.eye_landmarks, rgb, ids, bary, vertices, faces, camera, camera.name))
            except Exception:
                pass  # Optional shape aids must not discard accepted semantics.
    feature_details = face_landmarks.associate(face_observations, result['regions'], vertices, faces)
    eyes = eye_landmarks.associate(eye_observations, result['regions'])
    if body_observations:
        blocked = {f for hint in feature_details for f in hint['faces']}
        blocked.update(f for hint in feature_details for path in hint.get('anchor_paths', []) for f in path)
        result = body_regions.supplement(result, body_observations, vertices, faces, blocked)
        result['statistics'].update(visible_faces=int(visible_surface.sum()),
                                    unseen_faces=int(len(faces)-visible_surface.sum()))
    checkpoint()
    runtime = {'python': sys.version.split()[0], 'bits': struct.calcsize('P')*8,
               'packages': {name: importlib.metadata.version(name) for name in ('torch','torchvision','pyfacer','numpy','Pillow')}}
    return {'projection': result, 'eye_details': eyes,
            'feature_details': feature_details, 'vertices': vertices, 'faces': faces, 'render_geometry_id': rendered_id,
            'runtime': runtime, 'render_visible_faces': int(visible_surface.sum()),
            'render_unseen_faces': int(len(faces)-visible_surface.sum()),
            'source_sha256': source_sha256, 'weights': weights, 'views': reports, 'policy_version': POLICY_VERSION}
