"""Cross-sample offline material-region experiment; no fixed body coordinates.

Uses sampled source color, exact surface edges, physical area and automatic
face masks. This is a color-region heuristic, not whole-body semantic parsing
or intrinsic reflectance recovery. Source geometry and UVs are never changed.
"""
import numpy as np


def oklab(rgb):
    """sRGB 0..255 -> Oklab; Bjorn Ottosson's public-domain 2021 matrices.

    https://bottosson.github.io/posts/oklab/
    """
    c = np.asarray(rgb, dtype=np.float64) / 255
    if not np.isfinite(c).all() or np.any(c < 0) or np.any(c > 1):
        raise ValueError('Expected finite sRGB in [0,255]')
    c = np.where(c <= .04045, c / 12.92, ((c + .055) / 1.055)**2.4)
    lms = c @ np.array([[.4122214708,.2119034982,.0883024619],
                       [.5363325363,.6806995451,.2817188376],
                       [.0514459929,.1073969566,.6299787005]])
    return np.cbrt(lms) @ np.array([[.2104542553,1.9779984951,.0259040371],
                                  [.7936177850,-2.4285922050,.7827717662],
                                  [-.0040720468,.4505937099,-.8086757660]])


def nearest_palette(lab, palette):
    p = oklab(palette)
    return np.sum((np.asarray(lab)[...,None,:]-p)**2, axis=-1).argmin(axis=-1).astype(np.uint8)


def material_palette(lab, palette):
    """Heuristic hue retention when a saturated material lacks a close swatch.

    Neutral materials prefer neutral filaments if available. Saturated colors
    downweight baked brightness, so a red coat does not become gray merely
    because the available pink is lighter. This changes the objective, not the
    physical gamut; it is not a claim of minimum perceptual/printed error.
    """
    lab=np.asarray(lab);p=oklab(palette)
    chroma=np.linalg.norm(lab[...,1:],axis=-1);pc=np.linalg.norm(p[:,1:],axis=-1)
    delta=(lab[...,None,:]-p)**2
    lightness=np.where(chroma>=.06,.25,1.)
    cost=delta[...,0]*lightness[...,None]+delta[...,1]+delta[...,2]
    if np.any(pc<.045):
        cost=np.where((chroma[...,None]<.045)&(pc>=.045),np.inf,cost)
    return cost.argmin(axis=-1).astype(np.uint8)


def surface_edges(vertices, faces):
    """Exact seam welding for analysis only; manifold, opposite directed edges.

    No proximity edges, no weld tolerance and no mesh rewrite. Coincident
    non-manifold edges are excluded. Separate coincident closed shells are not
    a supported material topology and must not be claimed as tested.
    """
    _, inverse = np.unique(vertices, axis=0, return_inverse=True)
    f = inverse[faces]
    directed = f[:,[[0,1],[1,2],[2,0]]].reshape(-1,2)
    edges = np.sort(directed, axis=1)
    owner = np.repeat(np.arange(len(f)),3)
    key = edges[:,0].astype(np.int64)*(int(inverse.max())+1)+edges[:,1]
    order = np.argsort(key,kind='stable'); key=key[order]
    starts = np.r_[0,np.flatnonzero(np.diff(key))+1]
    count = np.diff(np.r_[starts,len(key)])
    pair = starts[count==2]
    ea,eb = order[pair],order[pair+1]
    good = np.all(directed[ea]==directed[eb,::-1],axis=1) & (owner[ea]!=owner[eb])
    ea,eb = ea[good],eb[good]
    original_edges=faces[:,[[0,1],[1,2],[2,0]]].reshape(-1,2)
    lengths=np.linalg.norm(vertices[original_edges[ea,0]]-vertices[original_edges[ea,1]],axis=1)
    degree=np.bincount(np.r_[owner[ea],owner[eb]],minlength=len(faces))
    return owner[ea],owner[eb],lengths,degree!=3


def components(labels, a, b):
    n=len(labels); parent=list(range(n)); rank=[0]*n
    def find(i):
        while parent[i]!=i:
            parent[i]=parent[parent[i]]; i=parent[i]
        return i
    same=labels[a]==labels[b]
    for i,j in zip(a[same].tolist(),b[same].tolist()):
        i,j=find(i),find(j)
        if i!=j:
            if rank[i]<rank[j]:i,j=j,i
            parent[j]=i
            if rank[i]==rank[j]:rank[i]+=1
    roots=np.fromiter((find(i) for i in range(n)),dtype=np.int32,count=n)
    return np.unique(roots,return_inverse=True)[1].astype(np.int32)


