"""Current-source parent proposals; colors assist seeded boundaries, never create identity.

The native consumer repeats mesh/identity/occlusion proofs, subtracts saved detail
cells and performs bounded incremental clipping. No palette or paid service here.
"""
from collections import defaultdict

import cv2
import numpy as np
from scipy.ndimage import binary_dilation

from local_leaf_boundaries import area, clip_triangle
from local_parent_boundary import refine
from local_semantic_projection import LABEL_NAMES
from local_semantic_render import project, shade

POLICY = dict(version='current-parent-coverage-v2', minimum_independent_views=2,
              semantic_confidence=.9, root_coverage=1., coverage_tolerance=1e-10, fixed_topology_rings=3,
              grabcut_iterations=5, source_only=True, correlated_crops_are_one_vote=True,
              native_occlusion_required=True, mixed_root_whole_color=False,
              local_pixel_band=6, contour_epsilon_pixels=.12)
POLICY['occluded_projection_votes_are_not_conflicts']=True
ROOT = np.eye(3)
SAMPLES = np.array([[1/3]*3, [.98,.01,.01], [.01,.98,.01], [.01,.01,.98],
                    [.49,.49,.02], [.02,.49,.49], [.49,.02,.49]])
# Interior samples of a fixed barycentric lattice. A narrow clipped sibling
# without a source-color sample is preserved by the native consumer.
COLOR_SAMPLES = np.concatenate((SAMPLES, np.array([
    [(i+1/3)/8, (j+1/3)/8, 1-(i+j+2/3)/8]
    for i in range(8) for j in range(8-i)])))
CLASSES = {'hair':1, 'skin':3, 'cloth':4}


def whole_root_labels(category, coverage):
    """A partial semantic mask never grants whole-source-triangle authority."""
    return np.where(coverage + POLICY['coverage_tolerance'] >= POLICY['root_coverage'],
                    category, 0).astype(np.uint8)


def face_parent(label):
    return 3 if label in ('face','nose','lr','rr','neck','body-skin') else CLASSES.get(label,0)


def bounded_scope(seeds, neighbors, barriers, rings=3):
    scope=set(seeds)-set(barriers)
    for _ in range(rings):
        scope |= {int(n) for f in tuple(scope) for n in neighbors[f] if n>=0 and n not in barriers}
    return scope


def mapped_labels(labels, confidence):
    result=np.zeros(labels.shape,np.uint8)
    for i,name in enumerate(LABEL_NAMES):
        parent=face_parent(name)
        if parent: result[(labels==i)&(confidence>=.9)]=parent
        elif name!='background': result[(labels==i)&(confidence>=.9)]=5
    return result


def observe(camera, family, rgb, ids, depths, bary, face_labels, face_quality, body=None):
    labels=np.zeros(ids.shape,np.uint8);quality=np.zeros(ids.shape,np.float32)
    if body is not None:
        _,_,raw,score=body
        labels=raw.copy();labels[labels==2]=3
        labels[~np.isin(labels,(1,3,4))]=0
        quality=score.copy()
    for raw,score in zip(face_labels,face_quality):
        mapped=mapped_labels(raw,score)
        take=(mapped>0)&(score>=.9)
        labels[take]=mapped[take];quality[take]=score[take]
    labels[ids<0]=0
    return dict(camera=camera,family=family,rgb=rgb,ids=ids,depths=depths,bary=bary,
                labels=labels,quality=quality)


def family_votes(records, count):
    """A focus crop refines its parent camera's vote, never counts as another camera."""
    families=defaultdict(list)
    for index,record in enumerate(records): families[record['family']].append(index)
    columns=[];views=[]
    for family,indices in sorted(families.items()):
        selected=np.zeros(count,np.uint8);selected_view=np.full(count,-1,np.int16)
        for i in sorted(indices,key=lambda i:records[i]['resolution']):
            labels=records[i]['root_labels'];take=labels!=255
            selected[take]=labels[take];selected_view[take]=i
        columns.append(selected);views.append(selected_view)
    votes=np.stack(columns,axis=1)
    support=(votes>0).sum(1)
    low=np.where(votes>0,votes,255).min(1);high=votes.max(1)
    conflict=(support>=2)&(low!=high)
    accepted=np.where((support>=2)&~conflict,high,0).astype(np.uint8)
    return accepted,conflict,np.stack(views,axis=1)


def source_colors(face_ids, faces, uv, colors, materials, material_ids, samples=SAMPLES):
    """Same GLB UV sampler as recognition, evaluated inside each source triangle."""
    result={}
    for start in range(0,len(face_ids),8192):
        selected=np.asarray(face_ids[start:start+8192],np.int32)
        ids=np.broadcast_to(selected[:,None],(len(selected),len(samples)))
        bary=np.broadcast_to(samples,(*ids.shape,3))
        rgb=shade(faces,uv,colors,materials,material_ids,ids,bary).astype(float)/255.
        for face,sampled_rgb in zip(selected,rgb): result[int(face)]=sampled_rgb
    return result


