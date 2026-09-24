"""Offline material-color study for the fixed 100 mm portrait, not a default.

Manual geometric feature windows + original GLB texture chroma distinguish
material from baked shading. No provider calls, geometry edits, or new palette.
The sample hashes deliberately reject unreviewed models/manual-paint projects.
"""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import re
import sys
import time
import xml.etree.ElementTree as ET
import zipfile

import numpy as np
from color_cleanup_minimal import CODES, ENCODE

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools' / 'ai'))
from local_semantic_render import load, shade

BASELINE_HASH = '2d5e9d371573c54a7296d386ad9419cb2c085d4f2c249a9c8a94f46ffe532f38'
PALETTE = ['#F7E2DA', '#282629', '#F6F7F9', '#EA9A92', '#668CB6', '#958B86']


def inside_polygon(points, polygon):
    """Ray crossing in front-view millimetres (y,z), including concave polygons."""
    x, y = points.T
    inside = np.zeros(len(points), bool)
    previous = polygon[-1]
    for current in polygon:
        x0, y0 = previous; x1, y1 = current
        if y0 != y1:
            inside ^= ((y0 > y) != (y1 > y)) & (x < (x1-x0)*(y-y0)/(y1-y0)+x0)
        previous = current
    return inside


def material_labels(centers, rgb, baseline, profile, feature_masks=None):
    """Sample-specific material hypotheses, using chroma rather than darkness.

    Features are interpreted separately; their labels are locked during later
    neighbour cleanup. The windows are manual, not AI semantic segmentation.
    """
    x, y, z = centers.T
    r, g, b = rgb.astype(float).T
    light = rgb.mean(axis=1)
    warm = (r > 1.09*g) & (g > 1.07*b) & (r-g > 10) & (g-b > 5)
    skin_zone = (z > 4) | ((z > -26) & (z < -7) & (x > 5))
    skin = warm & skin_zone & (light > 65)
    # White coat stays one material even where its texture contains grey folds.
    labels = np.full(len(centers), 2, np.uint8)
    labels[skin] = 0
    head = z > 23
    labels[head] = np.where(skin[head], 0, 1)
    # Neutral bright skin-edge texels should not become black hair.
    labels[head & (light > 155)] = 0
    blouse = (z > -11) & (z < 15.5) & (np.abs(y) < 8.3) & (x > 5) & (light < 150) & ~skin
    labels[blouse] = 1
    labels[z < profile['base_top_z_mm']] = 5
    lock = np.zeros(len(labels), bool)
    counts = {}
    for feature in profile['features']:
        mask = feature_masks[feature['name']] if feature_masks is not None else (
            inside_polygon(centers[:, 1:], feature['polygon_yz_mm']) & (x > feature['min_x_mm']))
        kind = feature['kind']
        if kind in ('brow', 'eye'):
            labels[mask] = 0
            labels[mask & (light < feature['dark_threshold'])] = 1
            if kind == 'eye':
                labels[mask & (baseline == 0)] = 2
        elif kind == 'mouth':
            labels[mask] = 0
            # Skin also has R>G>B. Require additional red excess to avoid
            # painting the entire manual mouth window pink.
            lip = mask & (r-g > 50) & ((r-g) > 1.8*(g-b)+25)
            labels[lip] = 3
            labels[mask & (light < 75)] = 1
            labels[mask & (baseline == 0)] = 2
        elif kind == 'nostrils':
            labels[mask & (baseline == 5)] = 5
            labels[mask & (light < 95)] = 1
        elif kind == 'watch':
            labels[mask & ~skin] = 5
            labels[mask & (light < 60)] = 1
        else:
            raise ValueError('Unknown manual feature')
        lock |= mask
        counts[feature['name']] = int(mask.sum())
    return labels, lock, counts


def neighbours(faces, vertex_count):
    edges = np.sort(faces[:, [[0, 1], [1, 2], [2, 0]]].reshape(-1, 2), axis=1)
    owners = np.repeat(np.arange(len(faces)), 3)
    key = edges[:, 0].astype(np.int64)*vertex_count+edges[:, 1]
    order = np.argsort(key, kind='stable'); key = key[order]; owners = owners[order]
    starts = np.r_[0, np.flatnonzero(np.diff(key))+1]
    counts = np.diff(np.r_[starts, len(key)])
    paired = starts[counts == 2]
    return owners[paired], owners[paired+1]