def color_clusters(rgb, areas, count=12):
    # Fit an area-weighted color histogram: independent of triangle density.
    rgb=np.asarray(rgb,dtype=np.float64);areas=np.asarray(areas,dtype=np.float64)
    if not np.isfinite(areas).all() or np.any(areas<0) or not np.any(areas>0):
        raise ValueError('Expected nonnegative physical areas with positive total')
    oklab(rgb)  # reject invalid source values before histogram rounding
    bins=np.floor(np.clip(rgb,0,255)/8).astype(np.int32)
    _,inverse=np.unique(bins,axis=0,return_inverse=True)
    mass=np.bincount(inverse,weights=areas)
    colors=np.column_stack([np.bincount(inverse,weights=areas*rgb[:,i])/np.maximum(mass,1e-20) for i in range(3)])
    valid=mass>0; colors,mass=colors[valid],mass[valid]
    signal=oklab(np.clip(colors,0,255))*[.45,1,1]
    centers=[signal[mass.argmax()]]
    for _ in range(min(count,len(signal))-1):
        distance=np.min(np.sum((signal[:,None]-centers)**2,axis=2),axis=1)
        centers.append(signal[np.argmax(distance*np.sqrt(mass))])
    centers=np.asarray(centers)
    for _ in range(24):
        ids=np.sum((signal[:,None]-centers)**2,axis=2).argmin(axis=1)
        updated=centers.copy()
        for k in range(len(centers)):
            use=ids==k
            if use.any():updated[k]=np.average(signal[use],axis=0,weights=mass[use])
        if np.max(np.abs(updated-centers))<1e-6:break
        centers=updated
    lab=oklab(rgb)
    # Bounded memory for million-face models.
    labels=np.empty(len(rgb),np.int32)
    for start in range(0,len(rgb),100000):
        signal=lab[start:start+100000]*[.45,1,1]
        labels[start:start+100000]=np.sum((signal[:,None]-centers)**2,axis=2).argmin(axis=1)
    return labels,lab


def merge_regions(labels, lab, areas, a, b, lengths, protected, unsafe):
    """One simultaneous area/boundary vote; never cascade into big regions."""
    roots=components(labels,a,b); n=int(roots.max())+1
    mass=np.bincount(roots,weights=areas,minlength=n)
    mean=np.column_stack([np.bincount(roots,weights=areas*lab[:,i],minlength=n)/np.maximum(mass,1e-20) for i in range(3)])
    locked=np.zeros(n,bool); locked[roots[protected|unsafe]]=True
    ra,rb=roots[a],roots[b]; use=ra!=rb
    ra,rb,w=ra[use],rb[use],lengths[use]
    src,dst=np.r_[ra,rb],np.r_[rb,ra]; w=np.r_[w,w]
    total=np.bincount(src,weights=w,minlength=n)
    compatible=(np.linalg.norm(mean[src,1:]-mean[dst,1:],axis=1)<.065)&(np.abs(mean[src,0]-mean[dst,0])<.30)
    eligible=(mass[dst]>=8*mass[src])&~locked[src]&compatible
    # Vote by neighboring region, not color alone: distant equal colors cannot
    # pool area to absorb an unrelated material region.
    keys=src[eligible].astype(np.int64)*n+dst[eligible]
    pairs,inv=np.unique(keys,return_inverse=True)
    votes=np.bincount(inv,weights=w[eligible])
    sources,targets=pairs//n,pairs%n
    winner=np.arange(n); best=np.zeros(n)
    for s,t,v in zip(sources,targets,votes):
        if v>best[s]:winner[s]=t;best[s]=v
    merge=(best>=.70*total)&(total>0)&~locked
    # A small absolute-area speck may be removed; large folds need the same
    # strong compatible boundary vote. Explicit/semantic detail locks win.
    selected=np.arange(n);selected[merge]=winner[merge]
    result=selected[roots]
    return result,{'regions_before':n,'merged_regions':int(merge.sum()),
                   'merged_area_mm2':float(areas[result!=roots].sum())}


def distill(rgb, areas, edges, semantic, confidence, schema, feature_lock):
    """Color regions plus measured skin/hair anchors, without body coordinates."""
    a,b,lengths,unsafe=edges
    labels,lab=color_clusters(rgb,areas)
    prototypes={}; roles={}
    for role,names in [('skin',('face','neck','nose','lr','rr')),('hair',('hair',))]:
        mask=(confidence>=.9)&np.isin(semantic,[schema.index(name) for name in names])&~feature_lock
        if not mask.any():continue
        representative=np.median(lab[mask],axis=0)
        prototypes[role]=representative
        # Only clusters supported mostly by this recognized tissue are
        # generalized by color. This is a hypothesis for unobserved surfaces.
        for k in np.unique(labels[mask]):
            selected=labels==k
            overlap=float(areas[selected&mask].sum()/max(areas[selected].sum(),1e-20))
            if overlap>=.35:
                labels[selected]=12+(role=='hair')
        labels[mask]=12+(role=='hair')
        roles[role]=mask
    # Each feature has a separate protected class supplied by its caller.
    labels[feature_lock]=14
    roots,stats=merge_regions(labels,lab,areas,a,b,lengths,feature_lock,unsafe)
    n=int(roots.max())+1
    mass=np.bincount(roots,weights=areas,minlength=n)
    mean=np.column_stack([np.bincount(roots,weights=areas*lab[:,i],minlength=n)/np.maximum(mass,1e-20) for i in range(3)])
    distilled=mean[roots]
    for role,mask in roles.items():
        class_id=12+(role=='hair')
        distilled[labels==class_id]=prototypes[role]
    # The caller handles eyes/brows/mouth from source detail, never a pooled
    # feature-wide representative.
    distilled[feature_lock]=lab[feature_lock]
    stats.update(prototypes={k:v.tolist() for k,v in prototypes.items()},
                 semantic_anchor_faces={k:int(v.sum()) for k,v in roles.items()})
    return distilled,stats
