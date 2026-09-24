"""Experimental cleanup of enclosed, texture-consistent surface-region holes.

This module proposes edit masks only. It does not change printable colors, the
workbench default, or saved user regions.
"""

import cv2
import numpy as np
from scipy.sparse import coo_matrix
from scipy.sparse.csgraph import connected_components


POLICY = "surface-enclosed-hole-experiment-v1"


def fill_enclosed_holes(patch, neighbors, area, rgb, labels, names, proposal,
                        group_id, semantic_name, max_area_share=.005, max_delta_lab=15.):
    """Return a new patch proposal after filling only safe enclosed unknown holes.

    A hole must be on the same connected mesh surface, have no open mesh edge,
    contain no protected semantic face or competing candidate, and match its
    enclosing group's original texture. Thresholds are explicit experimental
    parameters, not calibrated confidence values.
    """
    faces = len(patch)
    patches = len(proposal)
    if (not 0 < faces <= 2_000_000 or not 0 < patches <= faces or
        patch.shape != (faces,) or neighbors.shape != (faces, 3) or
        area.shape != (faces,) or rgb.shape != (faces, 3) or
        labels.shape != (faces,) or proposal.shape != (patches,) or
        patch.dtype.kind not in "iu" or neighbors.dtype.kind not in "iu" or
        labels.dtype.kind not in "iu" or proposal.dtype.kind not in "iu" or
        np.any(patch < 0) or np.any(patch >= patches) or
        np.any(neighbors < -1) or np.any(neighbors >= faces) or
        np.any(labels < -1) or np.any(labels >= len(names)) or
        np.any(proposal < -1) or not np.isfinite(area).all() or
        not np.isfinite(rgb).all() or np.any(area <= 0) or
        np.any(rgb < 0) or np.any(rgb > 1) or
        group_id < 0 or semantic_name not in names or
        not 0 < max_area_share <= .05 or not 0 < max_delta_lab <= 50):
        raise ValueError("Invalid bound surface-region proposal")
    patch_area = np.bincount(patch, weights=area, minlength=patches)
    if np.any(patch_area <= 0):
        raise ValueError("Patch IDs must be dense")
    group = proposal == group_id
    if not np.any(group):
        return proposal.copy(), dict(policy=POLICY, filled_patches=0, filled_area=0.)
    other = ~group
    patch_rgb = np.column_stack([
        np.bincount(patch, weights=area * rgb[:, c], minlength=patches) / patch_area
        for c in range(3)
    ])
    lab = cv2.cvtColor(np.clip(patch_rgb, 0, 1)[None].astype(np.float32), cv2.COLOR_RGB2LAB)[0]
    is_group_semantic = np.asarray([name == semantic_name for name in names], bool)
    protected_face = (labels >= 0) & ~is_group_semantic[np.maximum(labels, 0)]
    protected_patch = np.bincount(patch[protected_face], minlength=patches) > 0
    open_patch = np.bincount(patch[(neighbors < 0).any(axis=1)], minlength=patches) > 0

    left = np.repeat(np.arange(faces, dtype=np.int32), 3)
    right = neighbors.reshape(-1)
    valid = (right >= 0) & (left < right)
    pairs = np.unique(np.sort(np.column_stack((patch[left[valid]], patch[right[valid]])), axis=1), axis=0)
    pairs = pairs[pairs[:, 0] != pairs[:, 1]]
    internal = other[pairs[:, 0]] & other[pairs[:, 1]]
    internal_pairs = pairs[internal]
    graph = coo_matrix((np.ones(len(internal_pairs) * 2, np.uint8),
                        (np.r_[internal_pairs[:, 0], internal_pairs[:, 1]],
                         np.r_[internal_pairs[:, 1], internal_pairs[:, 0]])),
                       shape=(patches, patches)).tocsr()
    count, component = connected_components(graph, directed=False)
    other_ids = np.flatnonzero(other)
    component_area = np.bincount(component[other_ids], weights=patch_area[other_ids], minlength=count)
    component_open = np.bincount(component[other_ids[open_patch[other_ids]]], minlength=count) > 0
    component_protected = np.bincount(component[other_ids[protected_patch[other_ids]]], minlength=count) > 0
    competing = other & (proposal != -1)
    component_competing = np.bincount(component[np.flatnonzero(competing)], minlength=count) > 0
    border = pairs[group[pairs[:, 0]] ^ group[pairs[:, 1]]]
    other_border = np.where(group[border[:, 0]], border[:, 1], border[:, 0])
    group_border = np.where(group[border[:, 0]], border[:, 0], border[:, 1])
    border_component = component[other_border]
    touches_group = np.bincount(border_component, minlength=count) > 0
    eligible = ((component_area > 0) &
                (component_area <= max_area_share * float(patch_area[group].sum())) &
                touches_group & ~component_open & ~component_protected & ~component_competing)
    accepted = np.zeros(count, bool)
    for cid in np.flatnonzero(eligible):
        members = other_ids[component[other_ids] == cid]
        adjacent = group_border[border_component == cid]
        mean_hole = np.average(lab[members], axis=0, weights=patch_area[members])
        mean_group = np.average(lab[adjacent], axis=0, weights=patch_area[adjacent])
        accepted[cid] = np.linalg.norm(mean_hole - mean_group) <= max_delta_lab
    filled = other & accepted[component]
    result = proposal.copy()
    result[filled] = group_id
    assert np.array_equal(result[proposal != -1], proposal[proposal != -1])
    assert not np.any(filled & protected_patch)
    assert not np.any(filled & open_patch)
    return result, dict(policy=POLICY, filled_holes=int(accepted.sum()),
                        filled_patches=int(filled.sum()), filled_area=float(patch_area[filled].sum()),
                        protected_changes=0, competing_changes=0, open_edge_changes=0)
