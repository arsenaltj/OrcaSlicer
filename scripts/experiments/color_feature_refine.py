"""Geometry-aware detail refinement for the offline portrait experiment.

Manual feature windows still come from the reviewed sample profile. Diffusion
uses mesh neighbours in millimetres, never nearby pixels across UV islands.
"""
import numpy as np


def smooth_surface_signal(rgb, centers, normals, areas, a, b, active, radius_mm=.24, steps=5):
    """Small bilateral surface filter, anchored to the original color samples.

    No connections across inactive windows, long edges or sharp folds. Areas
    weight neighbours so small triangles do not each get a full-sized vote.
    """
    original = rgb.astype(np.float64)
    output = original.copy()
    use = active[a] & active[b]
    aa, bb = a[use], b[use]
    distance = np.linalg.norm(centers[aa]-centers[bb], axis=1)
    agreement = np.maximum(0, np.einsum('ij,ij->i', normals[aa], normals[bb]))
    color_distance = np.mean((original[aa]-original[bb])**2, axis=1)
    weight = np.exp(-.5*(distance/radius_mm)**2-color_distance/(2*32**2))*agreement**4
    weight[distance > 3*radius_mm] = 0
    src, dst = np.r_[aa,bb], np.r_[bb,aa]
    weights = np.r_[weight*areas[bb],weight*areas[aa]]
    total = np.bincount(src,weights=weights,minlength=len(rgb))
    valid = active & (total > 1e-15)
    self_mass = .9*areas
    for _ in range(steps):
        mean = np.column_stack([np.bincount(src,weights=weights*output[dst,k],minlength=len(rgb)) for k in range(3)])
        output[valid] = (self_mass[valid,None]*original[valid]+mean[valid])/(self_mass[valid]+total[valid])[:,None]
    return output


def detail_colors(kind, rgb):
    """Existing filament indices; distinguish features from incidental shading."""
    r,g,b = rgb.T
    light = rgb.mean(axis=1)
    out = np.zeros(len(rgb),dtype=np.uint8)  # skin
    if kind == 'brow':
        out[light < 155] = 5  # grey pigment, not a black painted-on block
    elif kind == 'eye':
        out[light < 108] = 5
        out[light < 73] = 1
        # The original 12-cluster palette erased much of the sclera. Read the
        # source texture instead; warm skin/eyelids do not satisfy this test.
        sclera = (light > 125) & (r-g < 23) & (g-b < 18)
        out[sclera] = 2
    elif kind == 'mouth':
        lip = (r-g > 50) & (r-g > 1.8*(g-b)+25)
        out[lip] = 3
        out[light < 62] = 1
        teeth = (light > 157) & (r-g < 30) & (g-b < 24)
        out[teeth] = 2
    else:
        raise ValueError('Unsupported detail kind')
    return out


def refine_details(vertices, faces, rgb, previous, feature_masks, profile, a, b):
    tri = vertices[faces]
    centers = tri.mean(axis=1)
    cross = np.cross(tri[:,1]-tri[:,0],tri[:,2]-tri[:,0])
    norm = np.linalg.norm(cross,axis=1)
    normals = cross/np.maximum(norm[:,None],1e-15)
    area = norm/2
    output = previous.copy()
    allowed = np.zeros(len(faces),bool)
    stats = {}
    for feature in profile['features']:
        if feature['kind'] not in ('brow','eye','mouth'): continue
        mask = feature_masks[feature['name']]
        allowed |= mask
        smooth = smooth_surface_signal(rgb,centers,normals,area,a,b,mask)
        output[mask] = detail_colors(feature['kind'],smooth[mask])
        stats[feature['name']] = {
            'changed_area_mm2':float(area[mask & (output != previous)].sum()),
            'black_before_mm2':float(area[mask & (previous == 1)].sum()),
            'black_after_mm2':float(area[mask & (output == 1)].sum()),
            'white_before_mm2':float(area[mask & (previous == 2)].sum()),
            'white_after_mm2':float(area[mask & (output == 2)].sum()),
        }
    assert np.array_equal(previous[~allowed],output[~allowed])
    stats['outside_feature_windows_unchanged'] = True
    stats['changed_area_mm2'] = float(area[output != previous].sum())
    stats['filter_radius_mm'] = .24
    stats['filter_steps'] = 5
    return output, stats
