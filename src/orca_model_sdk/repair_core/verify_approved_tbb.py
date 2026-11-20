"""Validate the explicit existing-Slicing TBB input; never discover or replace libraries."""
from pathlib import Path
import argparse,hashlib,json,os

def validate(library,include,manifest):
 library=Path(library);include=Path(include);manifest=Path(manifest)
 if not library.is_absolute() or not include.is_absolute():raise ValueError('Explicit absolute TBB runtime and header paths required')
 spec=json.loads(manifest.read_text(encoding='utf-8'));approved=spec['runtime']
 if not library.is_file() or library.name!=approved['library']:raise ValueError('Approved TBB runtime file required')
 if library.stat().st_size!=approved['bytes'] or hashlib.sha256(library.read_bytes()).hexdigest()!=approved['sha256']:raise ValueError('Approved TBB runtime SHA/size mismatch')
 paths={p.relative_to(include).as_posix():p for p in include.rglob('*') if p.is_file()}
 if set(paths)!=set(spec['headers']):raise ValueError('Pinned TBB public header inventory mismatch')
 for name,p in paths.items():
  if hashlib.sha256(p.read_bytes()).hexdigest()!=spec['headers'][name]:raise ValueError('Pinned TBB header SHA mismatch: '+name)
 return {'schemaVersion':1,'runtimePath':str(library.resolve()),'headerRoot':str(include.resolve()),'approvalManifestSha256':hashlib.sha256(manifest.read_bytes()).hexdigest(),'runtime':approved,'upstream':spec['upstream'],'headerCount':len(paths),'headersSha256':hashlib.sha256(json.dumps(spec['headers'],sort_keys=True).encode()).hexdigest(),'runtimeBuiltByThisProducer':False}

if __name__=='__main__':
 parser=argparse.ArgumentParser();parser.add_argument('--library',required=True);parser.add_argument('--include',required=True);parser.add_argument('--manifest',required=True);parser.add_argument('--output',required=True);args=parser.parse_args()
 try:
  result=validate(args.library,args.include,args.manifest);target=Path(args.output);data=json.dumps(result,indent=2)+'\n'
  target.parent.mkdir(parents=True,exist_ok=True)
  if target.exists() and target.read_text(encoding='utf-8')!=data:raise ValueError('Conflicting TBB binding output')
  if not target.exists():
   temporary=target.with_suffix(target.suffix+'.next')
   with temporary.open('x',encoding='utf-8') as f:f.write(data);f.flush();os.fsync(f.fileno())
   os.replace(temporary,target)
  print(json.dumps({'runtimeSha256':result['runtime']['sha256'],'headerCount':result['headerCount'],'sourceOwner':'existing-slicing-runtime'}))
 except (ValueError,OSError,KeyError) as error:raise SystemExit(str(error))
