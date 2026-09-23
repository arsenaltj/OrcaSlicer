"""Associate anonymous multi-view mask proposals on a bound mesh surface.

The output is a separate editing-region candidate. It never assigns semantic
labels, changes paint or touches the source mesh. -1 means unresolved.
"""
import numpy as np


def associate_masks(records, links, patch_area, protected, neighbors, face_patch):
    """Return per-patch anonymous IDs and a diagnostic summary.

    Each group contains at most one mask from each independent view. A patch
    needs two views supporting the same group. Nested or similarly sized
    conflicting groups abstain unless the smaller region is clearly distinct.
    """
    count=len(patch_area);n=len(face_patch)
    if (not 0<count<=n<=2_000_000 or protected.shape!=(count,) or
        neighbors.shape!=(n,3) or face_patch.shape!=(n,) or
        np.any(face_patch<0) or np.any(face_patch>=count) or
        not np.isfinite(patch_area).all() or np.any(patch_area<=0) or
        np.any(neighbors < -1) or np.any(neighbors >=n) or len(records)>2048):
        raise ValueError('Invalid bound surface or proposal count')
    parents=list(range(len(records)));views=[{int(r['view'])} for r in records]
    for r in records:
        ids=np.asarray(r['patches'],dtype=np.int64)
        if (int(r['view'])<0 or int(r['view'])>=8 or ids.ndim!=1 or
            np.any(ids<0) or np.any(ids>=count) or len(np.unique(ids))!=len(ids) or
            len(ids)!=len(r['coverage']) or len(ids)!=len(r['visible_fraction']) or
            not np.isfinite(r['score']) or not 0<=r['score']<=1 or
            np.any(~np.isfinite(r['coverage'])) or np.any(~np.isfinite(r['visible_fraction'])) or
            np.any(np.asarray(r['coverage'])<0) or np.any(np.asarray(r['coverage'])>1+1e-8) or
            np.any(np.asarray(r['visible_fraction'])<0) or np.any(np.asarray(r['visible_fraction'])>1+1e-8)):
            raise ValueError('Invalid projected mask')
    def root(i):
        while parents[i]!=i:
            parents[i]=parents[parents[i]];i=parents[i]
        return i
    for link in sorted(links,key=lambda x:(-min(1.,float(x['shared_iou'])),x['a'],x['b'])):
        a,b=int(link['a']),int(link['b'])
        iou=float(link['shared_iou'])
        # A ratio of independently summed surface areas can exceed one by one
        # floating-point ULP when the two masks cover the same visible faces.
        if (not 0<=a<len(records) or not 0<=b<len(records) or not np.isfinite(iou) or
            not 0<=iou<=1+8*np.finfo(float).eps):
            raise ValueError('Invalid view association')
        a,b=root(a),root(b)
        if a==b or views[a]&views[b]:continue
        if a>b:a,b=b,a
        parents[b]=a;views[a]|=views[b]
    groups={}
    for i in range(len(records)):groups.setdefault(root(i),[]).append(i)
    # Vote once per family; members of a group already have distinct views.
    candidates=[]
    for members in groups.values():
        if len(members)<2:continue
        vote=np.zeros(count,np.uint8);confidence=np.zeros(count,np.float32)
        for i in members:
            r=records[i];ids=np.asarray(r['patches'],dtype=np.int64)
            usable=np.asarray(r['visible_fraction'])>=.03
            ids=ids[usable]
            vote[ids]+=1
            confidence[ids]+=(float(r['score'])*np.asarray(r['coverage'])[usable]).astype(np.float32)
        keep=(vote>=2)&~protected
        if not keep.any():continue
        candidates.append((keep,confidence,float(patch_area[keep].sum()),members))
    selected=np.full(count,-1,np.int32);selected_area=np.full(count,np.inf)
    conflict=np.zeros(count,bool)
    # A smaller nested component has priority (eyes inside a whole face); when
    # two candidates have comparable footprint, an overlap is unresolved.
    for group,(keep,_,size,_) in enumerate(candidates):
        empty=keep&(selected<0);selected[empty]=group;selected_area[empty]=size
        overlap=keep&~empty
        conflict[overlap&(size>=.7*selected_area)&(selected_area>=.7*size)]=True
        smaller=overlap&(size<.7*selected_area);selected[smaller]=group;selected_area[smaller]=size
    selected[conflict|protected]=-1
    # A mask may cover disconnected shells. Split each selected group along the
    # exact native face graph, then compress to one ID per connected patch.
    left=np.repeat(np.arange(n),3);right=neighbors.reshape(-1)
    valid=(right>=0)&(left<right)
    left,right=left[valid],right[valid]
    pa,pb=face_patch[left],face_patch[right]
    valid=(pa!=pb)&(selected[pa]>=0)&(selected[pa]==selected[pb])
    graph=[[] for _ in range(count)]
    for a,b in zip(pa[valid],pb[valid]):graph[int(a)].append(int(b));graph[int(b)].append(int(a))
    output=np.full(count,-1,np.int32);next_id=0
    for seed in np.flatnonzero(selected>=0):
        if output[seed]>=0:continue
        queue=[int(seed)];output[seed]=next_id
        for at in queue:
            for other in graph[at]:
                if output[other]<0 and selected[other]==selected[seed]:
                    output[other]=next_id;queue.append(other)
        next_id+=1
    raw_regions=next_id
    if next_id:
        group_area=np.bincount(output[output>=0],weights=patch_area[output>=0],minlength=next_id)
        too_small=np.flatnonzero(group_area < patch_area.sum()/2800.)
        if len(too_small):output[np.isin(output,too_small)]=-1
        surviving=np.unique(output[output>=0]);remap=np.full(next_id,-1,np.int32)
        remap[surviving]=np.arange(len(surviving),dtype=np.int32)
        good=output>=0;output[good]=remap[output[good]];next_id=len(surviving)
    assert np.all(output[protected]<0)
    return output,dict(projected_masks=len(records),associated_groups=len(groups),supported_groups=len(candidates),
                       raw_connected_regions=raw_regions,connected_regions=next_id,covered_patches=int((output>=0).sum()),
                       covered_area=float(patch_area[output>=0].sum()),conflicted_patches=int(conflict.sum()))
