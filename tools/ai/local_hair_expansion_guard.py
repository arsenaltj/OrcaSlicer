"""Experimental multi-view clothing veto for newly expanded hair edit patches.

This operates on already bound face IDs. It never changes source semantics,
native printing color, or the original SAM2 hair seed. Body segmenter scores
are uncalibrated evidence; only strong, independent view agreement is used.
"""

import numpy as np


POLICY = 'hair-expansion-cloth-veto-experiment-v1'


def guard_expansion(patch, area, base, expanded, hair_group, observations):
    """Return a conservative patch edit proposal and diagnostic counts.

    Each observation is ``(view_family, face_id_image, six_body_scores)``.
    A newly added hair patch returns to its pre-expansion ID only if at least
    two independent views strongly see clothing and none strongly see hair.
    """
    faces = len(patch)
    patches = len(base)
    if (not 0 < faces <= 2_000_000 or not 0 < patches <= faces or
        patch.shape != (faces,) or area.shape != (faces,) or
        expanded.shape != (patches,) or patch.dtype.kind not in 'iu' or
        base.dtype.kind not in 'iu' or expanded.dtype.kind not in 'iu' or
        np.any(patch < 0) or np.any(patch >= patches) or
        np.any(base < -1) or np.any(expanded < -1) or
        not np.isfinite(area).all() or np.any(area <= 0) or
        hair_group < 0 or not 1 <= len(observations) <= 8):
        raise ValueError('Invalid bound hair proposal')
    patch_area = np.bincount(patch, weights=area, minlength=patches)
    if np.any(patch_area <= 0):
        raise ValueError('Patch IDs must be dense')
    cloth_votes = np.zeros(patches, np.uint8)
    hair_votes = np.zeros(patches, np.uint8)
    families = set()
    for family, ids, scores in observations:
        if family in families:
            raise ValueError('Correlated views may not vote twice')
        families.add(family)
        if (ids.ndim != 2 or ids.size > 1024**2 or ids.dtype.kind not in 'iu' or
            scores.shape != (*ids.shape, 6) or not np.isfinite(scores).all() or
            np.any(ids < -1) or np.any(ids >= faces) or
            np.any(scores < 0) or np.any(scores > 1) or
            not np.allclose(scores.sum(-1), 1., atol=1e-3)):
            raise ValueError('Invalid full-view body evidence')
        valid = ids >= 0
        face = ids[valid]
        if not len(face):
            continue
        probability = scores[valid]
        seen_pixels = np.bincount(face, minlength=faces)
        weights = area[face] / seen_pixels[face]
        part = patch[face]
        visible_area = np.bincount(part, weights=weights, minlength=patches)
        hair = np.bincount(part, weights=weights * probability[:, 1], minlength=patches)
        cloth = np.bincount(part, weights=weights * probability[:, 4], minlength=patches)
        enough = visible_area >= .02 * patch_area
        hair_score = hair / np.maximum(visible_area, 1e-15)
        cloth_score = cloth / np.maximum(visible_area, 1e-15)
        hair_votes += enough & (hair_score >= .8) & (hair_score >= cloth_score + .5)
        cloth_votes += enough & (cloth_score >= .8) & (cloth_score >= hair_score + .5)
    newly_added = (expanded == hair_group) & (base != hair_group)
    removed = newly_added & (cloth_votes >= 2) & (hair_votes == 0)
    output = expanded.copy()
    output[removed] = base[removed]
    assert np.array_equal(output[~newly_added], expanded[~newly_added])
    return output, dict(policy=POLICY, expanded_patches=int(newly_added.sum()),
                        removed_patches=int(removed.sum()),
                        removed_area=float(patch_area[removed].sum()),
                        kept_hair_seed_patches=int((base == hair_group).sum()),
                        conflicting_added_patches=int((newly_added & (cloth_votes >= 2) &
                                                       (hair_votes > 0)).sum()),
                        rule='>=2 independent strong cloth views and 0 strong hair views')
