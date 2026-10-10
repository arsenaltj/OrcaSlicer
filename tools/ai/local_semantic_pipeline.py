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
from local_semantic_views import coarse_cameras, focus_camera, parent_cameras
from local_semantic_projection import Observation, project, LABEL_NAMES, SUPPORTED_LABELS

POLICY_VERSION = 'visible-face-semantic-v12-parent-local-views'
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


def _apply_shape_parent_fallback(projection, shape_details):
    """Rebuild one-owner regions around accepted shape proposals.

    Landmark proposals may refine a broad ``face`` region when the parser did
    not emit a dedicated eye/lip row.  The old fallback only retained samples
    already under a detail row, so a proposed face remained in both ``face``
    and the detail returned by ``associate``.  Reassign accepted samples to a
    legal detail owner and move only rejected samples back to ``face``.
    """
    if not shape_details:
        return
    parent_labels = {
        're': {'re', 'face'}, 'le': {'le', 'face'},
        'ulip': {'ulip', 'face'}, 'llip': {'llip', 'face'},
        'imouth': {'imouth', 'face'},
        'lip-line-corner': {'lip-line-corner', 'face', 'ulip', 'llip', 'imouth'},
        'lb': {'lb', 'face'}, 'rb': {'rb', 'face'},
    }
    regions = [dict(region, samples=list(region['samples'])) for region in projection['regions']]
    owner = {}
    sample_by_face = {}
    for index, region in enumerate(regions):
        for sample in region['samples']:
            face = int(sample[0])
            owner[face] = (index, region['subject_id'], region['label'])
            sample_by_face[face] = sample

    accepted_moves = {}
    rejected_to_parent = set()
    for item in shape_details:
        label = item['label']
        accepted = set(item['accepted_faces']) if item['status'] != 'INVALID_SHAPE_CONFLICT' else set()
        rejected = set(item['rejected_faces'])
        for face in sorted(accepted):
            source = owner.get(face)
            legal = source is not None and source[1] == item['subject_id'] and source[2] in parent_labels.get(label, {label})
            if not legal:
                accepted.discard(face)
                rejected.add(face)
                reason = 'SOURCE_MAPPING' if source is None else 'SHAPE_OWNER_CONFLICT'
                if reason not in item['reasons']:
                    item['reasons'].append(reason)
                continue
            target = (item['subject_id'], label)
            previous = accepted_moves.get(face)
            if previous is not None and previous != target:
                accepted_moves.pop(face, None)
                rejected_to_parent.add(face)
                for other in shape_details:
                    if other is item or face not in other['accepted_faces']:
                        continue
                    other['accepted_faces'] = [value for value in other['accepted_faces'] if value != face]
                    other['rejected_faces'] = sorted(set(other['rejected_faces']) | {face})
                    other['status'] = 'PROTECTED_SHAPE_UNCERTAIN'
                    if 'SHAPE_CONFLICTING_COMPONENT' not in other['reasons']:
                        other['reasons'].append('SHAPE_CONFLICTING_COMPONENT')
                item['status'] = 'PROTECTED_SHAPE_UNCERTAIN'
                if 'SHAPE_CONFLICTING_COMPONENT' not in item['reasons']:
                    item['reasons'].append('SHAPE_CONFLICTING_COMPONENT')
                continue
            accepted_moves[face] = target
        item['accepted_faces'] = sorted(accepted)
        item['rejected_faces'] = sorted(rejected)
        if item['accepted_faces'] and item['status'] == 'INVALID_SHAPE_CONFLICT':
            # A hard mapping conflict removed only part of a proposal. Keep
            # the legal seed usable and expose the conflict as a risk; a
            # completely unbound proposal remains INVALID below.
            item['status'] = 'PROTECTED_SHAPE_UNCERTAIN'
        elif not item['accepted_faces'] and any(reason in item['reasons']
                                                for reason in ('SOURCE_MAPPING', 'SHAPE_OWNER_CONFLICT')):
            item['status'] = 'INVALID_SHAPE_CONFLICT'
        for face in rejected:
            source = owner.get(face)
            if source is not None and source[1] == item['subject_id'] and source[2] == label:
                rejected_to_parent.add(face)

    additions = {}
    for index, region in enumerate(regions):
        keep = []
        for sample in region['samples']:
            face = int(sample[0])
            target = accepted_moves.get(face)
            if target is not None and target != (region['subject_id'], region['label']):
                additions.setdefault(target, {})[face] = sample
                continue
            if face in rejected_to_parent and region['label'] != 'face':
                additions.setdefault((region['subject_id'], 'face'), {})[face] = sample
                continue
            keep.append(sample)
        region['samples'] = keep

    for target, samples in additions.items():
        existing = next((region for region in regions
                          if (region['subject_id'], region['label']) == target), None)
        if existing is None:
            existing = {'subject_id': target[0], 'label': target[1], 'samples': []}
            regions.append(existing)
        merged = {int(sample[0]): sample for sample in existing['samples']}
        merged.update(samples)
        existing['samples'] = list(merged.values())

    # A shape record can refer to an owner that was removed by a previous
    # conflict.  Keep only records that still have a concrete projection row.
    valid_faces = {int(sample[0]) for region in regions for sample in region['samples']}
    for item in shape_details:
        item['accepted_faces'] = [face for face in item['accepted_faces'] if face in valid_faces]
        item['rejected_faces'] = sorted(set(item['rejected_faces']) - set(item['accepted_faces']))
        item['nested_faces'] = [face for face in item.get('nested_faces', [])
                                if face in set(item['accepted_faces'])]
    regions = [region for region in regions if region['samples']]
    for region in regions:
        region['samples'] = sorted(region['samples'], key=lambda sample: sample[0])
    projection['regions'] = sorted(regions, key=lambda item: (item['subject_id'], item['label']))


