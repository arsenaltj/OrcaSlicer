"""Continuous parent-mask coverage on visible source triangles, including subpixels."""
from collections import defaultdict

import numpy as np

from local_parent_projection import analytic_coverage, coverage_labels


SAMPLES = np.array([[1/3]*3, [.98,.01,.01], [.01,.98,.01], [.01,.01,.98],
                    [.49,.49,.02], [.02,.49,.49], [.49,.02,.49]])
CLASSES = {'hair': 1, 'skin': 3, 'cloth': 4}


def project(triangles, camera):
    local = (triangles-np.asarray(camera['center'])) @ np.asarray(camera['basis'])[:2].T
    return np.stack(((local[...,0]/camera['half_height']+1)*camera['width']/2,
                     (1-local[...,1]/camera['half_height'])*camera['height']/2), axis=-1)


def root_votes(vertices, faces, camera, labels, confidence, visibility, eligible, raster_ids=None):
    """Visibility comes from the closest exact mesh hit, not raster face-ID occupancy."""
    if len(visibility)!=len(faces) or np.any(visibility>127):
        raise ValueError('Source visibility face count or sample mask drift')
    if raster_ids is not None and raster_ids.shape!=labels.shape:
        raise ValueError('Parent raster face mapping shape drift')
    raster_parent=np.zeros(len(faces),np.uint8)
    raster_coverage=np.zeros(len(faces),float)
    if raster_ids is not None:
        valid=(raster_ids>=0)&(raster_ids<len(faces))
        totals=np.bincount(raster_ids[valid],minlength=len(faces))
        for parent,category in CLASSES.items():
            supported=valid & np.isin(labels,(2,3) if parent=='skin' else (category,)) & (confidence>=.9)
            amounts=np.bincount(raster_ids[supported],minlength=len(faces))
            fractions=amounts/np.maximum(totals,1)
            take=(totals>0)&(fractions>=.9)
            raster_parent[take],raster_coverage[take]=category,fractions[take]
    roots = np.flatnonzero(eligible & (visibility>0))
    result = np.full(len(faces),255,np.uint8)
    amount = np.zeros(len(faces), float)
    for start in range(0,len(roots),32768):
        selected = roots[start:start+32768]
        xy = project(vertices[faces[selected]],camera)
        in_frame=(xy[...,0].min(1)>=0)&(xy[...,1].min(1)>=0)&\
                 (xy[...,0].max(1)<=labels.shape[1])&(xy[...,1].max(1)<=labels.shape[0])
        result[selected[in_frame]]=0
        parent,quality = coverage_labels(xy,labels,confidence)
        # A partial triangle still needs a visible sample inside the supported mask.
        locations = np.einsum('sj,fjc->fsc',SAMPLES,xy)
        pixels = np.floor(locations).astype(int)
        valid = (pixels[...,0]>=0)&(pixels[...,0]<labels.shape[1])&\
                (pixels[...,1]>=0)&(pixels[...,1]<labels.shape[0])
        sample_labels = np.zeros(valid.shape,np.uint8)
        sample_labels[valid] = labels[pixels[...,1][valid],pixels[...,0][valid]]
        sample_labels[sample_labels==2]=3
        sample_quality=np.zeros(valid.shape,float)
        sample_quality[valid]=confidence[pixels[...,1][valid],pixels[...,0][valid]]
        hit = (visibility[selected,None] & (1<<np.arange(7)))>0
        partial=visibility[selected]!=127
        bound=np.any(hit & valid & (sample_labels==parent[:,None]) & (sample_quality>=.9),axis=1) & (parent>0) & ~partial
        result[selected[bound]],amount[selected[bound]] = parent[bound],quality[bound]
        # Partial visibility uses every target pixel, including unknown/low-quality
        # pixels in its denominator. The occluder's projected mask is irrelevant.
        local=selected[partial & in_frame]
        result[local],amount[local]=raster_parent[local],raster_coverage[local]
    return result, amount


def cell_sample_support(samples,triangle,camera,labels,confidence,visibility,parent,
                        raster_ids=None,source_face_id=None,raster_barycentric=None,cell=None):
    xy=project(np.asarray(samples)@triangle,camera)
    pixels=np.floor(xy).astype(int)
    hit=(visibility & (1<<np.arange(7)))>0
    valid=(pixels[:,0]>=0)&(pixels[:,0]<labels.shape[1])&\
          (pixels[:,1]>=0)&(pixels[:,1]<labels.shape[0])
    supported=np.zeros(7,bool)
    supported[valid]=np.isin(labels[pixels[valid,1],pixels[valid,0]],(2,3) if parent=='skin' else
                             (CLASSES[parent],)) & (confidence[pixels[valid,1],pixels[valid,0]]>=.9)
    if visibility!=127:
        if raster_ids is None or source_face_id is None or raster_barycentric is None or cell is None:
            return False
        if raster_ids.shape!=labels.shape or raster_barycentric.shape!=(*labels.shape,3):
            raise ValueError('Parent cell raster mapping shape drift')
        from surface_partition import inside_analytic
        contour=project(np.asarray(cell['polygon'])@triangle,camera)
        lower=np.maximum(np.floor(contour.min(0)).astype(int),[0,0])
        upper=np.minimum(np.ceil(contour.max(0)).astype(int),labels.shape[::-1])
        crop=np.s_[lower[1]:upper[1],lower[0]:upper[0]]
        target=raster_ids[crop]==source_face_id
        bary=raster_barycentric[crop][target]
        contained=inside_analytic(cell['polygon'],bary)
        for hole in cell['holes']:
            contained &= ~inside_analytic(hole,bary)
        target[target]=contained
        support=np.isin(labels[crop],(2,3) if parent=='skin' else (CLASSES[parent],)) & (confidence[crop]>=.9)
        return bool(hit.any() and target.any() and (support&target).sum()/target.sum()>=.9)
    return bool(hit.any() and np.all(supported[hit]))


def full_sample_support(rows, visibility, accepted):
    if visibility.shape!=(len(rows),len(rows[0][2])):
        raise ValueError('Independent parent visibility vote mapping drift')
    if accepted.shape!=rows[0][2].shape:
        raise ValueError('Accepted parent vote mapping drift')
    families=defaultdict(list)
    for index,(family,resolution,labels) in enumerate(rows):
        families[family].append((resolution,labels,visibility[index]))
    result=np.zeros(accepted.shape,bool)
    for records in families.values():
        combined=np.zeros(accepted.shape,np.uint8)
        complete=np.zeros(accepted.shape,bool)
        for _,labels,bits in sorted(records,key=lambda r:r[0]):
            take=labels!=255
            combined[take],complete[take]=labels[take],bits[take]==127
        result |= (accepted>0)&(combined==accepted)&complete
    return result


def independent_votes(rows):
    """Higher-resolution same-direction crops refine one vote, never add a view."""
    families = defaultdict(list)
    for family,resolution,labels in rows:
        families[family].append((resolution,labels))
    columns = []
    for family,records in sorted(families.items()):
        combined = np.zeros_like(records[0][1])
        for _,labels in sorted(records,key=lambda r:r[0]):
            take = labels!=255
            combined[take] = labels[take]
        columns.append((family,combined))
    votes = np.column_stack([labels for _,labels in columns])
    support = (votes>0).sum(1)
    lo = np.where(votes>0,votes,255).min(1)
    hi = votes.max(1)
    conflicts = (support>1)&(lo!=hi)
    accepted = np.where((support>=2)&~conflicts,hi,0).astype(np.uint8)
    return accepted, conflicts, columns