def build(observations, regions, parent_details, shapes, vertices, faces, uv, colors,
          materials, material_ids, cancelled=None):
    from local_face_landmarks import surface_neighbors
    from local_parent_projection import coverage_labels
    checkpoint=cancelled or (lambda:False)
    subjects=sorted({r['subject_id'] for r in regions})
    if len(subjects)!=1 or not observations:
        return None
    subject=subjects[0];count=len(faces)
    known=np.zeros(count,np.uint8);barriers=set();reliable=set()
    for region in regions:
        category=face_parent(region['label'])
        for f,confidence,dominance,_,views in region['samples']:
            if category and confidence>=.9 and dominance>=.95 and views>=2:
                known[f]=category;reliable.add(f)
            elif not category: barriers.add(f)
    for f,_,_,score,_,views in parent_details:
        if score>=.9 and views>=2:known[f]=3;reliable.add(f)
    for shape in shapes:
        barriers.update(shape['accepted_faces']);barriers.update(shape['rejected_faces'])
    neighbors=surface_neighbors(vertices,faces)
    # The completion budget is measured from fixed evidence seeds, never from
    # already repaired colors. A classifier's direct pixel witnesses may add
    # new seeds, but a color-compatible unknown cannot start a new component.
    references=[];reference_labels=[]
    for observation in observations:
        ids=observation['ids'];valid=ids>=0
        target=np.zeros(ids.shape,np.uint8);target[valid]=known[ids[valid]]
        for category in CLASSES.values():
            take=(target==category)&valid
            pixels=observation['rgb'][take]
            if len(pixels):
                pixels=pixels[::max(1,len(pixels)//2048)]
                references.extend(pixels.tolist());reference_labels.extend([category]*len(pixels))
    reference=None
    if references:
        reference=(np.asarray(references,np.uint8)[:,None,:],np.asarray(reference_labels,np.uint8)[:,None])
    records=[];mixed=defaultdict(lambda:defaultdict(list));library={};view_audit=[]
    cameras=[];visible_scope=set()
    for observation in observations:
        if checkpoint():raise RuntimeError('parent_coverage_cancelled')
        camera=observation['camera'];ids=observation['ids'];valid=ids>=0
        rgb=observation['rgb'];known_pixels=np.zeros(ids.shape,np.uint8)
        known_pixels[valid]=known[ids[valid]]
        # Detail pixels are barriers; their entire source triangle is not. The
        # saved native partition subtracts locked siblings from mixed proposals.
        protected=(observation['labels']==5)
        foreground=valid&~protected
        # No arbitrary expansion beyond actual semantic/seed pixel support.
        seed=(known_pixels>0)|((observation['quality']>=.9)&np.isin(observation['labels'],(1,3,4)))
        allowed=binary_dilation(seed,iterations=POLICY['local_pixel_band'])&foreground
        selected,quality,report=refine(rgb,allowed,observation['labels'],observation['quality'],known_pixels,reference=reference)
        selected[~allowed]=0;quality[~allowed]=0
        xy=project(vertices,camera.basis,camera.center,camera.half_height,camera.size)[faces][...,:2]
        lower=xy.min(1);upper=xy.max(1)
        in_frame=(lower[:,0]>=0)&(lower[:,1]>=0)&(upper[:,0]<=ids.shape[1])&(upper[:,1]<=ids.shape[0])
        # Backfaces and depth/occlusion are checked again natively, including
        # triangles smaller than a pixel. Raster occupancy is not required.
        category,coverage=coverage_labels(xy,selected,quality)
        category=whole_root_labels(category,coverage)
        root_labels=np.where(in_frame,category,255).astype(np.uint8)
        visible_faces=np.unique(ids[valid]);visible_scope.update(map(int,visible_faces))
        record_index=len(records)
        records.append(dict(family=observation['family'],resolution=camera.size/camera.half_height,root_labels=root_labels))
        cameras.append(dict(family=observation['family'],direction=camera.basis[2].tolist(),distance=float(np.ptp(vertices,axis=0).max()*4+1)))
        view_audit.append(dict(name=camera.name,family=observation['family'],root_candidates=int((category>0).sum()),refinement=report))
        boundary=visible_faces[(category[visible_faces]==0)&in_frame[visible_faces]]
        boundaries={}
        for parent,c in CLASSES.items():
            outlines,hierarchy=cv2.findContours((selected==c).astype(np.uint8),cv2.RETR_CCOMP,cv2.CHAIN_APPROX_SIMPLE)
            boundaries[c]=[]
            for i,outline in enumerate(outlines):
                if cv2.contourArea(outline)<1:continue
                polygon=cv2.approxPolyDP(outline,POLICY['contour_epsilon_pixels'],True).reshape(-1,2).astype(float)+.5
                if len(polygon)<3:continue
                key=f'parent/{record_index}/{c}/{i}'
                library[key]=polygon.tolist()
                boundaries[c].append((key,polygon,hierarchy[0,i,3]>=0))
        for face in boundary:
            triangle=xy[face]
            if area(triangle)<.02:continue
            try:inverse=np.linalg.inv(np.column_stack((triangle,np.ones(3))))
            except np.linalg.LinAlgError:continue
            if np.abs(inverse).max()>1e5:continue
            for c,curves in boundaries.items():
                keys=[];holes=[]
                for key,polygon,is_hole in curves:
                    if np.any(polygon.max(0)<lower[face]) or np.any(polygon.min(0)>upper[face]):continue
                    (holes if is_hole else keys).append(key)
                if keys:
                    mixed[int(face)][c].append(dict(family=observation['family'],camera=record_index,
                        matrix=inverse.tolist(),contour_ids=keys,exclude_contour_ids=holes))
    accepted,conflicting,vote_views=family_votes(records,count)
    # Establish direct semantic seeds using raster witnesses, then complete at
    # most three real surface rings inside the independently supported masks.
    direct=set(reliable)
    for observation in observations:
        ids=observation['ids'];take=(ids>=0)&(observation['quality']>=.9)&np.isin(observation['labels'],(1,3,4))
        direct.update(map(int,ids[take]))
    scope=bounded_scope(direct,neighbors,(),POLICY['fixed_topology_rings'])
    # Projection onto an occluded triangle can show someone else's hair/cloth.
    # Native source-ray proof decides which votes are real before resolving
    # conflicts. Projected disagreement alone cannot erase the front surface.
    projected_votes=np.zeros((count,vote_views.shape[1]),np.uint8)
    for column in range(vote_views.shape[1]):
        for view in np.unique(vote_views[:,column]):
            if view>=0:
                selected=vote_views[:,column]==view
                projected_votes[selected,column]=records[int(view)]['root_labels'][selected]
    candidates=((projected_votes==3).sum(1)>=2)|((projected_votes==4).sum(1)>=2)
    root_ids=[int(f) for f in np.flatnonzero(candidates) if f in scope]
    root_set=set(root_ids)
    # Root-class disagreement remains blocked. It is not a substitute for
    # clipping two classes within the same independently supported triangle.
    mixed_ids=[f for f in sorted(mixed) if f in scope and f not in root_set and not accepted[f]]
    samples=source_colors(sorted(set(root_ids)|set(mixed_ids)),faces,uv,colors,materials,material_ids)
    mixed_colors=source_colors(mixed_ids,faces,uv,colors,materials,material_ids,COLOR_SAMPLES)
    roots=[];parts=[]
    for face in root_ids:
        category=3 if (projected_votes[face]==3).sum()>=2 else 4
        views=sorted(int(i) for i in vote_views[face] if i>=0 and records[int(i)]['root_labels'][face]==category)
        votes=[[int(i),int(records[int(i)]['root_labels'][face])] for i in vote_views[face]
               if i>=0 and records[int(i)]['root_labels'][face] in (1,3,4)]
        roots.append([face,category,views,np.median(samples[face],axis=0).tolist(),votes])
    for face in mixed_ids:
        layers=[]
        for c,views in mixed[face].items():
            independent={}
            for view in views:
                family=view['family']
                if family not in independent or records[view['camera']]['resolution']>records[independent[family]['camera']]['resolution']:
                    independent[family]=view
            views=list(independent.values())
            if c==1 or len({v['family'] for v in views})<2:continue
            layers.append(dict(label='skin' if c==3 else 'cloth',parent_label='skin' if c==3 else 'cloth',
                subject_id=subject,views=views,kind='CURRENT_PARENT_CLIP'))
        if layers:parts.append(dict(source_face_id=face,layers=layers,source_samples=samples[face].tolist(),
                                   color_samples=mixed_colors[face].tolist()))
    return dict(schema='orca.portrait-parent-proposal/v1',subject_id=subject,policy=POLICY,
                roots=roots,mixed=parts,cameras=cameras,curve_library=library,color_barycentric=COLOR_SAMPLES.tolist(),
                scope_faces=sorted(visible_scope),audit=dict(views=view_audit,root_proposals=len(roots),
                mixed_proposals=len(parts),cross_view_conflicts=int(conflicting.sum()),
                direct_seed_faces=len(direct),bounded_scope_faces=len(scope),frozen_source_faces=len(barriers),
                added_by_color_only=0))
