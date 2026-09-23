"""Offline skin-palette experiment; does not mutate workbench or printer slots.

Uses existing single-person semantic labels and geometry-area weights. The
trimmed texture target is a midtone estimate, not intrinsic albedo recovery.
The caller must resolve it using the real native palette and retain the recipe.
"""
from __future__ import annotations

import numpy as np

SKIN = frozenset(('face', 'nose', 'neck', 'lr', 'rr'))
PROTECTED = frozenset(('le', 're', 'iris', 'ulip', 'llip', 'imouth',
                       'hair', 'lb', 'rb', 'eyeg', 'cloth', 'hat'))


def weighted_quantile(values, weights, q):
    order = np.argsort(values, kind='stable')
    cumulative = np.cumsum(weights[order])
    return values[order[np.searchsorted(cumulative, q * cumulative[-1])]]


def skin_plan(rgb, areas, pieces, labels, names):
    """Return one shared target and eligible pieces, or a conservative no-op.

    No absolute light-skin threshold or portrait-specific IDs are used. Multiple
    face label instances are unsupported: do not pool different people's skin.
    Partial/ambiguous pieces remain untouched; no boundary or face IDs change.
    """
    rgb = np.asarray(rgb, dtype=float)
    areas = np.asarray(areas, dtype=float)
    pieces, labels = np.asarray(pieces), np.asarray(labels)
    n = len(areas)
    if (rgb.shape != (n, 3) or pieces.shape != (n,) or labels.shape != (n,)
            or not np.isfinite(rgb).all() or not np.isfinite(areas).all()
            or np.any(areas < 0) or np.any(rgb < 0) or np.any(rgb > 1)
            or np.any(labels < -1) or np.any(labels >= len(names))):
        raise ValueError('Invalid or mismatched face arrays')
    if names.count('face') != 1:
        return dict(status='unsupported_face_count',pieces=[])
    face = labels == names.index('face')
    skin = np.isin(labels, [i for i, name in enumerate(names) if name in SKIN])
    protected = np.isin(labels, [i for i, name in enumerate(names) if name in PROTECTED])
    seed = face & (areas > 0)
    if not seed.any():
        return dict(status='no_face_evidence',pieces=[])
    # Relative luminance ranking only. Trimming is per person, not bleaching.
    light = rgb @ np.array([.2126, .7152, .0722])
    lo = weighted_quantile(light[seed],areas[seed],.35)
    hi = weighted_quantile(light[seed],areas[seed],.75)
    mid = seed & (light >= lo) & (light <= hi)
    target = np.average(rgb[mid],axis=0,weights=areas[mid])
    eligible=[]
    for piece in np.unique(pieces[skin]):
        mask=pieces==piece
        total=areas[mask].sum()
        known=areas[mask & (labels>=0)].sum()
        support=areas[mask & skin].sum()
        excluded=areas[mask & protected].sum()
        if total>0 and support>=.25*total and support>=.9*known and excluded<=.02*total:
            eligible.append(int(piece))
    return dict(status='proposed' if eligible else 'ambiguous_regions',
                pieces=eligible,target_rgb=target.tolist(),
                target_hex='#'+''.join(f'{int(x*255+.5):02X}' for x in target),
                seed_area=float(areas[seed].sum()),midtone_area=float(areas[mid].sum()),
                luminance_band=[float(lo),float(hi)])
