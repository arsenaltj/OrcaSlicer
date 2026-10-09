"""Geometry-derived R7 drafts and ablations; screen AA never becomes a slot."""
import argparse
from collections import Counter
import html
import json
from pathlib import Path

import cv2
import numpy as np
from PIL import Image

from beauty_leaf_domain import digest
from portrait_r5_baseline import sha
from portrait_r5_visual_review import assignments, pixel_groups, publish, remap, lit, sheet
from portrait_r6_repair import load_render, verify, pixel_provenance
from portrait_r6_review import masks
from surface_partition import load, pixel_cells, color_plan, apply


def colors(run, partition_folder, output):
    manifest=verify(run)
    sidecar=json.loads((partition_folder/'shape-locks.json').read_text())
    identity={k:sidecar[k] for k in ('geometry_id','source_sha256','evidence_sha256','runtime_sha256','policy_sha256',
                                    'baseline_sha256','boundary_policy_sha256','face_count')}
    document,_=load(partition_folder,identity)
    output.mkdir(parents=True,exist_ok=False)
    variants=[]
    for count in (3,4,5,6):
        baseline=json.loads((run/f'baseline/colors-{count}/portrait-color-plan.json').read_text())
        palette=baseline['palette']
        if len(palette)!=count: raise ValueError('Fixed palette drift')
        plan=color_plan(document,palette)
        plan.update(partition_ref=sidecar['partition_ref'],r6_color_plan_sha256=sha(run/f'baseline/colors-{count}/portrait-color-plan.json'),
                    shape_lock_sha256=sha(partition_folder/'shape-locks.json'),
                    unused_slots_scope='changed partition only; retained R6 whole-body slots audited separately')
        folder=output/f'colors-{count}'; folder.mkdir()
        publish(folder/'portrait-color-plan.json',plan)
        publish(folder/'material-cells.json',[dict(id=r['id'],slot_uid=r['slot_uid'],slot=r['slot'],source=r['color_source']) for r in plan['cells']])
        variants.append(dict(color_count=count,plan_sha256=sha(folder/'portrait-color-plan.json'),
                             material_cells_sha256=sha(folder/'material-cells.json')))
    publish(output/'stage-report.json',dict(schema='orca.r7-color-report/v1',variants=variants,
        partition_file_sha256=sidecar['partition_ref']['sha256'],geometry_id=manifest['geometry_id'],
        source_sha256=manifest['source_sha256'],material_tree_changed=False,oral_auto_color=False,
        production_enabled=False,workbench_v3_enabled=False))
    print(json.dumps(dict(color_plans=len(variants),fixed_palettes=True)),flush=True)


def camera_identity(camera,size):
    identity={k:camera[k] for k in ('geometry_id','source_sha256','scope','view','basis','center','half_height','width','height')}
    if digest(identity)!=camera['render_id']:
        raise ValueError('Saved camera drift')
    identity['width']=identity['height']=size
    return dict(identity,render_id=digest(identity),baseline_render_id=camera['render_id'])


def render(data,camera):
    renderer,vertices,faces,uv,colors,materials,material_ids=data
    basis,center=np.asarray(camera['basis']),np.asarray(camera['center'])
    projected=renderer.project(vertices,basis,center,camera['half_height'],camera['width'])
    ids,depth,bary=raster_tiled(renderer,vertices,faces,basis,center,camera['half_height'],camera['width'],
        renderer.double_sided_faces(materials,material_ids))
    source=renderer.shade(faces,uv,colors,materials,material_ids,ids,bary,projected)
    return source,ids,bary,pixel_groups(ids)


