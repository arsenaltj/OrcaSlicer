"""Experimental surface-patch consensus, not installed or enabled by the worker.

Returns editing proposals, not calibrated semantic evidence. Original face IDs
remain unchanged. Fine facial labels are immutable; only hair/clothing and
unknown faces are eligible. Promotion requires native replay and visual review.
"""
import numpy as np

POLICY = 'surface-region-consensus-experiment-v1'


def _lab(rgb):
    linear = np.where(rgb <= .04045, rgb / 12.92, ((rgb + .055) / 1.055) ** 2.4)
    xyz = linear @ np.array([[.4124564,.2126729,.0193339],
                             [.3575761,.7151522,.1191920],
                             [.1804375,.0721750,.9503041]])
    xyz /= [.95047,1.,1.08883]
    v = np.where(xyz > (6/29)**3, np.cbrt(xyz), xyz/(3*(6/29)**2)+4/29)
    return np.column_stack((116*v[:,1]-16,500*(v[:,0]-v[:,1]),200*(v[:,1]-v[:,2])))


def propose(patch, neighbors, area, normals, rgb, labels, names, observations):
    """Observations are (independent_family, visible_face_ids, six_scores, direction).

    Scores include background, skin and accessories as competing evidence.
    Patches must be native, connected, and bound to these exact ordered faces.
    This function never reads files, runs models, changes paint or user records.
    """
    n=len(labels)
    if (not 0<n<=2_000_000 or patch.shape!=(n,) or neighbors.shape!=(n,3) or
        area.shape!=(n,) or normals.shape!=(n,3) or rgb.shape!=(n,3) or
        patch.dtype.kind not in 'iu' or labels.dtype.kind not in 'iu' or
        np.any(patch<0) or np.any(patch>=n) or np.any(neighbors < -1) or np.any(neighbors>=n) or
        np.any(labels < -1) or np.any(labels>=len(names)) or not 1<=len(observations)<=8 or
        not all(np.isfinite(v).all() for v in (area,normals,rgb)) or
        np.any(area<=0) or np.any(rgb<0) or np.any(rgb>1)):
        raise ValueError('Invalid bound surface data')
    m=int(patch.max())+1;patch_area=np.bincount(patch,weights=area,minlength=m)
    if np.any(patch_area<=0):raise ValueError('Patch IDs must be dense')
    coarse=np.array([v in ('hair','cloth') for v in names],bool)
    protected=(labels>=0)&~coarse[np.maximum(labels,0)]
    barrier=np.bincount(patch[protected],minlength=m)>0
    mean_rgb=np.column_stack([np.bincount(patch,weights=area*rgb[:,c],minlength=m)/patch_area for c in range(3)])
    mean_normal=np.column_stack([np.bincount(patch,weights=area*normals[:,c],minlength=m) for c in range(3)])
    mean_normal/=np.maximum(np.linalg.norm(mean_normal,axis=1,keepdims=True),1e-12)
    # Graph edges are real surface neighbors; coincident disconnected shells
    # must already be excluded by the native surface builder.
    left=np.repeat(np.arange(n),3);right=neighbors.reshape(-1);ok=(right>=0)&(left<right)
    left,right=left[ok],right[ok];edge=np.unique(np.sort(np.column_stack((patch[left],patch[right])),axis=1),axis=0)
    edge=edge[edge[:,0]!=edge[:,1]];lab=_lab(mean_rgb)
    if len(edge):
        a,b=edge.T;distance=np.linalg.norm(lab[a]-lab[b],axis=1)
        cosine=np.einsum('ij,ij->i',mean_normal[a],mean_normal[b])
        keep=(distance<=8.)&(cosine>=.7)&~barrier[a]&~barrier[b]
        edge=edge[keep];edge_weight=np.exp(-(distance[keep]/6.)**2)*cosine[keep]
    else:edge_weight=np.empty(0)
    sums=np.zeros((m,6));weights=np.zeros(m);support=np.zeros((m,6),np.uint8);families=set()
    for family,ids,scores,direction in observations:
        if family in families:raise ValueError('Correlated view family counted twice')
        families.add(family)
        if (ids.ndim!=2 or ids.size>1024**2 or ids.dtype.kind not in 'iu' or
            scores.shape!=(*ids.shape,6) or not np.isfinite(scores).all() or
            np.any(scores<0) or np.any(scores>1) or np.any(ids < -1) or np.any(ids>=n) or
            np.shape(direction)!=(3,) or not np.isfinite(direction).all() or
            not np.isclose(np.linalg.norm(direction),1.,atol=1e-4) or
            not np.allclose(scores.sum(-1),1.,atol=1e-3)):
            raise ValueError('Invalid full view evidence')
        visible=ids>=0;face=ids[visible];prob=scores[visible];p=patch[face]
        pixels=np.bincount(face,minlength=n)
        weight=area[face]/pixels[face]*np.maximum(normals[face]@direction,0.)**2
        total=np.bincount(p,weights=weight,minlength=m)
        average=np.column_stack([np.bincount(p,weights=weight*prob[:,c],minlength=m) for c in range(6)])/np.maximum(total[:,None],1e-15)
        influence=np.minimum(total/patch_area,1.)
        sums+=average*influence[:,None];weights+=influence
        support+=(average>=.8)&(influence[:,None]>=.02)
    raw=sums/np.maximum(weights[:,None],1e-15);smoothed=raw.copy()
    if len(edge):
        a,b=edge.T;total=np.bincount(np.r_[a,b],weights=np.r_[edge_weight,edge_weight],minlength=m)
        for _ in range(3):
            message=np.zeros_like(raw)
            for c in range(6):
                message[:,c]=(np.bincount(a,weights=edge_weight*smoothed[b,c],minlength=m)+
                              np.bincount(b,weights=edge_weight*smoothed[a,c],minlength=m))/np.maximum(total,1e-15)
            smoothed=np.where((total>0)[:,None],.8*raw+.2*message,raw)
    winner=smoothed.argmax(1);ordered=np.sort(smoothed,axis=1)
    reliable=(ordered[:,-1]>=.9)&(ordered[:,-1]-ordered[:,-2]>=.2)&~barrier
    adjacency=[[] for _ in range(m)]
    for a,b in edge:adjacency[int(a)].append(int(b));adjacency[int(b)].append(int(a))
    output=labels.copy();proposal=np.full(m,-1,np.int32)
    for category,name in ((1,'hair'),(4,'cloth')):
        named=[i for i,v in enumerate(names) if v==name]
        if not named:continue
        anchor_area=np.bincount(patch[np.isin(labels,named)],weights=area[np.isin(labels,named)],minlength=m)
        eligible=reliable&(winner==category)&(support[:,category]>=2)
        reached=eligible&(anchor_area>=.05*patch_area);queue=np.flatnonzero(reached).tolist()
        for at in queue:
            for other in adjacency[at]:
                if eligible[other] and not reached[other]:reached[other]=True;queue.append(other)
        proposal[reached]=named[0]
    selected=(proposal[patch]>=0)&~protected
    output[selected]=proposal[patch[selected]]
    assert np.array_equal(output[protected],labels[protected])
    return output,dict(policy=POLICY,patches=m,graph_edges=len(edge),eligible_patches=int(reliable.sum()),
                       proposed_patches=int((proposal>=0).sum()),changed_faces=int((output!=labels).sum()),
                       changed_existing_coarse_faces=int(((output!=labels)&(labels>=0)).sum()),
                       protected_changes=0,scope='experimental editing guidance, not calibrated semantic evidence')
