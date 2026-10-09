"""Audit all final contour edges, including old limits and unresolved source RGB."""
import argparse
from collections import Counter, defaultdict
import json
from pathlib import Path

import cv2
import numpy as np
from PIL import Image, ImageDraw
from scipy.spatial import cKDTree

from beauty_leaf_domain import LeafKey
from local_face_landmarks import surface_neighbors
from local_semantic_geometry import read
from portrait_r5_review_audit import oklab
from portrait_r5_visual_review import publish
from portrait_r6_repair import verify
from portrait_r7_review import camera_identity
from portrait_surface_ownership import trusted_regions
from surface_partition import load, inside, inside_analytic


def categories(label):
    return (label,label[5:]) if label.startswith('iris-') else (label,)


def at(face,point,by_face,old):
    row=by_face.get(face)
    if row:
        labels=set()
        for cell in row['cells']:
            if inside_analytic(cell['polygon'],[point])[0] and not any(inside_analytic(h,[point])[0] for h in cell['holes']):
                labels.update(categories(cell['label']))
        return tuple(sorted(labels))
    for key,label in old.get(face,[]):
        if np.linalg.solve(key.corners().T,point).min()>=-1e-7:
            return categories(label)
    return ()


def follows_permission_edge(a,b,layers):
    for layer in layers:
        for polygon in layer.get('parent_polygons',[]):
            for ring in [polygon['polygon']]+polygon.get('holes',[]):
                points=np.asarray(ring,dtype=float)[:,1:]
                for start,end in zip(points,np.roll(points,-1,axis=0)):
                    direction=end-start
                    length=float(direction@direction)
                    if length<1e-16:
                        continue
                    endpoints=np.array([a[1:],b[1:]])-start
                    t=endpoints@direction/length
                    distance=np.linalg.norm(endpoints-t[:,None]*direction,axis=1)
                    if np.max(distance)<1e-8 and t.min()>=-1e-7 and t.max()<=1+1e-7:
                        return True
    return False


def seam_audit(document,request):
    vertices={row['source_face_id']:row.get('source_vertices') for row in request['faces']}
    edges=defaultdict(dict)
    triangle_edges=defaultdict(dict)
    for face in document['faces']:
        root=face['source_face_id']
        source=vertices[root]
        if source is None:
            raise ValueError('Missing frozen welded edge mapping')
        for cell in face['cells']:
            for rings,destination in (([cell['polygon']]+cell['holes'],edges),(cell['triangles'],triangle_edges)):
                for ring in rings:
                    for bary in ring:
                        for zero in np.flatnonzero(np.abs(bary)<1e-9):
                            a,b=(int(zero)+1)%3,(int(zero)+2)%3
                            key=tuple(sorted((source[a],source[b])))
                            coordinate=bary[b] if source[a]<source[b] else bary[a]
                            destination[key].setdefault(root,set()).add(int(round(coordinate*1e9)))
    mismatches=[]
    checked=0
    for edge,roots in edges.items():
        if len(roots)<2:
            continue
        checked+=1
        values=list(roots.values())
        if any(value!=values[0] for value in values[1:]):
            mismatches.append(dict(welded_edge=edge,source_faces=sorted(roots)))
    dropped=[]
    for edge,roots in edges.items():
        for root,positions in roots.items():
            if not positions.issubset(triangle_edges.get(edge,{}).get(root,set())):
                dropped.append(dict(welded_edge=edge,source_face_id=root))
    return dict(shared_edges_checked=checked,mismatches=mismatches,dropped_tessellation_intersections=dropped,
                pass_no_t_junctions=not mismatches and not dropped,
                tolerance_barycentric=1e-9)


