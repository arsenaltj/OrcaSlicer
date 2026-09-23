"""Offline cross-sample automatic region trial using the existing local worker.

No manual camera, head-height or feature-coordinate priors. This validates GLB
surface recognition only; the generated packet is NOT native importer evidence.
"""
import argparse
import hashlib
import json
from pathlib import Path
import sys
import time

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools'/'ai'))
import local_semantic_geometry as geometry
import local_semantic_render as render
from local_semantic_pipeline import analyze
from local_semantic_projection import LABEL_NAMES
from local_semantic_worker import load_config, load_models, restrict_network


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--config',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('sources',type=Path,nargs='+')
    args=parser.parse_args()
    config=load_config(args.config)
    if Path(config['python_executable']).resolve()!=Path(sys.executable).resolve():
        raise ValueError('Run with the configured semantic Python')
    args.output.mkdir(parents=True,exist_ok=False)
    restrict_network()
    print('Loading verified local CPU models',flush=True)
    models=load_models(config)
    print('Models ready; processing without manual feature windows',flush=True)
    summaries=[]
    for sample_number,source in enumerate(args.sources,1):
        folder=args.output/f'sample-{sample_number}';folder.mkdir()
        started=time.monotonic();digest=sha(source)
        vertices,faces,*_=render.load(source)
        packet=folder/'glb-self-check.bin'
        geometry.write_new(packet,vertices,faces,digest)
        def on_view(report,rgb,ids,depths,bary,labels,confidence):
            name=report['camera']['name']
            Image.fromarray(rgb).save(folder/(name+'.png'))
            np.savez_compressed(folder/(name+'-observations.npz'),ids=ids,labels=labels,confidence=confidence)
            print(json.dumps({'sample':sample_number,'view':name,'detected':report['detected'],
                              'accepted':report['accepted'],'seconds':round(time.monotonic()-started,1)}),flush=True)
        result=analyze(source,packet,digest,lambda:models,on_view=on_view)
        semantic=np.full(len(faces),-1,np.int8);scores=np.zeros(len(faces),np.float32)
        for region in result['projection']['regions']:
            samples=np.asarray(region['samples']);ids=samples[:,0].astype(np.int64)
            if np.any(semantic[ids]>=0):raise ValueError('Conflicting automatic regions')
            semantic[ids]=LABEL_NAMES.index(region['label']);scores[ids]=samples[:,1]
        np.savez_compressed(folder/'regions.npz',semantic=semantic,confidence=scores)
        tri=vertices[faces];areas=np.linalg.norm(np.cross(tri[:,1]-tri[:,0],tri[:,2]-tri[:,0]),axis=1)/2
        report={k:v for k,v in result.items() if k not in ('vertices','faces','projection')}
        report.update(source_path=str(source.resolve()),regions_sha256=sha(folder/'regions.npz'),statistics=result['projection']['statistics'],
            label_schema=list(LABEL_NAMES),labels={label:{'faces':int(np.count_nonzero(semantic==i)),
            'area_mm2':float(areas[semantic==i].sum())} for i,label in enumerate(LABEL_NAMES) if np.any(semantic==i)},
            seconds=time.monotonic()-started,manual_feature_windows=False,
            limits=['GLB recognition only: packet was generated from GLB, not native importer.',
                    'Unknown and unsupported regions remain unknown; no whole-body segmentation.',
                    'Two samples cannot establish broad generalization or calibrated confidence.'])
        assert sha(source)==digest
        (folder/'result.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
        summaries.append({'folder':folder.name,'source_sha256':digest,'labels':report['labels'],'seconds':report['seconds']})
        print('Completed sample',sample_number,json.dumps(summaries[-1]),flush=True)
    (args.output/'result.json').write_text(json.dumps({'samples':summaries,'scope':'automatic face regions, not general color distillation',
        'implementation_sha256':{p.name:sha(p) for p in [Path(__file__),ROOT/'tools/ai/local_semantic_pipeline.py',
            ROOT/'tools/ai/local_semantic_projection.py',ROOT/'tools/ai/local_semantic_render.py',ROOT/'tools/ai/local_semantic_views.py']}},indent=2),encoding='utf-8')


if __name__=='__main__':main()
