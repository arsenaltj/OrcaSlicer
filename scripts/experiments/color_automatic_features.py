"""Bind automatic face regions to an exact GLB; never invent missing features."""
import hashlib
import heapq
import json
from pathlib import Path
import numpy as np

GROUPS = [('brow-rb','brow',('rb',)),('brow-lb','brow',('lb',)),
          ('eye-re','eye',('re',)),('eye-le','eye',('le',)),
          ('mouth','mouth',('imouth','llip','ulip'))]


def make_features(semantic, confidence, label_schema):
    if semantic.ndim!=1 or confidence.shape!=semantic.shape or not np.isfinite(confidence).all():
        raise ValueError('Invalid semantic arrays')
    if len(set(label_schema))!=len(label_schema) or np.any(semantic < -1) or np.any(semantic >= len(label_schema)):
        raise ValueError('Invalid semantic label schema')
    if np.any(confidence<0) or np.any(confidence>1):raise ValueError('Invalid confidence')
    trusted=confidence>=.9
    masks={};features=[]
    for name,kind,labels in GROUPS:
        ids=[label_schema.index(label) for label in labels]
        masks[name]=trusted & np.isin(semantic,ids)
        features.append({'name':name,'kind':kind,'dark_threshold':155 if kind=='brow' else 100})
    return features,masks


def expand_feature_windows(masks, semantic, schema, centers, a, b, radius_mm=.25):
    """A bounded candidate margin, not newly recognized semantic evidence.

    Only face/unknown surface may be traversed; other recognized anatomy,
    clothes and hair are barriers. RGB evidence still decides the final color.
    """
    n=len(semantic)
    src,dst=np.r_[a,b],np.r_[b,a]
    order=np.argsort(src,kind='stable');src,dst=src[order],dst[order]
    counts=np.bincount(src,minlength=n);starts=np.r_[0,np.cumsum(counts)]
    lengths=np.linalg.norm(centers[src]-centers[dst],axis=1)
    result={};candidates=[]
    for name,mask in masks.items():
        allowed=(semantic==-1)|(semantic==schema.index('face'))|mask
        distance=np.full(n,np.inf)
        seeds=np.flatnonzero(mask);distance[seeds]=0
        queue=[(0.,int(i)) for i in seeds];heapq.heapify(queue)
        while queue:
            d,i=heapq.heappop(queue)
            if d>distance[i]:continue
            for e in range(starts[i],starts[i+1]):
                j=dst[e];new=d+float(lengths[e])
                if allowed[j] and new<=radius_mm and new<distance[j]:
                    distance[j]=new;heapq.heappush(queue,(new,int(j)))
        candidates.append(distance<=radius_mm)
    # Ambiguous margins are not attributed to either feature. Original masks
    # are disjoint trusted evidence and remain intact.
    overlaps=np.sum(candidates,axis=0)>1
    for (name,mask),candidate in zip(masks.items(),candidates):result[name]=mask|(candidate & ~overlaps)
    return result


def load_features(folder, source_sha, geometry_sha, count, centers=None, edges=None):
    folder=Path(folder)
    report=json.loads((folder/'result.json').read_text(encoding='utf-8'))
    if report['source_sha256']!=source_sha or report['render_geometry_id']!=geometry_sha:
        raise ValueError('Automatic regions belong to another source or mesh')
    if report['manual_feature_windows'] is not False:raise ValueError('Automatic evidence required')
    actual_sha=hashlib.sha256((folder/'regions.npz').read_bytes()).hexdigest()
    if report['regions_sha256']!=actual_sha:raise ValueError('Automatic region artifact changed')
    with np.load(folder/'regions.npz',allow_pickle=False) as data:
        semantic=data['semantic'];confidence=data['confidence']
    if semantic.shape!=(count,):raise ValueError('Face count mismatch')
    features,masks=make_features(semantic,confidence,report['label_schema'])
    if any(not mask.any() for mask in masks.values()):
        raise ValueError('Incomplete automatic eyes/brows/mouth; retain the prior preview')
    raw_counts={name:int(mask.sum()) for name,mask in masks.items()}
    if centers is not None and edges is not None:
        masks=expand_feature_windows(masks,semantic,report['label_schema'],centers,*edges)
    evidence={'source_sha256':source_sha,'render_geometry_id':geometry_sha,
        'regions_sha256':actual_sha,'recognized_feature_faces':raw_counts,
        'manual_face_coordinates_used':False,'confidence_gate':.9,
        'candidate_surface_margin_mm':.25 if centers is not None else 0,
        'feature_faces':{name:int(mask.sum()) for name,mask in masks.items()},
        'recognition_seconds':report['seconds'],
        'limits':'Automatic facial localization only; body material policy remains sample-specific.'}
    return features,masks,evidence