def contour_audit(run,partition_folder,proposal_folder,output):
    manifest=verify(run)
    request=json.loads((proposal_folder/'request.json').read_text())
    expected=request['identity']
    document,_=load(partition_folder,expected)
    vertices,faces,_=read(run/'baseline/native.bin',manifest['source_sha256'])
    neighbors=surface_neighbors(vertices,faces)
    by_face={r['source_face_id']:r for r in document['faces']}
    requests={r['source_face_id']:r for r in request['faces']}
    old=defaultdict(list)
    locks=json.loads((run/'baseline/shape-locks.json').read_text())
    for lock in locks['locks']:
        nested={tuple(k) for k in lock['nested_leaves']}
        for row in lock['locked_leaves']:
            old[row[0]].append((LeafKey(*row),'iris-'+lock['label'] if tuple(row) in nested else lock['label']))
    curves=json.loads((proposal_folder/'fitted-contours.json').read_text())
    models=defaultdict(list)
    for label,views in curves.items():
        for view in views:
            transform=np.asarray(view['transform'])*view['scale']
            contours=view['contours']+view.get('holes',[])
            points=np.vstack(contours)
            ends=np.vstack([np.roll(np.asarray(c),-1,axis=0) for c in contours])
            previous=np.concatenate([np.roll(np.arange(len(c))+sum(len(p) for p in contours[:i]),1) for i,c in enumerate(contours)])
            models[label].append((transform,cKDTree(points),points,ends,previous))
            if label in ('le','re'):
                iris=np.vstack(view['iris'])
                ends=np.vstack([np.roll(np.asarray(c),-1,axis=0) for c in view['iris']])
                previous=np.concatenate([np.roll(np.arange(len(c))+sum(len(p) for p in view['iris'][:i]),1) for i,c in enumerate(view['iris'])])
                models['iris-'+label].append((transform,cKDTree(iris),iris,ends,previous))
    # Boolean boundaries can come from any operand: eyelid clipping an iris,
    # iris subtracting sclera, or original pigment subtracting eye white.
    for side in ('le','re'):
        opening=list(models[side])
        iris=list(models['iris-'+side])
        accessory=list(models['periocular-'+side])
        models['iris-'+side]=iris+opening
        models[side]=opening+iris+accessory
        models['periocular-'+side]=accessory+iris+opening
    cameras={(r['scope'],r['view']):r for r in json.loads((run/'baseline/review-manifest.json').read_text())['images']}
    camera=camera_identity(cameras['face','front'],4096)
    basis,center=np.asarray(camera['basis']),np.asarray(camera['center'])
    pixel_scale=4096/(2*camera['half_height'])
    def project(world):
        p=(world-center) @ basis.T
        return np.column_stack((2048+p[:,0]*pixel_scale,2048-p[:,1]*pixel_scale))
    segments=[]
    seen=set()
    labels={'le','re','lb','rb','ulip','llip','iris-le','iris-re','periocular-le','periocular-re'}
    for root,row in by_face.items():
        world_face=vertices[faces[root]]
        for cell in row['cells']:
            for label in set(categories(cell['label']))&labels:
                for ring in [cell['polygon']]+cell['holes']:
                    polygon=np.asarray(ring)
                    for a,b in zip(polygon,np.roll(polygon,-1,axis=0)):
                        if np.linalg.norm(a-b)<1e-10: continue
                        signature=(root,label,tuple(np.round(np.minimum(a,b),9)),tuple(np.round(np.maximum(a,b),9)))
                        if signature in seen: continue
                        seen.add(signature)
                        midpoint=(a+b)*.5
                        zero=np.flatnonzero((np.abs(a)<1e-9)&(np.abs(b)<1e-9))
                        retained=False
                        if len(zero):
                            neighbor=int(neighbors[root,(int(zero[0])+1)%3])
                            if neighbor>=0:
                                v=vertices[faces[neighbor]]
                                p=np.linalg.lstsq(np.vstack((v.T,np.ones(3))),np.r_[midpoint@world_face,1.],rcond=None)[0]
                                if label in at(neighbor,p,by_face,old): continue
                            retained=True
                        else:
                            delta=b[1:]-a[1:]
                            normal=np.array([-delta[1],delta[0]])/np.linalg.norm(delta)*1e-7
                            p=midpoint.copy(); q=midpoint.copy()
                            p[1:]+=normal; p[0]=1-p[1:].sum()
                            q[1:]-=normal; q[0]=1-q[1:].sum()
                            if (label in at(root,p,by_face,old))==(label in at(root,q,by_face,old)): continue
                        screen=project(np.array([a,b])@world_face)
                        sample_count=max(9,int(np.ceil(np.linalg.norm(screen[1]-screen[0])/.25))+1)
                        samples=np.linspace(a,b,sample_count) @ world_face
                        errors=[]
                        for transform,tree,points,ends,previous in models.get(label,[]):
                            xy=np.column_stack((samples,np.ones(len(samples)))) @ transform
                            _,indices=tree.query(xy,k=min(4,len(points)))
                            indices=np.asarray(indices).reshape(len(xy),-1)
                            indices=np.column_stack((indices,previous[indices]))
                            start,end=points[indices],ends[indices]
                            direction=end-start
                            t=np.sum((xy[:,None]-start)*direction,axis=2)/np.maximum(np.sum(direction**2,axis=2),1e-16)
                            candidates=start+np.clip(t,0,1)[:,:,None]*direction
                            distance=np.sum((candidates-xy[:,None])**2,axis=2)
                            fitted=candidates[np.arange(len(xy)),distance.argmin(1)]
                            matrix=np.column_stack((np.column_stack((world_face,np.ones(3))) @ transform,np.ones(3)))
                            if abs(np.linalg.det(matrix))<1e-8: continue
                            bary=np.column_stack((fitted,np.ones(len(fitted)))) @ np.linalg.inv(matrix)
                            nearest=bary @ world_face
                            errors.append(np.linalg.norm(project(nearest)-project(samples),axis=1))
                        error=float(np.max(np.min(np.asarray(errors),axis=0))) if errors else None
                        parent_limit=follows_permission_edge(a,b,requests[root]['layers'])
                        kind='R6_LOCAL_FALLBACK_RETAINED' if row['status'].endswith('FALLBACK') else \
                             'CONFIRMED_PARENT_LEAF_LIMIT_RETAINED' if parent_limit else \
                             'R6_OR_SOURCE_FACE_LIMIT_RETAINED' if retained else 'TRUE_CONTOUR_CLIP'
                        segments.append(dict(source_face_id=root,cell_id=cell['id'],label=label,
                            barycentric=[a.tolist(),b.tolist()],screen_4096=screen.tolist(),sample_count=sample_count,
                            length_pixels_4096=float(np.linalg.norm(screen[1]-screen[0])),
                            kind=kind,max_deviation_pixels_4096=error,
                            met_target=error is not None and error<=1.,face_status=row['status']))
    summary=[]
    for label in sorted(labels):
        rows=[r for r in segments if r['label']==label]
        measured=[r['max_deviation_pixels_4096'] for r in rows if r['max_deviation_pixels_4096'] is not None]
        summary.append(dict(label=label,segments=len(rows),true_clip_segments=sum(r['kind']=='TRUE_CONTOUR_CLIP' for r in rows),
            retained_segments=sum(r['kind']!='TRUE_CONTOUR_CLIP' for r in rows),
            retained_kinds=dict(Counter(r['kind'] for r in rows if r['kind']!='TRUE_CONTOUR_CLIP')),
            true_clip_over_target=sum(not r['met_target'] and r['kind']=='TRUE_CONTOUR_CLIP' for r in rows),
            true_clip_max_deviation_pixels_4096=max((r['max_deviation_pixels_4096'] for r in rows if
                r['kind']=='TRUE_CONTOUR_CLIP' and r['max_deviation_pixels_4096'] is not None),default=None),
            perimeter_pixels_4096=sum(r['length_pixels_4096'] for r in rows),
            retained_perimeter_pixels_4096=sum(r['length_pixels_4096'] for r in rows if r['kind']!='TRUE_CONTOUR_CLIP'),
            over_target_perimeter_pixels_4096=sum(r['length_pixels_4096'] for r in rows if not r['met_target']),
            over_one_pixel_segments=sum(not r['met_target'] for r in rows),max_deviation_pixels_4096=max(measured,default=None)))
    output.mkdir(parents=True,exist_ok=False)
    publish(output/'contour-error.json',dict(schema='orca.r7-contour-error/v1',camera=camera,target_pixels_4096=1.,
        partition_sha256=document['partition_sha256'],boundary_policy_sha256=document['boundary_policy_sha256'],
        summary=summary,segments=segments,all_final_segments_in_denominator=True,
        source_fit_status='PENDING_USER',measurement='Conservative nearest dense fitted-curve witness mapped back to the same source face.',
        edge_sample_spacing_pixels_4096=.25,seams=seam_audit(document,request),
        overall_smoothness_pass=all(r['met_target'] for r in segments),production_enabled=False))
    print(json.dumps(summary),flush=True)


