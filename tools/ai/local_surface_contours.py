"""Source-bound continuous contour proposals for an offline surface partition."""
from dataclasses import dataclass

import cv2
import numpy as np
from scipy.interpolate import CubicSpline, PchipInterpolator
from scipy.ndimage import gaussian_filter1d

from local_eye_landmarks import EYE_BY_LABEL
from local_face_landmarks import (EYEBROWS, UPPER_OUTER, UPPER_INNER, LOWER_OUTER,
                                 LOWER_INNER, polygon_mask)
from local_leaf_boundaries import (AnalyticVisibility, reconstruct_projection,
                                  smooth_lid, source_brow_contour)

POLICY = {'algorithm': 'r7-barycentric-contour/v1', 'roi_size': 1024,
          'acceptance_roi': 4096, 'curve_chord_target_pixels': .20,
          'minimum_independent_views': 2, 'eye_rings': 1, 'brow_rings': 2,
          'lip_rings': 1, 'periocular_band_fraction': .10,
          'periocular_min_delta_l': 18., 'periocular_inner_band_fraction': .035,
          'periocular_precedes_sclera': True, 'brow_all_seed_components': True,
          'periocular_preserves_source_holes': True,
          'oral_auto_color': False, 'source_color_is_ownership': False}


def independent_views(views, vertices, faces):
    result = []
    for view in sorted(views, key=lambda v: -v.quality):
        if view.boundary is None:
            continue
        transform, residual = reconstruct_projection(view.boundary, vertices, faces)
        direction = -np.cross(transform[:3, 0], transform[:3, 1])
        direction /= np.linalg.norm(direction)
        if any(direction @ previous[2] > .999 for previous in result):
            continue
        result.append((view, transform, direction, residual))
    return result


def smooth_closed(points, sigma=.65):
    """A source-supported periodic fit, sampled below the 4096 chord budget."""
    points = np.asarray(points, dtype=float)
    if points.shape[0] < 4 or points.shape[1] != 2 or not np.isfinite(points).all():
        raise ValueError('CONTOUR_POINTS_INSUFFICIENT')
    if np.linalg.norm(points[-1] - points[0]) < 1e-8:
        points = points[:-1]
    smoothed = gaussian_filter1d(points, sigma, axis=0, mode='wrap')
    closed = np.vstack((smoothed, smoothed[:1]))
    distance = np.linalg.norm(np.diff(closed, axis=0), axis=1)
    keep = np.r_[True, distance > 1e-8]
    closed = closed[keep]
    if len(closed) < 4:
        raise ValueError('CONTOUR_COLLAPSED')
    t = np.r_[0., np.cumsum(np.linalg.norm(np.diff(closed, axis=0), axis=1))]
    spline = CubicSpline(t, closed, bc_type='periodic')
    # 0.12 at 1024 stays below 0.5 pixels at the acceptance magnification.
    sampled = spline(np.linspace(0., t[-1], max(32, int(np.ceil(t[-1]/.12))), endpoint=False))
    return sampled


def smooth_arc(points):
    points = np.asarray(points, dtype=float)
    axis = points[-1]-points[0]
    width = np.linalg.norm(axis)
    if width < 2:
        raise ValueError('LIP_ARC_COLLAPSED')
    axis /= width
    normal = np.array([-axis[1], axis[0]])
    x, y = (points-points[0]) @ axis, (points-points[0]) @ normal
    if np.any(np.diff(x) <= 1e-6):
        raise ValueError('LIP_ARC_NOT_MONOTONE')
    t = np.linspace(0., width, 257)
    curve = points[0]+t[:,None]*axis+PchipInterpolator(x,y)(t)[:,None]*normal
    curve[0],curve[-1]=points[0],points[-1]
    return curve


