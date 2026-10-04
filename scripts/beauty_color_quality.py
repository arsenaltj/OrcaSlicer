"""Offline native-face color diagnosis, not a visual/print acceptance gate.

Consumes --quality exports from run_beauty_model_regression.py. Missing human
targets, corrections and semantic annotations remain unavailable, never inferred.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import sys

import numpy as np
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools/ai'))
# Reuse the existing production conversion/distance; do not invent a scoring space.
from printable_image_pipeline import _srgb_to_lab, _ciede2000


def _lab(rgb):
    return np.asarray([_srgb_to_lab(tuple(c*255)) for c in rgb])

LAYOUT = {'vertices.f32': ('<f4', 3), 'triangles.i32': ('<i4', 3),
          'source.f32': ('<f4', 3), 'automatic.f32': ('<f4', 3),
          'areas.f64': ('<f8', 1), 'pieces.u32': ('<u4', 1)}


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def load_export(path):
    path = Path(path)
    meta = json.loads(path.read_text(encoding='utf-8'))
    if meta.get('schema') != 1 or set(meta.get('files', {})) != set(LAYOUT):
        raise ValueError('Unsupported or incomplete native export')
    for key in ('source_sha256', 'loaded_mesh_colors_sha256', 'output_sha256'):
        if not re.fullmatch('[0-9a-f]{64}', meta.get(key, '')):
            raise ValueError('Missing export identity')
    arrays = {}
    for name, (dtype, columns) in LAYOUT.items():
        spec = meta['files'][name]
        shape = spec['shape']
        if (spec['dtype'] != dtype or not isinstance(shape, list) or len(shape) != 2
                or any(type(n) is not int for n in shape) or shape[1] != columns
                or not 1 <= shape[0] <= (6000000 if name == 'vertices.f32' else 2000000)):
            raise ValueError('Invalid array layout')
        file = path.parent / name
        size = int(np.prod(shape)) * np.dtype(dtype).itemsize
        if spec['bytes'] != size or file.stat().st_size != size or digest(file) != spec['sha256']:
            raise ValueError('Native array size or hash mismatch')
        arrays[name] = np.fromfile(file, dtype=dtype).reshape(shape)
        if not np.isfinite(arrays[name]).all():
            raise ValueError('Non-finite native data')
    count = len(arrays['triangles.i32'])
    if any(len(arrays[name]) != count for name in LAYOUT if name != 'vertices.f32'):
        raise ValueError('Face arrays do not share the same order/count')
    faces = arrays['triangles.i32']
    if np.any(faces < 0) or np.any(faces >= len(arrays['vertices.f32'])):
        raise ValueError('Triangle outside vertex range')
    return meta, arrays


def weighted_stats(values, weights):
    positive = weights > 0
    values, weights = values[positive], weights[positive]
    order = np.argsort(values, kind='stable')
    cumulative = np.cumsum(weights[order])
    quantile = lambda q: float(values[order[min(np.searchsorted(cumulative, q*cumulative[-1]), len(order)-1)]])
    return {'area_weighted_mean': float(np.average(values, weights=weights)),
            'area_weighted_p50': quantile(.5), 'area_weighted_p95': quantile(.95)}


def diagnose(source, automatic, areas, pieces, palette):
    source, automatic, palette = [np.asarray(a, dtype=np.float64) for a in (source, automatic, palette)]
    areas, pieces = np.asarray(areas).reshape(-1), np.asarray(pieces).reshape(-1)
    n = len(areas)
    if (not n or source.shape != (n, 3) or automatic.shape != (n, 3) or pieces.shape != (n,)
            or palette.ndim != 2 or palette.shape[1] != 3 or not 1 <= len(palette) <= 6
            or not np.issubdtype(pieces.dtype, np.integer)):
        raise ValueError('Invalid color/area/region shapes')
    for a in (source, automatic, palette):
        if not np.isfinite(a).all() or np.any(a < 0) or np.any(a > 1):
            raise ValueError('Expected finite sRGB in [0,1]')
    if not np.isfinite(areas).all() or np.any(areas < 0) or areas.sum() <= 0:
        raise ValueError('Expected positive total surface area')
    # Identical native colors recur across faces; evaluate each exact RGB once.
    # No quantization or sample subsampling is introduced for speed.
    unique, inverse_colors = np.unique(source, axis=0, return_inverse=True)
    source_lab, palette_lab = _lab(unique), _lab(palette)
    distances = np.asarray([[_ciede2000(c, p) for p in palette_lab] for c in source_lab])
    nearest = distances.min(axis=1)[inverse_colors]
    assigned = np.zeros(n, dtype=np.int32)
    available = np.zeros(n, dtype=bool)
    for index, rgb in enumerate(palette):
        matches = np.all(np.abs(automatic-rgb) <= 1e-6, axis=1)
        assigned[matches] = index
        available |= matches
    if not available.all():
        raise ValueError('Automatic output contains colors outside supplied palette')
    error = distances[inverse_colors, assigned]
    extra = np.maximum(error-nearest, 0)
    ids, inverse = np.unique(pieces, return_inverse=True)
    totals = np.bincount(inverse, weights=areas)
    sums = np.bincount(inverse, weights=areas*error)
    excess = np.bincount(inverse, weights=areas*extra)
    ranking = sorted((i for i in range(len(ids)) if totals[i] > 0), key=lambda i: excess[i], reverse=True)
    return {'metric': 'CIEDE2000 / Lab D65; native face-mean sRGB; geometry-area weighted',
            'automatic_vs_source': weighted_stats(error, areas),
            'independent_face_nearest_palette_bound': weighted_stats(nearest, areas),
            'extra_vs_independent_face_bound': weighted_stats(extra, areas),
            'zero_area_faces_excluded': int(np.count_nonzero(areas == 0)),
            'regions': [{'piece_id': int(ids[i]), 'semantic_label': None,
                         'area_fraction': float(totals[i]/areas.sum()),
                         'mean_delta_e00': float(sums[i]/totals[i]),
                         'mean_extra_delta_e00': float(excess[i]/totals[i])} for i in ranking[:12]],
            'interpretation': 'Nearest-face bound ignores region coherence and user intent; excess is diagnostic, not an algorithm defect score.'}


def constrain_region_palette(distances, areas, baseline_pieces, candidate_pieces,
                             baseline_slots, proposed_slots):
    """Offline automatic-only candidate; preserve source error on stationary faces.

    Slot arguments index columns in the frozen palette distance matrix. This is
    not a manual-paint/target persistence policy or a production matching entry.
    Also diagnose whether *all* faces can be protected with one color per region.
    """
    distances = np.asarray(distances, dtype=float)
    areas = np.asarray(areas, dtype=float).reshape(-1)
    labels = [np.asarray(a).reshape(-1) for a in
              (baseline_pieces, candidate_pieces, baseline_slots, proposed_slots)]
    n = len(areas)
    if (not n or distances.ndim != 2 or distances.shape[0] != n
            or not 1 <= distances.shape[1] <= 6
            or not np.isfinite(distances).all() or np.any(distances < 0)
            or not np.isfinite(areas).all() or np.any(areas < 0) or areas.sum() <= 0
            or any(a.shape != (n,) or not np.issubdtype(a.dtype, np.integer) for a in labels)):
        raise ValueError('Invalid region/palette constraints')
    before, after, old_slots, proposed = labels
    channels = distances.shape[1]
    if any(np.any(a < 0) for a in (before, after, old_slots, proposed)) or any(
            np.any(a >= channels) for a in (old_slots, proposed)):
        raise ValueError('Invalid region ID or palette index')
    for pieces, slots in ((before, old_slots), (after, proposed)):
        ids, inv = np.unique(pieces, return_inverse=True)
        low, high = np.full(len(ids), channels), np.full(len(ids), -1)
        np.minimum.at(low, inv, slots); np.maximum.at(high, inv, slots)
        if np.any(low != high):
            raise ValueError('Expected one color per region')
    ids, inv = np.unique(after, return_inverse=True)
    stationary = (before == after) & (areas > 0)
    original_error = distances[np.arange(n), old_slots]
    worst_stationary = np.full((len(ids), channels), -np.inf)
    worst_all = np.full((len(ids), channels), -np.inf)
    costs = np.zeros((len(ids), channels))
    for channel in range(channels):
        difference = distances[:, channel] - original_error
        np.maximum.at(worst_stationary[:, channel], inv[stationary], difference[stationary])
        positive = areas > 0
        np.maximum.at(worst_all[:, channel], inv[positive], difference[positive])
        costs[:, channel] = np.bincount(inv, weights=areas*distances[:, channel], minlength=len(ids))
    epsilon = 1e-7  # Floating-point tolerance, not a perceptual allowance.
    allowed = worst_stationary <= epsilon
    if not allowed.any(axis=1).all():
        raise ValueError('Stationary-region baseline must supply a feasible color')
    choices = np.argmin(np.where(allowed, costs, np.inf), axis=1)
    # Retain the proposed color on an exact cost tie.
    _, first = np.unique(after, return_index=True)
    previous = proposed[first]
    tie = allowed[np.arange(len(ids)), previous] & (costs[np.arange(len(ids)), previous] == costs[np.arange(len(ids)), choices])
    choices[tie] = previous[tie]
    assigned = choices[inv]
    selected_error = distances[np.arange(n), assigned]
    infeasible = ~np.any(worst_all <= epsilon, axis=1)
    return assigned, {
        'status': 'offline_candidate_not_accepted',
        'stationary_positive_area_faces': int(stationary.sum()),
        'stationary_max_error_increase': float(np.max(selected_error[stationary]-original_error[stationary])) if stationary.any() else None,
        'all_faces_infeasible_regions': [int(i) for i in ids[infeasible]],
        'all_faces_infeasible_area_fraction': float(areas[infeasible[inv]].sum()/areas.sum()),
        'changed_region_colors': int(np.count_nonzero(choices != previous)),
        'limits': 'Frozen physical palette, no manual targets; source-error constraint does not establish visual/print quality.'}


def renderer_module(path=None):
    source = ROOT / 'tools/ai/local_semantic_render.py'
    path = Path(path) if path else source
    if digest(path) != digest(source):
        raise ValueError('Renderer must match the current repository implementation')
    spec = importlib.util.spec_from_file_location('quality_renderer', path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module, {'source_sha256': digest(path),
                    'accelerator_sha256': digest(path.with_name('local_semantic_raster.dll'))
                    if path.with_name('local_semantic_raster.dll').exists() else None}


def render_sheet(arrays, renderer, output, size=256):
    vertices, faces = arrays['vertices.f32'], arrays['triangles.i32']
    lo, hi = vertices.min(0), vertices.max(0)
    center = (lo.astype(float)+hi)/2
    half = float(np.max(hi.astype(float)-lo)*.56)
    if half <= 0:
        raise ValueError('Cannot render zero-size geometry')
    bases = [np.array([right, [0, 0, 1], depth], dtype=float) for right, depth in
             [([1, 0, 0], [0, -1, 0]), ([0, 1, 0], [1, 0, 0]),
              ([-1, 0, 0], [0, 1, 0]), ([0, -1, 0], [-1, 0, 0])]]
    sheet = Image.new('RGB', (size*4, (size+30)*4), '#eeeeee')
    draw = ImageDraw.Draw(sheet)
    titles = ['Source face mean', 'User target: MISSING', 'Automatic physical slots', 'Manual correction: MISSING']
    for row, basis in enumerate(bases):
        ids, _, _ = renderer.raster(vertices, faces, basis, center, half, size,
                                    np.ones(len(faces), dtype=bool))
        valid = ids >= 0
        for column, name in ((0, 'source.f32'), (2, 'automatic.f32')):
            pixels = np.full((size, size, 3), 238, dtype=np.uint8)
            pixels[valid] = np.rint(arrays[name][ids[valid]]*255).astype(np.uint8)
            sheet.paste(Image.fromarray(pixels), (column*size, row*(size+30)+30))
        for column, title in enumerate(titles):
            draw.text((column*size+5, row*(size+30)+5), f'{row+1}: {title}', fill='#202020')
        for column in (1, 3):
            draw.text((column*size+15, row*(size+30)+size//2), 'No verified human reference', fill='#666666')
    sheet.save(output)
    return {'size': size, 'basis': [b.tolist() for b in bases], 'center': center.tolist(),
            'half_height': half, 'lighting': 'unlit', 'double_sided': True,
            'color': 'flat native face-mean sRGB, no texture filtering or shading',
            'directions': ['-Y', '+X', '+Y', '-X'], 'orientation': 'native coordinates, semantic front not inferred'}


def run(manifest, output, renderer_path=None):
    output = Path(output)
    output.mkdir(parents=True, exist_ok=False)
    meta, arrays = load_export(manifest)
    channels = meta['palette']
    if not channels or any(not c['compatible'] for c in channels):
        raise ValueError('Expected compatible physical channels')
    palette = []
    for c in channels:
        if not re.fullmatch('#[0-9a-fA-F]{6}', c['color']):
            raise ValueError('Invalid palette color')
        palette.append([int(c['color'][i:i+2], 16)/255 for i in (1, 3, 5)])
    metrics = diagnose(arrays['source.f32'], arrays['automatic.f32'], arrays['areas.f64'], arrays['pieces.u32'], palette)
    renderer, renderer_identity = renderer_module(renderer_path)
    camera = render_sheet(arrays, renderer, output/'comparison.png')
    result = {'status': 'diagnostic_only', 'manifest_sha256': digest(manifest),
              'source_sha256': meta['source_sha256'], 'native_output_sha256': meta['output_sha256'],
              'evaluator_sha256': digest(__file__),
              'color_math_sha256': digest(ROOT/'tools/ai/printable_image_pipeline.py'),
              'palette': channels, 'camera': camera,
              'renderer': renderer_identity, 'metrics': metrics,
              'groups': {'original': 'native face-mean approximation', 'target': None,
                         'automatic': meta['automatic_method'], 'manual_correction': None},
              'semantic_regions': {'skin': None, 'lips': None, 'hair': None, 'clothes': None},
              'limits': ['No verified human target/correction or semantic annotations',
                         'Software palette, not measured filament colors',
                         'Source face means are not original texture pixels or intrinsic albedo',
                         'No GUI, slicing, unseen-set or physical-print acceptance']}
    # Recheck all native payloads after analysis; never bless a changing input.
    after, _ = load_export(manifest)
    if after != meta:
        raise ValueError('Native export changed during evaluation')
    (output/'report.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--manifest', required=True)
    parser.add_argument('--output', required=True)
    parser.add_argument('--renderer-module', help='Exact copy of repository renderer beside its built accelerator')
    args = parser.parse_args()
    result = run(args.manifest, args.output, args.renderer_module)
    print(result['status'])