def raster_tiled(renderer,vertices,faces,basis,center,half_height,size,sided,tile_limit=2048):
    if size<=tile_limit:
        return renderer.raster(vertices,faces,basis,center,half_height,size,sided)
    if size%tile_limit:
        raise ValueError('Unsupported exact raster tile size')
    ids=np.empty((size,size),dtype=np.int32)
    depth=np.empty((size,size),dtype=np.float64)
    bary=np.empty((size,size,3),dtype=np.float64)
    for y in range(0,size,tile_limit):
        for x in range(0,size,tile_limit):
            dx=((x+tile_limit/2)/size*2-1)*half_height
            dy=(1-(y+tile_limit/2)/size*2)*half_height
            tile_center=center+dx*basis[0]+dy*basis[1]
            values=renderer.raster(vertices,faces,basis,tile_center,half_height*tile_limit/size,tile_limit,sided)
            target=np.s_[y:y+tile_limit,x:x+tile_limit]
            ids[target],depth[target],bary[target]=values
    return ids,depth,bary


def review(run,partition_folder,color_folder,output,preview=False):
    manifest=verify(run)
    sidecar=json.loads((partition_folder/'shape-locks.json').read_text())
    expected={k:sidecar[k] for k in ('geometry_id','source_sha256','evidence_sha256','runtime_sha256','policy_sha256',
                                    'baseline_sha256','boundary_policy_sha256','face_count')}
    document,_=load(partition_folder,expected)
    locks=json.loads((run/'baseline/shape-locks.json').read_text())
    ownership=json.loads((run/'baseline/surface-ownership.json').read_text())
    data=load_render(run)
    # Validate the tile transform against a supported full frame before using
    # it at 4096. Existing native raster limits remain unchanged.
    cameras_for_check={(r['scope'],r['view']):r for r in json.loads((run/'baseline/review-manifest.json').read_text())['images']}
    check=cameras_for_check['face','front']
    renderer,vertices,faces,_,_,materials,material_ids=data
    args=(renderer,vertices,faces,np.asarray(check['basis']),np.asarray(check['center']),check['half_height'],1024,
          renderer.double_sided_faces(materials,material_ids))
    whole=raster_tiled(*args)
    tiled=raster_tiled(*args,tile_limit=512)
    if not np.array_equal(whole[0]>=0,tiled[0]>=0): raise ValueError('Tiled visibility differs')
    visible=whole[0]>=0
    p=np.einsum('ij,ijk->ik',whole[2][visible],vertices[faces[whole[0][visible]]])
    q=np.einsum('ij,ijk->ik',tiled[2][visible],vertices[faces[tiled[0][visible]]])
    tile_error=float(np.max(np.linalg.norm(p-q,axis=1)))
    if tile_error>1e-7: raise ValueError('Tiled source mapping differs')
    tile_check=dict(pixel_id_ties=int(np.count_nonzero(whole[0]!=tiled[0])),max_world_error=tile_error,
                    max_native_tile=2048,original_renderer_unchanged=True)
    del whole,tiled,p,q
    triangles=data[1][data[2]]
    normals=np.cross(triangles[:,1]-triangles[:,0],triangles[:,2]-triangles[:,0]).astype(float)
    normals/=np.maximum(np.linalg.norm(normals,axis=1)[:,None],1e-15)
    del triangles
    cameras={(r['scope'],r['view']):r for r in json.loads((run/'baseline/review-manifest.json').read_text())['images']}
    selected=list(cameras.values()) if not preview else [cameras['face','front']]
    variants={}
    for count in (3,4,5,6):
        old=run/f'baseline/colors-{count}'
        plan=json.loads((color_folder/f'colors-{count}/portrait-color-plan.json').read_text())
        if plan['palette']!=json.loads((old/'portrait-color-plan.json').read_text())['palette']:
            raise ValueError('R6 palette changed')
        variants[count]=(plan,assignments(old/'face-colors.bin',manifest['face_count']),
                         json.loads((old/'subface-colors.json').read_text()))
    output.mkdir(parents=True,exist_ok=False)
    metrics,images=[],[]
    render_jobs=[(camera,1024) for camera in selected]
    if not preview:
        render_jobs.extend((cameras['face','front'],size) for size in (2048,4096))
    for camera,size in render_jobs:
        camera=camera_identity(camera,size)
        scope,view=camera['scope'],camera['view']
        source,ids,bary,groups=render(data,camera)
        mapping,lookup=pixel_cells(document,ids,bary,groups)
        old_labels=masks(ownership,locks,ids,bary,groups)
        valid=ids>=0
        target=mapping>=0
        enabled=np.zeros(len(lookup),dtype=bool)
        planned={r['id']:r['slot_uid'] is not None for r in variants[3][0]['cells']}
        for i,cell in enumerate(lookup): enabled[i]=planned[cell['id']]
        writable=target.copy(); writable[target]=enabled[mapping[target]]
        combined_front=[]
        oral_cells=np.array([c['label']=='imouth' for c in lookup])
        oral_mask=target.copy(); oral_mask[target]=oral_cells[mapping[target]]
        accessory_cells=np.array([c['label'].startswith('periocular-') for c in lookup])
        accessory_mask=target.copy(); accessory_mask[target]=accessory_cells[mapping[target]]
        for count,(plan,roots,children) in variants.items():
            old=remap(source,ids,bary,roots,children,groups,manifest['face_count'])
            modes={mode:apply(old,mapping,lookup,plan,old_labels,mode) for mode in ('details-only','clipping-only','combined')}
            difference=np.max(np.abs(modes['combined'].astype(int)-old.astype(int)),axis=2)
            if np.any((difference>0)&~writable):
                raise ValueError('R7 changed a non-target or locally retained pixel')
            if np.any((difference>0)&oral_mask): raise ValueError('Oral passthrough was recolored')
            folder=output/f'colors-{count}'; folder.mkdir(exist_ok=True)
            versions=dict(source=source,R6=old,**modes)
            for name,image in versions.items():
                if scope=='face':
                    path=folder/f'{view}-{size}-{name}-raw.png'
                    Image.fromarray(image).save(path)
                    images.append(dict(path=str(path.relative_to(output)),sha256=sha(path),color_count=count,
                        scope=scope,view=view,size=size,mode=name,lighting='flat-raw',camera=camera))
            for lighting in ('flat','lit'):
                rgb=list(versions.values())
                if lighting=='lit': rgb=[lit(v,ids,normals,np.asarray(camera['basis'])) for v in rgb]
                path=folder/f'{scope}-{view}-{size}-{lighting}.png'
                sheet(rgb,['Source','R6','Details only','Clipping only','R7'],path,512 if scope=='face' else 384)
                images.append(dict(path=str(path.relative_to(output)),sha256=sha(path),color_count=count,
                    scope=scope,view=view,size=size,mode='comparison',lighting=lighting,camera=camera))
            palette={s['uid']:np.rint(np.asarray(s['rgb'])*255).astype(np.uint8) for s in plan['palette']}
            pink=np.all(modes['combined']==palette.get('portrait-lips',[-1,-1,-1]),axis=2)&valid
            lip_cells=np.array([c['label'] in ('ulip','llip') for c in lookup])
            lip_mask=target.copy(); lip_mask[target]=lip_cells[mapping[target]]
            metric=dict(color_count=count,scope=scope,view=view,size=size,render_id=camera['render_id'],
                        changed_visible_pixels=int((difference>0).sum()),non_target_changed_pixels=0,
                        outside_lips_pink_pixels=int((pink&~lip_mask).sum()),
                        visible_eye_line_pixels=int(accessory_mask.sum()),
                        restored_eye_line_pixels=int(((difference>0)&accessory_mask).sum()),oral_changed_pixels=0)
            metrics.append(metric)
            if scope=='face' and view=='front':
                combined_front.append(modes['combined'])
            if size==4096:
                # Supersampled display is separate from unblended identity and
                # slot samples. No AA RGB is written to a decision file.
                for name,image in versions.items():
                    aa=Image.fromarray(image).resize((1024,1024),Image.Resampling.LANCZOS)
                    path=folder/f'{view}-1024-{name}-aa.png'; aa.save(path)
                    images.append(dict(path=str(path.relative_to(output)),sha256=sha(path),color_count=count,
                        scope=scope,view=view,size=1024,mode=name,lighting='flat-aa',camera=camera))
            if scope=='face' and view=='front' and size==1024 and count==5:
                provenance,depths,paths=pixel_provenance(ids,bary,roots,children,groups)
                audit=dict(source=source,r6=old,r7=modes['combined'],ids=ids,bary=bary,provenance=provenance,
                           leaf_depth=depths,leaf_path=paths,cell=mapping,r6_labels=old_labels)
                np.savez_compressed(output/'front-audit.npz',**audit)
        if combined_front:
            path=output/f'front-{size}-3-6-colors.png'
            sheet(combined_front,['3 colors','4 colors','5 colors','6 colors'],path,512)
        print(json.dumps(dict(rendered=f'{scope}-{view}',roi=size)),flush=True)
    publish(output/'review-manifest.json',dict(schema='orca.r7-visual-review/v1',images=images,metrics=metrics,
        partition_ref=sidecar['partition_ref'],source_sha256=manifest['source_sha256'],geometry_id=manifest['geometry_id'],
        visual_status='PARTIAL_IMPROVEMENT',software_lighting_verified=False,production_enabled=False,
        workbench_v3_enabled=False,aa_writes_material=False,iris_is_independent_pupil=False,tile_check=tile_check))
    page=['<!doctype html><meta charset="utf-8"><title>R7 Fangfei review</title>',
          '<style>body{font:15px system-ui;margin:20px;color:#222;background:#f3f5f6}img{max-width:100%;height:auto}a{color:#176748}h2{font-size:20px}.palette{display:flex;gap:12px}i{display:inline-block;width:22px;height:22px;border:1px solid #555}</style>',
          '<h1>R7 Fangfei: contours and restored detail</h1><p>Source | R6 | Details only | Clipping only | Combined. Partial improvement, visual acceptance pending. Offline lit is not software lighting.</p>',
          '<p>Iris is the existing dark ocular region; no separate pupil recognition. Raw 1024/2048/4096 renders preserve unblended ownership. AA is display only.</p>',
          '<h2>3 / 4 / 5 / 6 colors</h2><a href="front-1024-3-6-colors.png"><img src="front-1024-3-6-colors.png"></a>']
    for count,(plan,_,_) in variants.items():
        page.append(f'<h2>{count} colors</h2><div class="palette">')
        for slot in plan['palette']:
            rgb=np.rint(np.asarray(slot['rgb'])*255).astype(int)
            page.append(f'<span><i style="background:rgb({rgb[0]},{rgb[1]},{rgb[2]})"></i> {html.escape(slot["uid"])}</span>')
        page.append('</div>')
        for camera in selected:
            scope,view=camera['scope'],camera['view']
            page.append(f'<h3>{scope} / {view}</h3>')
            for lighting in ('flat','lit'):
                relative=f'colors-{count}/{scope}-{view}-1024-{lighting}.png'
                page.append(f'<a href="{relative}"><img src="{relative}" loading="lazy"></a>')
        page.append('<p>Native face ROI: ')
        for size in ((1024,) if preview else (1024,2048,4096)):
            for mode in ('source','R6','details-only','clipping-only','combined'):
                path=f'colors-{count}/front-{size}-{mode}-raw.png'
                page.append(f'<a href="{path}">{size} {mode}</a> | ')
        if not preview:
            path=f'colors-{count}/front-1024-combined-aa.png'
            page.append(f'<a href="{path}">Display AA</a>')
        page.append('</p>')
    with (output/'index.html').open('x',encoding='utf-8') as stream: stream.write('\n'.join(page))


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('stage',choices=('colors','review'))
    parser.add_argument('--run',type=Path,required=True)
    parser.add_argument('--partition',type=Path,required=True)
    parser.add_argument('--colors',type=Path)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--preview',action='store_true')
    args=parser.parse_args()
    if args.stage=='colors': colors(args.run.resolve(),args.partition.resolve(),args.output.resolve())
    else: review(args.run.resolve(),args.partition.resolve(),args.colors.resolve(),args.output.resolve(),args.preview)