def iris_curve(points, label, scale):
    indices, center_id, rim = EYE_BY_LABEL[label]
    p = np.asarray(points, dtype=float)
    axis = p[indices[8]]-p[indices[0]]
    width = np.linalg.norm(axis)
    if width < 12:
        raise ValueError('IRIS_LANDMARK_SUPPORT_INSUFFICIENT')
    axis /= width
    up = np.array([-axis[1],axis[0]])
    radii = np.max(np.abs((p[rim]-p[center_id]) @ np.column_stack((axis,up))),axis=0)
    if np.min(radii) < 2 or np.max(radii) > width*.5:
        raise ValueError('IRIS_SOURCE_MAPPING_UNCERTAIN')
    # The ellipse itself is continuous. These chords bound its sagitta at 4x.
    radius = float(max(radii)*scale*4)
    steps = max(64,int(np.ceil(np.pi/np.arccos(max(-1.,1.-.20/max(radius,.21))))))
    angles=np.linspace(0.,2*np.pi,steps,endpoint=False)
    curve=p[center_id]+np.cos(angles)[:,None]*radii[0]*axis+np.sin(angles)[:,None]*radii[1]*up
    return curve*scale, dict(center=(p[center_id]*scale).tolist(),radii=(radii*scale).tolist(),
                            axis=axis.tolist(),ellipse_segments=steps,chord_error_target_4096=.20)


def eye_accessory(projection, label, legal):
    """Only original pigment adjacent to the aperture; no synthetic eye ring."""
    points, ids = projection.points, projection.ids
    indices=EYE_BY_LABEL[label][0]
    aperture=polygon_mask(smooth_lid(points[indices]),ids.shape)
    width=np.linalg.norm(points[indices[8]]-points[indices[0]])
    radius=max(1,int(round(width*POLICY['periocular_band_fraction'])))
    kernel=cv2.getStructuringElement(cv2.MORPH_ELLIPSE,(radius*2+1,radius*2+1))
    outer=cv2.dilate(aperture.astype(np.uint8),kernel).astype(bool)
    inner_radius=max(1,int(round(width*POLICY['periocular_inner_band_fraction'])))
    inner_kernel=cv2.getStructuringElement(cv2.MORPH_ELLIPSE,(inner_radius*2+1,inner_radius*2+1))
    eroded=cv2.erode(aperture.astype(np.uint8),inner_kernel).astype(bool)
    band=outer & ~eroded & projection.valid & np.isin(ids,list(legal))
    skin=cv2.dilate(outer.astype(np.uint8),kernel).astype(bool) & ~outer & projection.valid & np.isin(ids,list(legal))
    if skin.sum() < 8 or band.sum() < 4:
        raise ValueError('PERIOCULAR_SOURCE_REFERENCE_INSUFFICIENT')
    lab=cv2.cvtColor(projection.rgb.astype(np.float32)/255.,cv2.COLOR_RGB2Lab)
    reference=float(np.median(lab[skin,0]))
    pigment=band & (lab[:,:,0] < reference-POLICY['periocular_min_delta_l'])
    # A supported line must touch the existing aperture. Detached dark specks
    # in adjacent skin cannot create a new eyeliner component.
    touch=cv2.dilate(aperture.astype(np.uint8),np.ones((3,3),np.uint8)).astype(bool)
    _,components=cv2.connectedComponents(pigment.astype(np.uint8),8)
    seeds=np.unique(components[pigment & touch]); seeds=seeds[seeds!=0]
    pigment &= np.isin(components,seeds)
    scale=1024/max(ids.shape)
    size=(int(round(ids.shape[1]*scale)),int(round(ids.shape[0]*scale)))
    normalized=cv2.resize(pigment.astype(np.float32),size,interpolation=cv2.INTER_LINEAR)
    mask=cv2.GaussianBlur(normalized,(0,0),.55) >= .45
    outlines,hierarchy=cv2.findContours(mask.astype(np.uint8),cv2.RETR_CCOMP,cv2.CHAIN_APPROX_NONE)
    curves,holes=[],[]
    for index,outline in enumerate(outlines):
        if cv2.contourArea(outline)<1:
            continue
        curve=smooth_closed(outline.reshape(-1,2).astype(float)+.5,.5)
        (holes if hierarchy[0,index,3]>=0 else curves).append(curve)
    if not curves:
        raise ValueError('NO_SOURCE_SUPPORTED_EYE_LINE')
    return curves,holes,dict(source_pigment_pixels=int(pigment.sum()),components=len(curves),
                            source_holes=len(holes),source_reference_l=reference,black_ring_drawn=False)


