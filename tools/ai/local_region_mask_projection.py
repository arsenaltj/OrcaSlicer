"""Experimental class-free mask evidence on an exactly bound mesh.

No network dependencies. IDs are region proposals, never semantic classes.
Occluded faces remain unknown; repeated pixels do not multiply face area.
"""
import numpy as np


def project_masks(face_ids, masks, face_patch, face_area, scores, *, min_coverage=.8):
    """Return sparse per-mask patch coverage; keep overlapping proposals separate.

    Coverage is relative to the visible area of a patch in this view. Consumers
    must retain visible_fraction and require independent-view support before
    interpreting it as a complete surface region.
    """
    n=len(face_patch)
    if (face_ids.ndim!=2 or face_ids.size>1024**2 or face_ids.dtype.kind not in 'iu' or
        face_patch.shape!=(n,) or face_patch.dtype.kind not in 'iu' or n==0 or
        face_area.shape!=(n,) or not np.isfinite(face_area).all() or np.any(face_area<=0) or
        np.any(face_patch<0) or np.any(face_patch>=n) or np.any(face_ids < -1) or np.any(face_ids>=n) or
        masks.ndim!=3 or masks.shape[1:]!=face_ids.shape or masks.dtype!=np.bool_ or len(masks)>2048 or
        np.shape(scores)!=(len(masks),) or not np.isfinite(scores).all() or
        np.any(scores<0) or np.any(scores>1) or not 0<min_coverage<=1):
        raise ValueError('Invalid bound mask evidence')
    count=int(face_patch.max())+1
    total=np.bincount(face_patch,weights=face_area,minlength=count)
    if np.any(total<=0):raise ValueError('Patch IDs must be dense')
    visible=face_ids>=0;faces=face_ids[visible];patches=face_patch[faces]
    repeats=np.bincount(faces,minlength=n)
    weight=face_area[faces]/np.maximum(repeats[faces],1)
    seen=np.bincount(patches,weights=weight,minlength=count)
    result=[]
    for index,mask in enumerate(masks):
        supported=mask[visible]
        # A background prediction or entire-object mask cannot define internal parts.
        inside=int(supported.sum());outside=int(mask[~visible].sum())
        if not inside or outside>inside or inside>=.95*len(faces):continue
        area=np.bincount(patches[supported],weights=weight[supported],minlength=count)
        coverage=area/np.maximum(seen,1e-15)
        selected=np.flatnonzero((coverage>=min_coverage)&(seen>0))
        if len(selected):result.append(dict(mask_id=index,score=float(scores[index]),patches=selected,
            coverage=coverage[selected],visible_fraction=seen[selected]/total[selected]))
    return result