def _sync_shape_details(projection, feature_details, shape_details, audit):
    shapes = {(item['subject_id'], item['label']): item for item in shape_details}
    result = []
    for feature in feature_details:
        shape = shapes.get((feature['subject_id'], feature['label']))
        if shape is None or not shape['accepted_faces']:
            continue
        feature = dict(feature, faces=list(shape['accepted_faces']), iris_faces=list(shape.get('nested_faces', [])))
        if 'anchor_paths' in feature:
            feature['anchor_paths'] = [path for path in feature['anchor_paths'] if path[0] in set(feature['faces'])]
        result.append(feature)
    for row in audit:
        shape = shapes.get((row['subject_id'], row['label']))
        if shape is not None:
            row.update(final_accepted_count=len(shape['accepted_faces']), final_nested_count=len(shape.get('nested_faces', [])),
                       rejected_faces=list(shape['rejected_faces']), reasons=list(shape['reasons']),
                       owner_filter_removed_count=max(0, row['topology_completed_count'] - len(shape['accepted_faces'])))
    return result


def analyze(source, native_packet, source_sha256, model_loader, on_view=None, cancelled=None, refine_brows=True, progress=None):
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
    if progress: progress("recognizing", "加载离线模型权重")
    torch, detector, parser, weights = model_loader()
    checkpoint()
    supported = np.array([i for i, name in enumerate(LABEL_NAMES) if name in SUPPORTED_LABELS])
    cameras = coarse_cameras(vertices)
    observations, reports, candidates = [], [], []
    eye_observations, face_observations = [], []
    body_observations = []
    parent_observations = []
    body_failed = False
    observation_pixels = 0
    visible_surface = np.zeros(len(faces), dtype=bool)
    ambiguous = ambiguous_triangles(vertices, faces)
    sided = render.double_sided_faces(materials, material_ids)

    def process_view(camera, allow_focus, family, parent_only=False):
        nonlocal observation_pixels, body_failed
        checkpoint()
        ids, depths, bary = render.raster(vertices, faces, camera.basis, camera.center,
                                           camera.half_height, camera.size, sided)
        visible = ids >= 0
        ids[visible & ambiguous[np.maximum(ids, 0)]] = -1
        if not parent_only:
            visible_surface[ids[ids >= 0]] = True
        checkpoint()
        projected = render.project(vertices, camera.basis, camera.center, camera.half_height, camera.size)
        rgb = render.shade(faces, uv, colors, materials, material_ids, ids, bary, projected)
        local_body = None
        if (allow_focus or parent_only) and not body_failed and getattr(parser, 'body_regions', None) is not None:
            try:
                local_body = body_regions.observe(parser.body_regions, rgb, ids, family)
                if not parent_only:
                    body_observations.append(local_body)
            except Exception:
                if parent_only:
                    raise  # A partial supplement must not claim a complete parent pass.
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
                    if not parent_only:
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
        if keep and not parent_only and getattr(parser, 'eye_landmarks', None) is not None:
            try:
                eye_observations.extend(eye_landmarks.observe(parser.eye_landmarks, rgb, ids))
                source_pixels = render.source_texture_projection(faces, uv, materials, material_ids, ids, bary, projected)
                face_observations.extend(face_landmarks.observe(parser.eye_landmarks, rgb, ids, bary, vertices, faces, camera, family, source_pixels))
            except Exception:
                pass  # Missing eye hints must not discard valid raw semantics.
        from local_parent_ownership import observe as observe_parents
        body = local_body if parent_only else next((row for row in body_observations if row[0] == family), None) if allow_focus else None
        parent_observations.append(observe_parents(camera, family, rgb, ids, depths, bary, labels, confidence, body))
        if on_view is not None:
            on_view(report, rgb, ids, depths, bary, labels, confidence)
        reports.append(report)
        if progress: progress("ownership" if parent_only else "recognizing", "完成视角 " + camera.name, len(reports), 0)

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
    if progress: progress("ownership", "融合独立视角与可见区域归属")
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
                source_pixels = render.source_texture_projection(faces, uv, materials, material_ids, ids, bary, projected)
                face_observations.extend(face_landmarks.observe(parser.eye_landmarks, rgb, ids, bary, vertices, faces, camera, camera.name, source_pixels))
            except Exception:
                pass  # Optional shape aids must not discard accepted semantics.
    shape_audit = []
    feature_details, shape_details = face_landmarks.associate(
        face_observations, result['regions'], vertices, faces, with_shapes=True, audit=shape_audit, refine_brows=refine_brows)
    _apply_shape_parent_fallback(result, shape_details)
    feature_details = _sync_shape_details(result, feature_details, shape_details, shape_audit)
    eyes = [{'subject_id': item['subject_id'], 'label': item['label'],
             'aperture_faces': list(item['accepted_faces']), 'iris_faces': list(item.get('nested_faces', []))}
            for item in shape_details if item['label'] in ('le', 're') and item.get('nested_faces')]
    if not shape_details:
        eyes = eye_landmarks.associate(eye_observations, result['regions'])
    parent_details = []
    if body_observations:
        blocked = {f for hint in feature_details for f in hint['faces']}
        blocked.update(f for hint in feature_details for path in hint.get('anchor_paths', []) for f in path)
        result = body_regions.supplement(result, body_observations, vertices, faces, blocked)
        result['statistics'].update(visible_faces=int(visible_surface.sum()),
                                    unseen_faces=int(len(faces)-visible_surface.sum()))
        blocked.update(f for shape in shape_details for f in shape.get('rejected_faces', []))
        parent_details = body_regions.parent_details(result, body_observations, vertices, faces, blocked)
    checkpoint()
    if progress: progress("ownership", "校验五官边界与父级覆盖", 1, 3)
    contour_proposal = None
    contour_diagnostic = ''
    try:
        from local_contour_proposals import build as build_contours
        contour_proposal = build_contours(face_observations, result['regions'], shape_details,
                                         vertices, faces, cancelled, progress=progress)
    except Exception as error:
        contour_diagnostic = type(error).__name__
    if progress: progress("ownership", "建立连续裁切与父级证据", 2, 3)
    parent_proposal = None
    parent_diagnostic = ''
    try:
        # Add evidence around the observed head without changing the already
        # established face/eye/lip projection or its shape proposals.
        supplemental = parent_cameras(vertices, faces, result['regions'])
        available = max(0, 16-len(parent_observations))
        if len(supplemental) > available:
            parent_diagnostic = 'parent_local_view_budget_exhausted'
        for camera in supplemental[:available]:
            checkpoint()
            if progress: progress("ownership", "补充局部独立视角 " + camera.name)
            process_view(camera, False, camera.name, parent_only=True)
        if file_sha256(source) != source_sha256:
            raise ValueError('Semantic source changed during parent analysis')
        from local_parent_ownership import build as build_parents
        parent_proposal = build_parents(parent_observations, result['regions'], parent_details, shape_details,
            vertices, faces, uv, colors, materials, material_ids, cancelled, progress=progress)
    except Exception as error:
        parent_diagnostic = type(error).__name__ + ':' + str(error)[:160]
    checkpoint()
    if progress: progress("ownership", "区域证据生成完成，等待原生校验", 3, 3)
    runtime = {'python': sys.version.split()[0], 'bits': struct.calcsize('P')*8,
               'packages': {name: importlib.metadata.version(name) for name in ('torch','torchvision','pyfacer','numpy','Pillow')}}
    return {'projection': result, 'eye_details': eyes,
            'parent_details': parent_details, 'parent_proposal': parent_proposal, 'parent_diagnostic': parent_diagnostic,
            'feature_details': feature_details, 'shape_details': shape_details, 'shape_audit': shape_audit,
            'contour_proposal': contour_proposal, 'contour_diagnostic': contour_diagnostic,
            'vertices': vertices, 'faces': faces, 'render_geometry_id': rendered_id,
            'runtime': runtime, 'render_visible_faces': int(visible_surface.sum()),
            'render_unseen_faces': int(len(faces)-visible_surface.sum()),
            'source_sha256': source_sha256, 'weights': weights, 'views': reports, 'policy_version': POLICY_VERSION}