@dataclass
class ContourView:
    family: str
    transform: np.ndarray
    contours: list
    scale: float
    visibility: object
    iris: list = None
    opening: list = None
    audit: dict = None
    holes: list = None

    def projection(self, world):
        return np.column_stack((world,np.ones(len(world)))) @ self.transform*self.scale

    def barycentric_polygons(self, world, curves=None):
        projected=self.projection(world)
        matrix=np.column_stack((projected,np.ones(3)))
        if abs(np.linalg.det(matrix)) < 1e-8:
            raise ValueError('SOURCE_FACE_PROJECTION_DEGENERATE')
        inverse=np.linalg.inv(matrix)
        result=[]
        lower,upper=projected.min(0),projected.max(0)
        for polygon in self.contours if curves is None else curves:
            if np.any(polygon.max(0)<lower) or np.any(polygon.min(0)>upper):
                continue
            bary=np.column_stack((polygon,np.ones(len(polygon)))) @ inverse
            if not np.isfinite(bary).all() or np.max(np.abs(bary))>=1e5:
                raise ValueError('SOURCE_FACE_PROJECTION_CONDITIONING')
            result.append(dict(polygon=bary.tolist(),holes=[]))
        return result

    def references(self, world, library, prefix, curves=None):
        projected=self.projection(world)
        matrix=np.column_stack((projected,np.ones(3)))
        if abs(np.linalg.det(matrix))<1e-8:
            raise ValueError('SOURCE_FACE_PROJECTION_DEGENERATE')
        inverse=np.linalg.inv(matrix)
        if np.max(np.abs(inverse))>=1e5:
            raise ValueError('SOURCE_FACE_PROJECTION_CONDITIONING')
        lower,upper=projected.min(0),projected.max(0)
        def references(polygons,kind):
            keys=[]
            for index,polygon in enumerate(polygons):
                if np.any(polygon.max(0)<lower) or np.any(polygon.min(0)>upper): continue
                key=prefix+'/'+self.family+'/'+kind+str(index)
                if key not in library: library[key]=polygon.tolist()
                keys.append(key)
            return keys
        keys=references(self.contours if curves is None else curves,'')
        holes=references(self.holes or [],'hole-') if curves is None else []
        return dict(family=self.family,matrix=inverse.tolist(),contour_ids=keys,exclude_contour_ids=holes)

    def supported(self, face, world):
        triangle=self.projection(world)/self.scale
        return self.visibility.fraction(face,triangle,world)>=.9


def proposals(label, independent, vertices, faces, accepted, core, permitted, legal):
    result,audit=[],[]
    for view,transform,_,residual in independent:
        if label=='imouth-preserve':
            available='ulip' in view.parts or 'llip' in view.parts
        else:
            available=label.removeprefix('periocular-') in view.parts
        if not available:
            continue
        projection=view.boundary
        scale=1024/max(projection.ids.shape)
        row=dict(family=view.family,camera_residual_pixels_1024=residual*scale)
        audit.append(row)
        try:
            opening,iris,holes=None,None,None
            if label in ('le','re'):
                curves=[smooth_lid(projection.points[EYE_BY_LABEL[label][0]])*scale]
                opening=curves
                curve,fit=iris_curve(projection.points,label,scale)
                iris=[curve]; row['iris_fit']=fit
            elif label in ('lb','rb'):
                contours,source_audit=source_brow_contour(projection,accepted,core,permitted,legal,
                    projection.points[EYEBROWS[label]],all_components=True)
                curves=[smooth_closed(c) for c in contours]; row.update(source_audit)
            elif label in ('ulip','llip'):
                outer,inner=(UPPER_OUTER,UPPER_INNER) if label=='ulip' else (LOWER_OUTER,LOWER_INNER)
                curves=[np.vstack((smooth_arc(projection.points[outer]),smooth_arc(projection.points[inner])[::-1]))*scale]
            elif label=='imouth-preserve':
                curves=[np.vstack((smooth_arc(projection.points[UPPER_INNER]),smooth_arc(projection.points[LOWER_INNER])[::-1]))*scale]
            elif label.startswith('periocular-'):
                curves,holes,source_audit=eye_accessory(projection,label[11:],legal); row.update(source_audit)
            else:
                raise ValueError('UNKNOWN_CONTOUR_LABEL')
            visibility=AnalyticVisibility(vertices,faces,transform,projection.ids.shape)
            result.append(ContourView(view.family,transform,curves,scale,visibility,iris,opening,row,holes))
            row.update(status='SOURCE_CONTOUR',contours=len(curves))
        except (ValueError,cv2.error) as error:
            row.update(status='R6_LOCAL_EVIDENCE_FALLBACK',reason=str(error))
    return result,audit