def residual_audit(run,partition_folder,review_folder,contour_folder,output):
    manifest=verify(run)
    evidence=json.loads((run/'baseline/evidence.json').read_text())
    records,conflicts=trusted_regions(evidence)
    ownership_report=json.loads((run/'baseline/ownership-report.json').read_text())
    sidecar=json.loads((partition_folder/'shape-locks.json').read_text())
    document=json.loads((partition_folder/sidecar['partition_ref']['path']).read_text())
    lookup=[dict(c,source_face_id=f['source_face_id']) for f in document['faces'] for c in f['cells']]
    with np.load(review_folder/'front-audit.npz',allow_pickle=False) as data:
        source,r6,r7,ids,bary,provenance,depths,paths,mapping,old_labels=(data[k] for k in
            ('source','r6','r7','ids','bary','provenance','leaf_depth','leaf_path','cell','r6_labels'))
    from portrait_r6_repair import load_render
    render_data=load_render(run)
    _,_,faces,uv,_,_,_=render_data
    target=(mapping>=0)
    features=np.zeros(ids.shape,dtype=bool)
    accessory=np.zeros(ids.shape,dtype=bool)
    new_brows={label:np.zeros(ids.shape,dtype=bool) for label in ('lb','rb')}
    for index,cell in enumerate(lookup):
        if cell['label'] in ('le','re','lb','rb','ulip','llip') or cell['label'].startswith(('iris-','periocular-')):
            features |= mapping==index
        if cell['label'] in new_brows:
            new_brows[cell['label']] |= mapping==index
        if cell['label'].startswith('periocular-'):
            accessory |= mapping==index
    palette=json.loads((run/'baseline/colors-5/portrait-color-plan.json').read_text())['palette']
    lip=np.rint(np.array(next(s['rgb'] for s in palette if s['uid']=='portrait-lips'))*255).astype(np.uint8)
    pink=(ids>=0)&~features&np.all(r7==lip,axis=2)
    lab=oklab(r7)
    warm=(ids>=0)&~features&~pink&(lab[:,:,0]<.75)&(lab[:,:,1]>.005)&(lab[:,:,2]>.01)
    scope=(old_labels==1)&~features
    face_locator=np.isin(ids,ownership_report['scope_face_ids'])|(old_labels==3)
    y,x=np.nonzero(face_locator)
    x0,x1,y0,y1=int(x.min()),int(x.max()+1),int(y.min()),int(y.max()+1)
    edge=cv2.morphologyEx(face_locator.astype(np.uint8),cv2.MORPH_GRADIENT,np.ones((3,3),np.uint8))
    band=cv2.dilate(edge,np.ones((17,17),np.uint8)).astype(bool)
    yy,xx=np.indices(ids.shape)
    hairline=band&(ids>=0)&(xx>=x0+.7*(x1-x0))&(yy<y0+.65*(y1-y0))
    provenance_names=('SOURCE_TEXTURE','R6_ROOT_SLOT','R6_SUBFACE_COLOR','MANUAL_COLOR')
    components=[]
    class_counts=Counter()
    for kind,mask in (('PINK_SLOT_RGB',pink),('WARM_DARK_DIAGNOSTIC',warm)):
        _,parts,stats,_=cv2.connectedComponentsWithStats(mask.astype(np.uint8),8)
        for part in range(1,len(stats)):
            pixels=np.flatnonzero(parts==part)
            keys=defaultdict(list)
            for pixel in pixels:
                root=int(ids.ravel()[pixel]); origin=int(provenance.ravel()[pixel])
                labels=records.get(root,{})
                ownership='EXPLICIT_CONFLICT' if root in conflicts else \
                    'CONFIRMED_R6_SKIN_LEAF' if old_labels.ravel()[pixel]==1 else \
                    'CONFIRMED_R6_CLOTH_LEAF' if old_labels.ravel()[pixel]==2 else \
                    'VERIFIED_HAIR' if 'hair' in labels else 'COARSE_SKIN_ONLY' if set(labels)&{'face','nose'} else \
                    'UNRESOLVED_HAIR_SKIN_OR_SOURCE_SHADOW'
                key=(root,int(depths.ravel()[pixel]),int(paths.ravel()[pixel]),origin,ownership,int(mapping.ravel()[pixel]))
                keys[key].append(int(pixel)); class_counts[ownership]+=1
            leaves=[]
            for (root,depth,path,origin,owner,index),samples in sorted(keys.items()):
                weights=bary.reshape(-1,3)[samples]
                tex=weights@uv[faces[root]]
                leaves.append(dict(source_face_id=root,leaf=[root,depth,path],pixels=len(samples),
                    cell_id=lookup[index]['id'] if index>=0 else None,color_source=provenance_names[origin],
                    ownership=owner,uv_min=tex.min(0).tolist(),uv_max=tex.max(0).tolist(),
                    source_rgb_mean=source.reshape(-1,3)[samples].mean(0).tolist(),
                    r6_rgb_mean=r6.reshape(-1,3)[samples].mean(0).tolist(),
                    evidence_labels=sorted(records.get(root,{})),
                    retain_reason='CONFIRMED_R6_PARENT_PRESERVED' if owner.startswith('CONFIRMED_R6_') else
                    'R7_CONTOUR_SCOPE_OR_INSUFFICIENT_PURE_LEAF_OWNERSHIP'))
            components.append(dict(kind=kind,pixels=len(pixels),bbox=stats[part,:4].tolist(),leaves=leaves,
                                   screen_right_hairline_pixels=int(hairline.ravel()[pixels].sum())))
    output.mkdir(parents=True,exist_ok=False)
    annotated=r7.copy(); annotated[pink]=[0,210,210]; annotated[warm&~pink]=[220,165,0]
    Image.fromarray(annotated).save(output/'residual-locations.png')
    errors=json.loads((contour_folder/'contour-error.json').read_text())
    native_background=review_folder/'colors-5/front-4096-combined-raw.png'
    canvas=Image.open(native_background).convert('RGB') if native_background.is_file() else \
        Image.fromarray(r7).resize((4096,4096),Image.Resampling.NEAREST)
    draw=ImageDraw.Draw(canvas)
    for segment in errors['segments']:
        color=(220,70,55) if not segment['met_target'] else (220,145,20) if segment['kind']!='TRUE_CONTOUR_CLIP' else (20,160,110)
        points=[tuple(p) for p in segment['screen_4096']]
        draw.line(points,fill=color,width=2)
    canvas.save(output/'all-contour-status-4096.png')
    from portrait_r5_visual_review import remap,child,pixel_groups
    locks=json.loads((run/'baseline/shape-locks.json').read_text())
    roots,children={},[]
    for lock in locks['locks']:
        if lock['label'] not in ('lb','rb'):
            continue
        value={'lb':1,'rb':2}[lock['label']]
        for entry in lock['locked_leaves']:
            key=LeafKey(*entry)
            if key.depth:
                children.append(child(key,[value/255]*3))
            else:
                roots[key.source_face_id]=[value/255]*3
    old_brow_labels=remap(np.zeros_like(r7),ids,bary,roots,children,pixel_groups(ids),manifest['face_count'])[:,:,0]
    brow_delta=[]
    for label,mask in new_brows.items():
        previous=old_brow_labels==({'lb':1,'rb':2}[label])
        y,x=np.nonzero(previous|mask)
        bbox=[int(x.min()),int(y.min()),int(x.max()+1),int(y.max()+1)] if len(x) else None
        brow_delta.append(dict(label=label,screen_side='right' if len(x) and x.mean()>ids.shape[1]/2 else 'left',
            r6_pixels=int(previous.sum()),r7_pixels=int(mask.sum()),restored_pixels=int((mask&~previous).sum()),
            trimmed_pixels=int((previous&~mask).sum()),bbox=bbox))
    hairline_components=[dict(index=i,kind=r['kind'],pixels=r['screen_right_hairline_pixels'])
        for i,r in enumerate(components) if r['screen_right_hairline_pixels']]
    hairline_pixels=np.flatnonzero(hairline&(pink|warm))
    hairline_class=Counter()
    for pixel in hairline_pixels:
        root=int(ids.ravel()[pixel])
        labels=records.get(root,{})
        owner='EXPLICIT_CONFLICT' if root in conflicts else 'CONFIRMED_R6_SKIN_LEAF' if old_labels.ravel()[pixel]==1 else \
            'VERIFIED_HAIR' if 'hair' in labels else 'COARSE_SKIN_ONLY' if set(labels)&{'face','nose'} else 'UNRESOLVED'
        hairline_class[owner]+=1
    located=r7.copy(); located[hairline&(pink|warm)]=[0,210,210]
    Image.fromarray(located).save(output/'screen-right-hairline-locations.png')
    report=dict(schema='orca.r7-source-audit/v1',outside_features_pink_pixels=int(pink.sum()),
        partition_sha256=document['partition_sha256'],boundary_policy_sha256=document['boundary_policy_sha256'],
        r6_outside_locks_pink_pixels=int((np.all(r6==lip,axis=2)&(ids>=0)&(old_labels!=3)).sum()),
        face_scope_pink_pixels=int((pink&scope).sum()),outside_features_warm_diagnostic_pixels=int(warm.sum()),
        classification=dict(class_counts),components=components,source_sha256=manifest['source_sha256'],
        unique_diagnostic_pixels=int((pink|warm).sum()),diagnostic_kinds_disjoint=True,
        visible_eye_line_pixels=int(accessory.sum()),actual_eye_line_changed_pixels=int((accessory&np.any(r7!=r6,axis=2)).sum()),
        contour_overlay_background_roi=4096 if native_background.is_file() else 1024,
        brow_delta=brow_delta,screen_right_hairline=dict(pixels=len(hairline_pixels),classification=dict(hairline_class),
            component_refs=hairline_components,locator_face_bbox=[x0,y0,x1,y1],band_radius_pixels_1024=8,
            locator_is_not_authorization=True),
        geometry_id=manifest['geometry_id'],hairline_status='PARTIAL_ATTRIBUTION_KEEP_UNCONFIRMED_R6',
        source_shadows_are_not_automatically_hair=True,all_unresolved_residuals_in_denominator=True,
        screen_right_definition='frontal image right, not anatomical right',visual_status='PARTIAL_IMPROVEMENT',
        note='Warm RGB is a diagnostic locator, not proof of mistaken ownership. Existing confirmed R6 parent colors are retained.',
        material_tree_changed=False,production_enabled=False)
    publish(output/'source-audit.json',report)
    print(json.dumps({k:report[k] for k in ('outside_features_pink_pixels','face_scope_pink_pixels','classification')}),flush=True)


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('stage',choices=('contours','residuals'))
    parser.add_argument('--run',type=Path,required=True)
    parser.add_argument('--partition',type=Path,required=True)
    parser.add_argument('--proposals',type=Path)
    parser.add_argument('--review',type=Path)
    parser.add_argument('--contours',type=Path)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    if args.stage=='contours': contour_audit(args.run.resolve(),args.partition.resolve(),args.proposals.resolve(),args.output.resolve())
    else: residual_audit(args.run.resolve(),args.partition.resolve(),args.review.resolve(),args.contours.resolve(),args.output.resolve())
