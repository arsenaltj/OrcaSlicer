"""Shared analytic semantic mask coverage, including subpixel source polygons."""
import numpy as np
from local_leaf_boundaries import area, clip_triangle

CLASSES = {"hair":1,"skin":3,"cloth":4}

def analytic_coverage(polygon, labels, confidence, parent):
    """Integrate continuous polygon/pixel intersections; a leaf need not fill a pixel."""
    polygon = np.asarray(polygon, dtype=float)
    total = area(polygon)
    if total < 1e-12:
        return 0.
    classes = {'skin': (2, 3), 'hair': (1,), 'cloth': (4,)}[parent]
    lo = np.maximum(np.floor(polygon.min(0)).astype(int), 0)
    hi = np.minimum(np.ceil(polygon.max(0)).astype(int), labels.shape[::-1])
    covered = 0.
    for y in range(lo[1], hi[1]):
        for x in range(lo[0], hi[0]):
            if labels[y, x] in classes and confidence[y, x]>=.9:
                square = np.array([[x,y], [x+1,y], [x+1,y+1], [x,y+1]], dtype=float)
                covered += area(clip_triangle(polygon, square))
    return min(1., covered/total)



def coverage_labels(triangles, labels, confidence):
    """Exact pixel/polygon area near boundaries; uniform bounding boxes are fast."""
    count = len(triangles)
    result = np.zeros(count, np.uint8)
    covered = np.zeros(count, float)
    if not count:
        return result, covered
    lower = np.floor(triangles.min(1)).astype(int)
    upper = np.ceil(triangles.max(1)).astype(int)
    height, width = labels.shape
    in_frame = (lower[:,0]>=0)&(lower[:,1]>=0)&(upper[:,0]<=width)&(upper[:,1]<=height)
    lower = np.clip(lower, [0,0], [width,height])
    upper = np.clip(upper, [0,0], [width,height])
    size = (upper[:,0]-lower[:,0])*(upper[:,1]-lower[:,1])
    valid = in_frame & (size>0)
    masks = {parent: np.isin(labels,(2,3) if parent=='skin' else (category,))&(confidence>=.9)
             for parent,category in CLASSES.items()}
    possible = np.zeros((count,3), bool)
    for index,(parent,category) in enumerate(CLASSES.items()):
        integral = np.pad(masks[parent].astype(np.int32), ((1,0),(1,0))).cumsum(0).cumsum(1)
        amount = (integral[upper[:,1],upper[:,0]]-integral[lower[:,1],upper[:,0]]-
                  integral[upper[:,1],lower[:,0]]+integral[lower[:,1],lower[:,0]])
        same = valid & (amount==size)
        result[same], covered[same] = category, 1.
        possible[:,index] = valid & (amount>0)
    for row in np.flatnonzero(valid & (result==0) & possible.any(1)):
        for index,parent in enumerate(CLASSES):
            if possible[row,index]:
                fraction = analytic_coverage(triangles[row], labels, confidence, parent)
                if fraction>=.9:
                    result[row], covered[row] = CLASSES[parent], fraction
                    break
    return result, covered