def remove_isolated_faces(labels, lock, a, b, iterations=3):
    """Only unanimous 3-neighbour outliers; no erosion of material boundaries.

    Locked features and open/nonmanifold faces cannot be changed. Decisions in
    each round are simultaneous, never dependent on triangle traversal order.
    """
    result = labels.copy()
    degree = np.bincount(np.r_[a, b], minlength=len(labels))
    for _ in range(iterations):
        votes = np.zeros((len(labels), 6), np.uint8)
        np.add.at(votes, (a, result[b]), 1)
        np.add.at(votes, (b, result[a]), 1)
        use = (degree == 3) & (votes.max(axis=1) == 3) & ~lock
        result[use] = votes[use].argmax(axis=1)
    return result


def validate_alignment(glb_vertices, glb_faces, vertices, faces):
    if len(glb_faces) != len(faces): raise ValueError('Source face count differs')
    aligned = glb_vertices-(glb_vertices.min(axis=0)+glb_vertices.max(axis=0))/2
    # No guessed face correspondence or nearest-neighbour UV transfer allowed.
    error = float(np.max(np.abs(aligned[glb_faces]-vertices[faces])))
    if error > 1e-4: raise ValueError(f'Source face/corner geometry mismatch: {error} mm')
    return error


def export_painted_project(archive, model_name, paint_labels, path):
    """Save and verify an independent project without mutating ZipInfo inputs."""
    xml = archive.read(model_name)
    index = iter(paint_labels.tolist())
    updated = re.sub(rb'paint_color="[^"]*"', lambda m: ('paint_color="'+ENCODE[next(index)]+'"').encode(), xml)
    if next(index, None) is not None: raise ValueError('Paint count exceeds source triangles')
    with zipfile.ZipFile(path, 'w', compression=zipfile.ZIP_DEFLATED) as out:
        for item in archive.infolist():
            if item.filename.startswith('Metadata/') and item.filename.endswith('.png'): continue
            out.writestr(copy.copy(item), updated if item.filename == model_name else archive.read(item.filename))
    with zipfile.ZipFile(path) as check:
        stripped = lambda data: re.sub(rb'paint_color="[^"]*"', b'paint_color=""', data)
        assert stripped(check.read(model_name)) == stripped(xml)
        restored = ET.fromstring(check.read(model_name)).find('.//{*}triangles')
        assert np.array_equal(paint_labels, [CODES[t.get('paint_color')] for t in restored])
        for item in archive.infolist():
            if item.filename == model_name or (item.filename.startswith('Metadata/') and item.filename.endswith('.png')): continue
            assert check.read(item.filename) == archive.read(item.filename)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('source', type=Path)
    parser.add_argument('glb', type=Path)
    parser.add_argument('profile', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--refine-facial-details', action='store_true')
    parser.add_argument('--automatic-feature-regions', type=Path)
    args = parser.parse_args()
    if args.automatic_feature_regions and not args.refine_facial_details:
        parser.error('--automatic-feature-regions requires --refine-facial-details')
    started = time.monotonic()
    profile = json.loads(args.profile.read_text(encoding='utf-8'))
    source_hash = hashlib.sha256(args.source.read_bytes()).hexdigest()
    glb_hash = hashlib.sha256(args.glb.read_bytes()).hexdigest()
    if source_hash != BASELINE_HASH or glb_hash != profile['glb_sha256']:
        raise ValueError('Only the reviewed baseline and source GLB are supported')
    args.output.mkdir(parents=True, exist_ok=False)
    with zipfile.ZipFile(args.source) as archive:
        meshes = []
        for name in archive.namelist():
            if name.endswith('.model'):
                meshes.extend((name, m) for m in ET.fromstring(archive.read(name)).findall('.//{*}mesh'))
        if len(meshes) != 1: raise ValueError('Exactly one mesh required')
        name, mesh = meshes[0]
        vertices = np.array([[float(v.get(k)) for k in ('x','y','z')] for v in mesh.find('{*}vertices')], np.float32)
        faces = np.array([[int(t.get(k)) for k in ('v1','v2','v3')] for t in mesh.find('{*}triangles')], np.int32)
        baseline = np.array([CODES[t.get('paint_color')] for t in mesh.find('{*}triangles')], np.uint8)
        palette = json.loads(archive.read('Metadata/project_settings.config'))['filament_colour']
        if palette != PALETTE: raise ValueError('Fixed six-filament palette required')
        sv, sf, uv, colors, materials, mi = load(args.glb)
        alignment_error = validate_alignment(sv, sf, vertices, faces)
        rgb = shade(sf, uv, colors, materials, mi, np.arange(len(sf)), np.full((len(sf), 3), 1/3))
        print('Source texture aligned; max corner error:', alignment_error, flush=True)
        tri = vertices[faces]; centers = tri.mean(axis=1)
        labels, lock, features = material_labels(centers, rgb, baseline, profile)
        a, b = neighbours(faces, len(vertices))
        cleaned = remove_isolated_faces(labels, lock, a, b)
        assert np.array_equal(cleaned[lock], labels[lock])
        isolated_count=int(np.count_nonzero(cleaned!=labels))
        previous = cleaned.copy()
        refinement = None
        automatic = None
        if args.refine_facial_details:
            from color_feature_refine import refine_details
            masks = {f['name']: inside_polygon(centers[:,1:],f['polygon_yz_mm']) & (centers[:,0] > f['min_x_mm'])
                     for f in profile['features']}
            cleaned, refinement = refine_details(vertices,faces,rgb,previous,masks,profile,a,b)
        if args.automatic_feature_regions:
            from color_automatic_features import load_features
            from local_semantic_geometry import geometry_fingerprint
            previous=cleaned.copy()  # last accepted manual-feature version for comparison only
            auto_features,auto_masks,automatic=load_features(args.automatic_feature_regions,glb_hash,
                geometry_fingerprint(sv,sf),len(faces),centers,(a,b))
            # Body/watch still use the fixed sample policy. No manual eyes,
            # brows, lips or nose windows may feed the automatic result.
            body_features=[f for f in profile['features'] if f['kind']=='watch']
            auto_masks.update({f['name']:inside_polygon(centers[:,1:],f['polygon_yz_mm']) &
                (centers[:,0]>f['min_x_mm']) for f in body_features})
            auto_profile={**profile,'features':auto_features+body_features}
            auto_labels,auto_lock,_=material_labels(centers,rgb,baseline,auto_profile,auto_masks)
            auto_labels=remove_isolated_faces(auto_labels,auto_lock,a,b)
            cleaned,refinement=refine_details(vertices,faces,rgb,auto_labels,auto_masks,auto_profile,a,b)
            refinement['local_filter_changed_area_mm2']=refinement['changed_area_mm2']
        export_painted_project(archive,name,cleaned,args.output/'cleaned.3mf')
        same_major_colors = np.array([3,1,0,3,4,5], dtype=np.uint8)[cleaned]
        export_painted_project(archive,name,same_major_colors,args.output/'cleaned-original-colors.3mf')
    cross = np.cross(tri[:, 1]-tri[:, 0], tri[:, 2]-tri[:, 0])
    area = np.linalg.norm(cross, axis=1)/2
    normals = np.zeros_like(vertices)
    for corner in range(3): np.add.at(normals, faces[:, corner], cross)
    normals /= np.maximum(np.linalg.norm(normals, axis=1, keepdims=True), 1e-15)
    compare_labels = (np.repeat(baseline,3),np.repeat(previous,3),np.repeat(cleaned,3)) if refinement else (np.repeat(baseline,3),np.repeat(cleaned,3))
    buffer = np.column_stack((tri.reshape(-1,3), normals[faces].reshape(-1,3),
                              *compare_labels, np.repeat(rgb/255,3,axis=0))).astype('<f4')
    (args.output/'mesh.bin').write_bytes(buffer.tobytes())
    np.savez_compressed(args.output/'labels.npz', baseline=baseline, previous=previous, cleaned=cleaned,
                        feature_lock=auto_lock if automatic else lock)
    assert hashlib.sha256(args.source.read_bytes()).hexdigest() == source_hash
    assert hashlib.sha256(args.glb.read_bytes()).hexdigest() == glb_hash
    stats = {'changed_faces':int(np.count_nonzero(cleaned != baseline)),
             'changed_area_mm2':float(area[cleaned != baseline].sum()), 'surface_area_mm2':float(area.sum()),
             'gray_skin_faces_before':int(np.count_nonzero((baseline == 5) & (cleaned == 0))),
             'gray_to_white_faces':int(np.count_nonzero((baseline == 5) & (cleaned == 2))),
             'isolated_faces_cleaned':isolated_count,
             'feature_windows_faces':features,
             'material_area_mm2':{str(i):float(area[cleaned == i].sum()) for i in np.unique(cleaned)}}
    stats['cleanup_only_changed_area_mm2'] = float(area[same_major_colors != baseline].sum())
    stats['gray_to_skin_area_mm2'] = float(area[(baseline == 5) & (cleaned == 0)].sum())
    stats['gray_to_white_area_mm2'] = float(area[(baseline == 5) & (cleaned == 2)].sum())
    report = {'experiment':'sample-material-distillation-v1', 'source_3mf_sha256':source_hash,
              'glb_sha256':glb_hash, 'profile':profile, 'palette':palette, 'vertex_stride':11,
              'vertices':len(vertices), 'faces':len(faces),
              'bounds':[vertices.min(axis=0).tolist(), vertices.max(axis=0).tolist()],
              'checks':{'geometry_unchanged':True, 'source_unchanged':True, 'paint_roundtrip':True,
                        'other_project_members_unchanged':True, 'feature_smoothing_locks':True,
                        'source_corner_alignment_max_error_mm':alignment_error},
              'statistics':stats, 'seconds':time.monotonic()-started,
              'limits':['Fixed sample: manual feature windows and material-to-filament choices, not general semantic AI.',
                        'Face-centroid texture sampling; no continuous texture reconstruction.',
                        'No native import/slice/physical print validation; no new default algorithm.',
                        'Six physical colors unchanged; dark green absent, blouse uses black.']}
    if refinement:
        report.update(experiment='sample-feature-refinement-v1',vertex_stride=12,refinement=refinement)
        report['checks']['outside_feature_windows_unchanged'] = True
        report['checks']['feature_smoothing_locks'] = 'Initial material pass only; intentional facial refinement follows'
    if automatic:
        report.update(experiment='automatic-facial-regions-v1',automatic=automatic)
        report['refinement']['changed_area_mm2']=float(area[cleaned!=previous].sum())
        report['checks']['outside_feature_windows_unchanged']='Not claimed: automatic and manual feature extents differ'
        report['statistics']['manual_reference_feature_faces']=report['statistics'].pop('feature_windows_faces')
        report['statistics']['reference_isolated_faces_cleaned']=report['statistics'].pop('isolated_faces_cleaned')
        report['statistics']['automatic_candidate_feature_faces']=automatic['feature_faces']
        changed=cleaned!=previous
        report['automatic']['changed_bounds_mm']=[centers[changed].min(axis=0).tolist(),centers[changed].max(axis=0).tolist()] if changed.any() else []
        report['profile']['features']=[f for f in profile['features'] if f['kind']=='watch']
        report['limits'][0]='Automatic facial regions; body/watch material rules and filament choices remain sample-specific.'
    report['implementation_sha256'] = {name:hashlib.sha256(Path(__file__).with_name(name).read_bytes()).hexdigest()
        for name in ['color_material_distill.py','color_feature_refine.py','color_automatic_features.py','portrait_material_profile.json',
                     'color_feature_compare.html' if refinement else 'color_material_compare.html']}
    (args.output/'result.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
    template = 'color_feature_compare.html' if refinement else 'color_material_compare.html'
    (args.output/'index.html').write_text(Path(__file__).with_name(template).read_text(encoding='utf-8'),encoding='utf-8')
    print(json.dumps({'statistics':stats,'seconds':report['seconds']},ensure_ascii=False),flush=True)


if __name__ == '__main__': main()
