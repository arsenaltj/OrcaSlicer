"""Render accepted 3D automatic regions on their actual source-view pixels."""
import argparse
import json
from pathlib import Path
import numpy as np
from PIL import Image
from color_automatic_features import make_features


def main():
    parser=argparse.ArgumentParser();parser.add_argument('folder',type=Path);args=parser.parse_args()
    cards=[];checks=[]
    for folder in sorted(args.folder.glob('sample-*')):
        report=json.loads((folder/'result.json').read_text(encoding='utf-8'))
        views=[v for v in report['views'] if v['camera']['name'].startswith('focus') and v['accepted']]
        if not views:continue
        view=max(views,key=lambda v:max(d['score'] for d in v['detections']))
        name=view['camera']['name']
        with np.load(folder/'regions.npz',allow_pickle=False) as data:
            features,masks=make_features(data['semantic'],data['confidence'],report['label_schema'])
        with np.load(folder/(name+'-observations.npz'),allow_pickle=False) as data:ids=data['ids']
        source=np.asarray(Image.open(folder/(name+'.png')).convert('RGB'));overlay=source.copy()
        valid=ids>=0
        for feature in features:
            pixels=np.zeros(ids.shape,bool);pixels[valid]=masks[feature['name']][ids[valid]]
            color={'brow':[255,170,25],'eye':[0,230,230],'mouth':[255,30,140]}[feature['kind']]
            overlay[pixels]=np.rint(.25*source[pixels]+.75*np.array(color)).astype(np.uint8)
        Image.fromarray(source).save(folder/'gallery-original.png')
        Image.fromarray(overlay).save(folder/'gallery-regions.png')
        count=sum(bool(mask.any()) for mask in masks.values())
        cards.append(f'<article><h2>样本 {len(cards)+1} <small>五官组 {count}/5 · 首次识别 {report["seconds"]:.0f} 秒</small></h2>'
            f'<img src="{folder.name}/gallery-regions.png" data-overlay="{folder.name}/gallery-regions.png" data-original="{folder.name}/gallery-original.png" alt="自动识别的眼睛、眉毛和嘴部范围">'
            '<p>橙色：眉毛 · 青色：眼睛 · 粉色：嘴部。标记来自最终融合的网格区域；未识别处保留原图。</p></article>')
        checks.append({'sample':folder.name,'feature_groups':count,'view':name,'native_binding':'NOT_CLAIMED'})
    html='''<!doctype html><html lang="zh-CN"><meta charset="utf-8"><title>自动五官定位 · 两样本检查</title>
<style>body{margin:0;padding:24px;background:#f3f5f7;color:#263341;font:14px "Microsoft YaHei",sans-serif}h1{font-size:22px}p{line-height:1.7;color:#637080}button,a{display:inline-block;margin:5px 10px 10px 0;padding:9px 13px;border:1px solid #b9cad4;border-radius:7px;background:white;color:#08736e;text-decoration:none;cursor:pointer}main{display:grid;grid-template-columns:repeat(auto-fit,minmax(300px,1fr));gap:18px}article{background:white;border-radius:12px;padding:16px}h2{font-size:17px}small{font-size:12px;color:#65717e;font-weight:normal}img{display:block;width:100%}table{border-collapse:collapse;margin:18px 0;background:white}td,th{padding:10px 15px;border:1px solid #d8dfe5;text-align:left}</style>
<h1>能泛化到哪一步？先检查自动五官范围</h1><p>两个已有模型使用同一套本地检测、解析和多视角投影，无手填五官坐标。这里只验证五官定位；没有把白外套的材质规则套到红衣模型上。</p>
<button id="toggle">隐藏标记，看原贴图</button><a href="../run4-final/">返回第四版配色对照</a>
<main>'''+''.join(cards)+'''</main>
<table><tr><th>计划阶段</th><th>当前状态</th></tr><tr><td>1 · 基线与原因定位</td><td>已有真实基线，完整归因未完成</td></tr><tr><td>2 · B 组区域清理</td><td>效果方向已认可，正在验证自动定位与复用范围</td></tr><tr><td>3 · ImageMap</td><td>尚未独立构建与切片验证</td></tr><tr><td>4 · 实物小样</td><td>尚未执行</td></tr><tr><td>5 · 接入默认流程</td><td>尚未执行</td></tr></table>
<p>两个样本不能证明通用：不同肤色、胡须、眼镜、遮挡、多人及非人像尚未验证。全身材质识别、不同耗材的自动分配也未通过泛化验收。</p>
<script>let shown=true;document.querySelector('#toggle').onclick=()=>{shown=!shown;document.querySelectorAll('img').forEach(i=>i.src=shown?i.dataset.overlay:i.dataset.original);document.querySelector('#toggle').textContent=shown?'隐藏标记，看原贴图':'显示自动识别标记'};</script></html>'''
    (args.folder/'gallery.html').write_text(html,encoding='utf-8')
    (args.folder/'gallery-checks.json').write_text(json.dumps(checks,indent=2),encoding='utf-8')
    print(json.dumps(checks))


if __name__=='__main__':main()
