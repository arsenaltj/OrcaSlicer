"""Run one identical region/palette policy on cached automatic sample inputs."""
import argparse
import hashlib
import json
from pathlib import Path
import time
import xml.etree.ElementTree as ET
import zipfile
import numpy as np
from PIL import Image
from color_material_distill import PALETTE, BASELINE_HASH, load, shade, validate_alignment, export_painted_project
from color_automatic_features import load_features
from color_feature_refine import smooth_surface_signal, detail_colors
from color_region_materials import oklab, nearest_palette, material_palette, surface_edges, components, distill
from local_semantic_geometry import geometry_fingerprint


def sha(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def island_stats(labels, areas, edges, lock):
    roots=components(labels,*edges[:2]);n=int(roots.max())+1
    area=np.bincount(roots,weights=areas,minlength=n)
    protected=np.zeros(n,bool);protected[roots[lock]]=True
    small=(area>0)&(area<.8)&~protected
    return {'unprotected_islands_below_0_8_mm2':int(small.sum()),'area_mm2':float(area[small].sum())}


def run(folder, output, reference=None, baseline=None):
    started=time.monotonic()
    recognition=json.loads((folder/'result.json').read_text(encoding='utf-8'))
    source=Path(recognition['source_path']);digest=sha(source)
    vertices,faces,uv,colors,materials,mi=load(source)
    original_geometry=geometry_fingerprint(vertices,faces)
    tri=vertices[faces];center=tri.mean(axis=1)
    cross=np.cross(tri[:,1]-tri[:,0],tri[:,2]-tri[:,0]);norm=np.linalg.norm(cross,axis=1)
    normals=cross/np.maximum(norm[:,None],1e-20);areas=norm/2
    edges=surface_edges(vertices,faces)
    features,masks,binding=load_features(folder,digest,original_geometry,len(faces),center,edges[:2])
    lock=np.logical_or.reduce(list(masks.values()))
    with np.load(folder/'regions.npz',allow_pickle=False) as data:
        semantic,confidence=data['semantic'],data['confidence']
    rgb=shade(faces,uv,colors,materials,mi,np.arange(len(faces)),np.full((len(faces),3),1/3))
    palette=np.array([[int(c[i:i+2],16) for i in (1,3,5)] for c in PALETTE])
    direct=nearest_palette(oklab(rgb),palette)
    distilled,stats=distill(rgb,areas,edges,semantic,confidence,recognition['label_schema'],lock)
    if 'skin' not in stats['prototypes']:
        raise ValueError('No trusted skin anchor: preserve prior result, do not invent a skin color')
    labels=material_palette(distilled,palette)
    before_details=labels.copy()
    # Local detail colors also use the actual six-color palette; no assumed
    # filament index for skin, clothes, hair or lips.
    filtered=smooth_surface_signal(rgb,center,normals,areas,*edges[:2],lock)
    palette_lab=oklab(palette);schema=recognition['label_schema']
    skin=int(material_palette(np.asarray(stats['prototypes']['skin']),palette))
    def observed_role(names,fallback):
        use=(confidence>=.9)&np.isin(semantic,[schema.index(n) for n in names])
        return int(material_palette(np.median(oklab(rgb[use]),axis=0),palette)) if use.any() else fallback
    role_map=np.array([skin,int(palette_lab[:,0].argmin()),int(palette_lab[:,0].argmax()),
                      observed_role(('llip','ulip'),skin),skin,observed_role(('rb','lb'),int(palette_lab[:,0].argmin()))],np.uint8)
    for feature in features:
        mask=masks[feature['name']]
        labels[mask]=role_map[detail_colors(feature['kind'],filtered[mask])]
    assert np.array_equal(labels[~lock],before_details[~lock])
    stats['detail_role_filaments']=role_map.tolist()
    output.mkdir(parents=True,exist_ok=False)
    previous=direct;reference_evidence=None;export_evidence=None
    if reference:
        prior=json.loads((reference/'result.json').read_text(encoding='utf-8'))
        if prior['glb_sha256']==digest:
            if prior['automatic']['render_geometry_id']!=original_geometry:
                raise ValueError('Previous geometry differs')
            with np.load(reference/'labels.npz',allow_pickle=False) as data:previous=data['cleaned']
            if previous.shape!=labels.shape or previous.max()>=len(palette):
                raise ValueError('Invalid previous labels')
            # The previously displayed "original main colors" mode used this
            # palette remapping. Preserve it explicitly for an honest comparison.
            previous=np.array([3,1,0,3,4,5],np.uint8)[previous]
            reference_evidence={'report_sha256':sha(reference/'result.json'),
                                'labels_sha256':sha(reference/'labels.npz'),'palette_mode':'original-main-colors'}
            if baseline:
                if sha(baseline)!=BASELINE_HASH:raise ValueError('Baseline project changed')
                with zipfile.ZipFile(baseline) as archive:
                    meshes=[(name,m) for name in archive.namelist() if name.endswith('.model')
                            for m in ET.fromstring(archive.read(name)).findall('.//{*}mesh')]
                    if len(meshes)!=1:raise ValueError('Expected one native source mesh')
                    name,mesh=meshes[0]
                    nv=np.array([[float(v.get(k)) for k in ('x','y','z')] for v in mesh.find('{*}vertices')],np.float32)
                    nf=np.array([[int(t.get(k)) for k in ('v1','v2','v3')] for t in mesh.find('{*}triangles')],np.int32)
                    if json.loads(archive.read('Metadata/project_settings.config'))['filament_colour']!=PALETTE:
                        raise ValueError('Project palette differs')
                    error=validate_alignment(vertices,faces,nv,nf)
                    export_painted_project(archive,name,labels,output/'cleaned.3mf')
                    export_evidence={'native_corner_error_mm':error,'project_sha256':sha(output/'cleaned.3mf'),
                                     'paint_geometry_and_other_members_roundtrip':'PASS','native_reopen':'NOT_RUN'}
                assert sha(baseline)==BASELINE_HASH
    np.savez_compressed(output/'labels.npz',direct=direct,previous=previous,cleaned=labels,feature_lock=lock)
    # Keep source continuous color estimates as research data, not a baked UV
    # texture nor an ImageMap-ready C+ input.
    np.savez_compressed(output/'region-color.npz',oklab=distilled)
    vertex_normals=np.zeros_like(vertices)
    for c in range(3):np.add.at(vertex_normals,faces[:,c],cross)
    vertex_normals/=np.maximum(np.linalg.norm(vertex_normals,axis=1,keepdims=True),1e-20)
    buffer=np.column_stack((tri.reshape(-1,3),vertex_normals[faces].reshape(-1,3),
        np.repeat(direct,3),np.repeat(previous,3),np.repeat(labels,3),np.repeat(rgb/255,3,axis=0))).astype('<f4')
    (output/'mesh.bin').write_bytes(buffer.tobytes())
    best=max((v for v in recognition['views'] if v['accepted'] and v['camera']['name'].startswith('focus')),
             key=lambda v:max(d['score'] for d in v['detections']))
    camera=best['camera'];direction=np.asarray(camera['basis'])[2]
    source_view=next(v for v in recognition['views'] if v['camera']['name']==best['view_family'])
    for kind,view in [('face',best),('full',source_view)]:
        name=view['camera']['name']
        with np.load(folder/(name+'-observations.npz'),allow_pickle=False) as data:ids=data['ids']
        valid=ids>=0
        base=np.asarray(Image.open(folder/(name+'.png')).convert('RGB'))
        for label,values in [('direct',direct),('cleaned',labels),('previous',previous)]:
            img=base.copy();img[valid]=palette[values[ids[valid]]]
            Image.fromarray(img).save(output/f'{kind}-{label}.png')
        Image.fromarray(base).save(output/f'{kind}-source.png')
    bounds=[vertices.min(0).tolist(),vertices.max(0).tolist()]
    stats.update(before=island_stats(direct,areas,edges,lock),after=island_stats(labels,areas,edges,lock),
        changed_area_mm2=float(areas[labels!=direct].sum()),surface_area_mm2=float(areas.sum()),
        material_area_mm2={str(i):float(areas[labels==i].sum()) for i in np.unique(labels)})
    report={'experiment':'cross-sample-color-regions-v1','source_sha256':digest,'source_path':str(source),
        'geometry_sha256':original_geometry,'automatic_binding':binding,'palette':PALETTE,'faces':len(faces),
        'previous_reference':reference_evidence,'native_export':export_evidence,
        'vertex_stride':12,'bounds':bounds,'statistics':stats,'seconds':time.monotonic()-started,
        'view':{'front_yaw':float(np.arctan2(direction[0],-direction[1])),
                'face_center':camera['center'],'face_extent':camera['half_height'],
                'full_center':np.mean(bounds,axis=0).tolist(),'full_extent':float(np.linalg.norm(np.ptp(vertices,axis=0))*.53)},
        'checks':{'geometry_unchanged':geometry_fingerprint(vertices,faces)==original_geometry,
                  'source_unchanged':sha(source)==digest,'palette_members_only':bool(labels.max()<len(palette)),
                  'detail_pass_does_not_change_body':True,'native_reopen':'NOT_RUN','slice':'NOT_RUN','physical':'NOT_RUN'},
        'parameters':{'clusters':12,'clustering_lightness_weight':.45,'merge_chroma_distance':.065,
                      'saturated_palette_lightness_weight':.25,'saturated_chroma_threshold':.06,'neutral_chroma_threshold':.045,
                      'merge_lightness_distance':.30,'boundary_vote':.70,'area_ratio':8},
        'limits':['Heuristic color regions, not semantic whole-body materials or intrinsic reflectance.',
                  'Direct nearest-Oklab comparison is not the native 12-cluster baseline.',
                  'No manual body coordinates, per-sample tuning or assumed filament-role index.',
                  'Shared face color thresholds remain heuristics and have not been validated across skin tones.',
                  'Two samples and RGB swatches do not establish generalization or physical color accuracy.',
                  'Face-centroid sampling and exact-position seam graph; coincident shells unvalidated.'],
        'implementation_sha256':{p:sha(Path(__file__).with_name(p)) for p in
            ['color_cross_sample.py','color_region_materials.py','color_cross_sample.html','color_automatic_features.py','color_feature_refine.py']}}
    (output/'result.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
    (output/'index.html').write_text(Path(__file__).with_name('color_cross_sample.html').read_text(encoding='utf-8'),encoding='utf-8')
    print(json.dumps({'sample':output.name,'seconds':report['seconds'],'stats':stats},ensure_ascii=False),flush=True)
    return report


def main():
    parser=argparse.ArgumentParser();parser.add_argument('recognition',type=Path);parser.add_argument('output',type=Path)
    parser.add_argument('--reference',type=Path);parser.add_argument('--baseline',type=Path)
    args=parser.parse_args();args.output.mkdir(parents=True,exist_ok=False)
    results=[]
    for folder in sorted(args.recognition.glob('sample-*')):results.append(run(folder,args.output/folder.name,args.reference,args.baseline))
    cards=''.join(f'<article><h2>样本 {i+1}</h2><a href="sample-{i+1}/">打开可旋转对照</a><div class="pair"><figure><img src="sample-{i+1}/full-direct.png"><figcaption>逐面直接匹配六耗材</figcaption></figure><figure><img src="sample-{i+1}/full-cleaned.png"><figcaption>同参数区域合色</figcaption></figure></div><p>小色岛 {r["statistics"]["before"]["unprotected_islands_below_0_8_mm2"]} → {r["statistics"]["after"]["unprotected_islands_below_0_8_mm2"]}；区域处理 {r["seconds"]:.1f} 秒（复用五官识别缓存）。</p></article>' for i,r in enumerate(results))
    (args.output/'index.html').write_text('<!doctype html><html lang="zh-CN"><meta charset="utf-8"><title>全身区域合色 · 两样本验证</title><style>body{max-width:1300px;margin:auto;padding:24px;font:15px Microsoft YaHei;background:#f2f5f7;color:#233}p{line-height:1.8}h1{font-size:24px}article{background:white;padding:18px;margin:20px 0;border-radius:12px}.pair{display:flex}figure{margin:10px;width:50%}img{width:100%}a{color:#007e76}</style><h1>全身区域合色：两模型使用同一套参数</h1><p>去掉固定身体坐标；根据源贴图、表面邻接与自动五官范围合色。六耗材保持一致。左侧是本次直接匹配对照，不是 Orca 的原始默认结果。</p><p>这是一轮泛化候选实验，不替换上一版。小色岛减少不等于外观或打印通过；重点查看衣物边界、发色和五官。</p>'+cards+'<p><a href="../run4-final/">查看上一版固定样本效果</a> · 当前计划第 2 步；ImageMap 独立验证正在准备，实物验证尚未执行。</p></html>',encoding='utf-8')
    (args.output/'result.json').write_text(json.dumps({'samples':[{'source':r['source_sha256'],'seconds':r['seconds'],'statistics':r['statistics']} for r in results], 'same_parameters':all(r['parameters']==results[0]['parameters'] for r in results)},indent=2),encoding='utf-8')


if __name__=='__main__':main()
