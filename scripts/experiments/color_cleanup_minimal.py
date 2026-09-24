"""Offline A/B prototype on a real Orca import; never edits product defaults.

Only supports one mesh with unsplit physical-filament face labels and a
translation-only build transform. Requires numpy; preserves mesh geometry.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import time
import xml.etree.ElementTree as ET
import zipfile

import numpy as np

CODES = {"4": 0, "8": 1, "0C": 2, "1C": 3, "2C": 4, "3C": 5}
ENCODE = {v: k for k, v in CODES.items()}


def cleanup(vertices, faces, labels, body_limit=.8, head_limit=.04):
    """One immutable region pass. Upper 32% gets conservative detail protection.

    This geometric guard is a sample experiment, not semantic face recognition.
    Decisions use physical mm², perimeter votes, and region size contrast.
    """
    n = len(faces)
    tri = vertices[faces]
    area = np.linalg.norm(np.cross(tri[:, 1]-tri[:, 0], tri[:, 2]-tri[:, 0]), axis=1)/2
    centroids = tri.mean(axis=1)
    edges = np.sort(faces[:, [[0, 1], [1, 2], [2, 0]]].reshape(-1, 2), axis=1)
    owner = np.repeat(np.arange(n), 3)
    key = edges[:, 0].astype(np.int64)*len(vertices)+edges[:, 1]
    order = np.argsort(key, kind='stable')
    key, edges, owner = key[order], edges[order], owner[order]
    starts = np.r_[0, np.flatnonzero(np.diff(key))+1]
    counts = np.diff(np.r_[starts, len(key)])
    pair_starts = starts[counts == 2]
    a, b = owner[pair_starts], owner[pair_starts+1]
    edge_length = np.linalg.norm(vertices[edges[pair_starts, 0]]-vertices[edges[pair_starts, 1]], axis=1)
    # No smoothing across nonmanifold edges. Keep affected regions unchanged.
    unsafe = owner[np.repeat(counts != 2, counts)]
    parent, rank = list(range(n)), [0]*n

    def find(i):
        while parent[i] != i:
            parent[i] = parent[parent[i]]
            i = parent[i]
        return i

    for i, j in zip(a[labels[a] == labels[b]].tolist(), b[labels[a] == labels[b]].tolist()):
        i, j = find(i), find(j)
        if i != j:
            if rank[i] < rank[j]: i, j = j, i
            parent[j] = i
            if rank[i] == rank[j]: rank[i] += 1
    roots = np.fromiter((find(i) for i in range(n)), dtype=np.int32, count=n)
    region_area = np.bincount(roots, weights=area, minlength=n)
    lo, hi = np.full((n, 3), np.inf), np.full((n, 3), -np.inf)
    np.minimum.at(lo, roots, tri.min(axis=1))
    np.maximum.at(hi, roots, tri.max(axis=1))
    protected = np.zeros(n, bool)
    protected[roots[unsafe]] = True
    # Protect long thin features (e.g. brows/lip edges) despite their small area.
    protected |= (hi-lo).max(axis=1) > 3.0
    head_z = vertices[:, 2].min()+np.ptp(vertices[:, 2])*.68
    limit = np.where(hi[:, 2] >= head_z, head_limit, body_limit)
    small = (region_area > 0) & (region_area <= limit) & ~protected
    ra, rb = roots[a], roots[b]
    boundary = ra != rb
    ra, rb, lengths = ra[boundary], rb[boundary], edge_length[boundary]
    votes = np.zeros((n, 6))
    total = np.zeros(n)
    for src, dst in ((ra, rb), (rb, ra)):
        use = small[src]
        np.add.at(total, src[use], lengths[use])
        use &= region_area[dst] >= 10*region_area[src]
        np.add.at(votes, (src[use], labels[dst[use]]), lengths[use])
    winner = votes.argmax(axis=1)
    merge = small & (total > 0) & (votes.max(axis=1) >= .9*total)
    output = labels.copy()
    changed = merge[roots]
    output[changed] = winner[roots[changed]]
    stats = {
        'regions_before': int(np.count_nonzero(region_area)),
        'merged_regions': int(merge.sum()),
        'changed_faces': int(changed.sum()),
        'changed_area_mm2': float(area[changed].sum()),
        'surface_area_mm2': float(area.sum()),
        'body_threshold_mm2': body_limit, 'head_threshold_mm2': head_limit,
        'head_guard_z_mm': float(head_z),
        'head_changed_area_mm2': float(area[changed & (centroids[:, 2] >= head_z)].sum()),
        'nonmanifold_or_open_edge_groups': int(np.count_nonzero(counts != 2)),
        'protection': 'upper 32%: conservative threshold; long thin regions and open edges retained; no semantic recognition',
    }
    return output, stats


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    start = time.monotonic()
    original = args.source.read_bytes()
    digest = hashlib.sha256(original).hexdigest()
    with zipfile.ZipFile(args.source) as archive:
        main_model = ET.fromstring(archive.read('3D/3dmodel.model'))
        for node in main_model.findall('.//{*}component')+main_model.findall('.//{*}item'):
            transform = node.get('transform')
            if transform and list(map(float, transform.split()))[:9] != [1,0,0,0,1,0,0,0,1]:
                raise ValueError('This bounded experiment requires identity rotation/scale')
        meshes = []
        for name in archive.namelist():
            if name.endswith('.model'):
                meshes.extend((name, m) for m in ET.fromstring(archive.read(name)).findall('.//{*}mesh'))
        if len(meshes) != 1: raise ValueError('Exactly one imported mesh required')
        name, mesh = meshes[0]
        vertices = np.array([[float(v.attrib[k]) for k in ('x','y','z')] for v in mesh.find('{*}vertices')], np.float32)
        nodes = mesh.find('{*}triangles')
        faces = np.array([[int(t.attrib[k]) for k in ('v1','v2','v3')] for t in nodes], np.int32)
        labels = np.array([CODES[t.attrib['paint_color']] for t in nodes], np.uint8)
        palette = json.loads(archive.read('Metadata/project_settings.config'))['filament_colour']
        if len(palette) != 6: raise ValueError('Expected six physical filaments')
        print('Loaded', len(vertices), 'vertices,', len(faces), 'faces', flush=True)
        cleaned, stats = cleanup(vertices, faces, labels)
        print('Cleaned', stats, flush=True)
        xml = archive.read(name)
        index = iter(cleaned.tolist())
        updated = re.sub(rb'paint_color="[^"]*"', lambda m: ('paint_color="'+ENCODE[next(index)]+'"').encode(), xml)
        assert next(index, None) is None
        out_file = args.output/'cleaned.3mf'
        if out_file.exists(): raise FileExistsError(out_file)
        with zipfile.ZipFile(out_file, 'w', compression=zipfile.ZIP_DEFLATED) as out:
            for item in archive.infolist():
                # Baseline thumbnails would misrepresent the derived result.
                if item.filename.startswith('Metadata/') and item.filename.endswith('.png'): continue
                out.writestr(item, updated if item.filename == name else archive.read(item.filename))
        reparsed = ET.fromstring(updated).find('.//{*}mesh')
        assert ET.tostring(reparsed.find('{*}vertices')) == ET.tostring(mesh.find('{*}vertices'))
        assert np.array_equal(faces, np.array([[int(t.attrib[k]) for k in ('v1','v2','v3')] for t in reparsed.find('{*}triangles')]))
        assert np.array_equal(cleaned, np.array([CODES[t.attrib['paint_color']] for t in reparsed.find('{*}triangles')]))
        assert hashlib.sha256(args.source.read_bytes()).hexdigest() == digest
    tri = vertices[faces]
    cross = np.cross(tri[:, 1]-tri[:, 0], tri[:, 2]-tri[:, 0])
    normals = np.zeros_like(vertices)
    for c in range(3): np.add.at(normals, faces[:, c], cross)
    normals /= np.maximum(np.linalg.norm(normals, axis=1, keepdims=True), 1e-15)
    # An identical expanded surface is shared by both WebGL views.
    buffer = np.column_stack((tri.reshape(-1,3), normals[faces].reshape(-1,3),
                              np.repeat(labels,3), np.repeat(cleaned,3))).astype('<f4')
    (args.output/'mesh.bin').write_bytes(buffer.tobytes())
    report = {'source_3mf_sha256': digest, 'vertices':len(vertices), 'faces':len(faces),
              'palette':palette, 'bounds':[vertices.min(axis=0).tolist(), vertices.max(axis=0).tolist()],
              'checks':{'geometry_unchanged':True,'paint_roundtrip':True,'source_unchanged':True},
              'limits':['offline prototype; no new native default','no slicing/physical print validation',
                        'manual semantic protection not yet implemented; upper head protected conservatively'],
              'statistics':stats,'seconds':time.monotonic()-start}
    (args.output/'result.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
    print(json.dumps(report,ensure_ascii=False),flush=True)


if __name__ == '__main__': main()
