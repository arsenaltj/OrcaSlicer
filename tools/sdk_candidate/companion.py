"""Append-only first-observation companions; never compiler-read evidence.

Only collector GO permits real observation. Import and validation are inert.
"""
from pathlib import Path
import datetime
import json
import xml.etree.ElementTree as ET
import time
import importlib.util
import adapter as a

SCHEMA='b5f-producer-companion-v1'
TIMING='FIRST_OBSERVED_NOW_NOT_HISTORICAL_COMPILE_TIME_HASH'
ROLES={'cache','runner','import-lib','runtime','approved-binding'}


def index(item):
    refs=item['fullIndex'].get('files',item['fullIndex'].get('refs',[]))
    return refs+list(item.get('originalRecords',{}).values())


def origin(item,stage):
    return dict(source=a.SOURCE,tree=a.TREE,stage=stage,sourceSeal=item['seal'],
        fullIndex=item['fullIndex'],originalRecords=item.get('originalRecords',{}),
        rawSource=item['rawSource'],terminal=item['terminal'])


def basis(entry,producer,stage,source_graph):
    """Check actual provenance artifacts, not a new 'passed' flag."""
    role=entry['role'];a.require(role in ROLES,'unapproved companion role')
    a.require(entry['stage']==stage,'companion stage differs')
    path=Path(entry['current']['path']).resolve()
    build=Path(producer['rawSource']['buildPath']).resolve().parent
    if stage.endswith('windows'):build=build.parent
    a.require(Path(entry['producerBuildRoot']).resolve()==build,'wrong producer build root')
    a.require(entry['current']!=producer['rawSource']['raw'] and path!=Path(producer['rawSource']['buildPath']).resolve(),
              'raw SDK cannot use companion path')
    a.require(path.suffix.lower() not in ('.o','.obj','.pch','.cpp','.h','.hpp','.c'), 'source/object bypass forbidden')
    evidence=entry['basis']
    pins=index(producer)
    def historical(ref):
        a.require(ref in pins or contains(source_graph,ref),'basis is not original producer provenance')
        return a.verify(ref)
    normal=a.small_json(historical(evidence['normalPlan']))
    plan=normal['stages'][stage] if isinstance(normal.get('stages'),dict) else next(x for x in normal['stages'] if x['stage']==stage)
    configure=plan['configureArgv'];build_argv=plan['buildArgv']
    a.require(a.SOURCE in json.dumps(configure) and str(build).replace('\\','/').lower() in
              json.dumps(configure).replace('\\\\','/').replace('\\','/').lower(),'configure source/build origin missing')
    if role=='cache':
        a.require(path==build/'CMakeCache.txt','wrong cache companion')
        values={line.split(':',1)[0]:line.split('=',1)[1] for line in path.read_text(encoding='utf8').splitlines()
                if not line.startswith(('#','//')) and ':' in line and '=' in line}
        expected={}
        for arg in configure:
            if arg.startswith('-D') and '=' in arg:
                k,v=arg[2:].split('=',1);expected[k.split(':')[0]]=v
        a.require(expected and values.get('ORCA_MODEL_SOURCE_COMMIT')==a.SOURCE,'cache source not frozen')
        a.require(all(values.get(k)==v for k,v in expected.items()),'cache differs from frozen configure/options/toolchain/deps')
        a.require(Path(values['CMAKE_CACHEFILE_DIR']).resolve()==build,'cache build dir changed')
    elif role in ('runner','import-lib'):
        a.require(stage.endswith('windows'),'link companion wrong platform')
        target=evidence['target']
        a.require('--target' in build_argv and target in build_argv[build_argv.index('--target')+1:], 'missing actual target')
        allowed={'model-windows':{'runner':('orca_model_runner','orca_model_runner.exe'),
                                  'import-lib':('orca_model_sdk','orca_model_sdk_c.lib')},
                 'slicing-windows':{'runner':('orca_mobile_slicing_json_gate','orca_mobile_slicing_json_gate.exe'),
                                    'import-lib':('orca_mobile_slicing_sdk','orca_mobile_slicing_sdk_c.lib')}}
        a.require((target,path.name)==allowed[stage][role] and path.parent==build/'Release','wrong target/output')
        project=historical(evidence['project'])
        a.require(project.name==target+'.vcxproj','project target differs')
        # Link output must be declared explicitly; unresolved macros stay unbound.
        root=ET.fromstring(project.read_bytes())
        output=[x.text for x in root.iter() if x.tag.split('}')[-1] in ('OutputFile','ImportLibrary') and x.text]
        declared=str(path).replace('\\','/').lower()
        a.require(any(x.replace('\\','/').lower()==declared for x in output),'missing exact project link output')
        tlog=historical(evidence['linkTlog'])
        data=tlog.read_bytes()
        text=data.decode('utf-16') if data.startswith((b'\xff\xfe',b'\xfe\xff')) else data.decode('utf8')
        a.require(declared in text.replace('\\','/').lower(),'missing actual link tlog output')
        if role=='runner':
            with path.open('rb') as stream:
                a.require(stream.read(2)==b'MZ','runner is not PE')
                stream.seek(0)
                a.require(any(a.SOURCE.encode() in block for block in iter(lambda:stream.read(1024*1024),b'')),
                          'runner current source identity missing')
        else:
            with path.open('rb') as stream:a.require(stream.read(8)==b'!<arch>\n','import lib structure')
    elif role in ('runtime','approved-binding'):
        a.require(stage.endswith('android'),'Windows runtime companion needs explicit original dependency/link graph; unbound')
        approved=a.small_json(historical(evidence['approvedRule']))
        binding=a.small_json(a.verify(evidence['binding']))
        a.require(contains(source_graph,evidence['approvedRule']) and
                  contains(source_graph,dict(path=binding['runtimePath'],sha256=approved['runtime']['sha256'],bytes=approved['runtime']['bytes'])),
                  'approved runtime not corresponding stage input')
        a.require(Path(binding['headerRoot']).is_dir(),'approved header root missing')
        a.require(evidence['headerProof'] in pins or contains(source_graph,evidence['headerProof']), 'header provenance missing')
        a.verify(evidence['headerProof'])
        verifier=historical(evidence['approvedVerifier'])
        spec=importlib.util.spec_from_file_location('companion_approved_tbb',verifier)
        module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
        actual=module.validate(binding['runtimePath'],binding['headerRoot'],a.verify(evidence['approvedRule']))
        a.require(actual==binding,'binding/runtime/headerRoot differs from original approved verifier')
        if role=='runtime':
            a.require(entry['current']['sha256']==approved['runtime']['sha256'] and
                      entry['current']['bytes']==approved['runtime']['bytes'],'runtime approved bytes differ')
        else:
            a.require(path.name=='approved-tbb-binding.json' and entry['current']==evidence['binding'], 'wrong approved binding')


def contains(value,target):
    if isinstance(value,dict):
        return all(value.get(k)==v for k,v in target.items()) or any(contains(x,target) for x in value.values())
    return isinstance(value,list) and any(contains(x,target) for x in value)


def validate(receipt,producer,stage):
    a.require(receipt['schema']==SCHEMA and receipt['origin']==origin(producer,stage),'wrong companion producer origin')
    a.require(a.small_json(a.verify(producer['seal']))==producer['fullIndex'],'companion original seal/index differs')
    a.require(receipt['evidenceTiming']==TIMING,'companion is not historical compiler hash')
    a.require(receipt['driftBefore']==receipt['driftAfter'] and receipt['driftBefore'], 'missing or changed drift proof')
    expected=index(producer)+[producer['seal'],producer['rawSource']['raw']]
    a.require(all(ref in receipt['driftBefore'] for ref in expected),'incomplete original source/raw/object/record drift coverage')
    graphref=next(x for x in index(producer) if Path(x['path']).name=='actual-compiler-inputs.json')
    graph=a.small_json(a.verify(graphref))
    def pins(value):
        if isinstance(value,dict):
            if all(k in value for k in ('path','bytes','sha256')):yield value
            for v in value.values():yield from pins(v)
        elif isinstance(value,list):
            for v in value:yield from pins(v)
    a.require(all(ref in receipt['driftBefore'] for ref in pins(graph)), 'read/source drift coverage missing')
    objects=next(x for x in index(producer) if Path(x['path']).name=='new-object-inventory.json')
    a.require(all(ref in receipt['driftBefore'] for ref in pins(a.small_json(a.verify(objects)))), 'object drift coverage missing')
    seen=set()
    for entry in receipt['companions']:
        a.require(entry['evidenceTiming']==TIMING and entry['firstObservedUtc'],'missing first observation')
        a.require(entry['current']['path'] not in seen,'duplicate companion')
        seen.add(entry['current']['path'])
        basis(entry,producer,stage,graph)


def accepted(source_proof,producer,stage):
    pair=source_proof.get('companion')
    if pair is None:return []
    seal=a.small_json(a.verify(pair['seal']))
    a.require(seal['schema']=='b5f-companion-seal-v1' and seal['receipt']==pair['receipt'],'companion seal differs')
    receipt=a.small_json(a.verify(pair['receipt']))
    validate(receipt,producer,stage)
    for ref in receipt['driftBefore']:a.verify(ref)
    return [x['current'] for x in receipt['companions']]


def closure(source_proof):
    pair=source_proof.get('companion')
    if pair is None:return []
    receipt=a.small_json(a.verify(pair['receipt']))
    return [pair['seal'],pair['receipt']]+receipt['driftBefore']+[
        ref for x in receipt['companions'] for ref in [x['current']]+list(x['basis'].values()) if isinstance(ref,dict) and 'sha256' in ref]


def collect(plan,go):
    """GO-only hashing of existing outputs. No native CLI or compile route."""
    a.require(go['companionCollectionAuthorized'] is True and go['source']==a.SOURCE,'collector GO required')
    a.require(a.identity(go['planPath'])['sha256']==go['planSha256'],'collector plan mismatch')
    a.require(plan==a.small_json(go['planPath']),'collector exact plan required')
    a.require(plan['stage'] in a.STAGES and set(plan['producers'])==set(a.STAGES),'four producers required')
    for stage,item in plan['producers'].items():
        a.require(item is not None,'unbound producer')
        a.terminal_ok(stage,item['terminal'],item['rawSource'])
        a.require(a.small_json(a.verify(item['seal']))==item['fullIndex'],'collector original seal differs')
    a.require(go['approvedOrigins']=={stage:origin(item,stage) for stage,item in plan['producers'].items()},
              'root has not approved exact four successful origin graphs')
    expected=plan['toolFiles']+plan['inputs']
    local={str(Path(__file__).resolve()),str(Path(a.__file__).resolve()),
           str(Path(__file__).with_name('contracts.py').resolve()),str(Path(__file__).with_name('artifact_owner.py').resolve()),
           str(Path(__file__).with_name('api_context.py').resolve())}
    a.require(local <= {str(Path(r['path']).resolve()) for r in plan['toolFiles']},'collector transitive tool closure missing')
    closure={r['path']:r for r in go['closure']}
    a.require(closure=={r['path']:r for r in expected},'collector complete closure differs')
    for ref in plan['toolFiles']:a.verify(ref)
    # No fallback monitor. Use the same qualified artifact preflight protocol.
    import contracts
    contracts.owner_protocol(plan,a)
    import subprocess
    toolroot=Path(__file__).resolve().parents[2]
    a.require(subprocess.check_output(['git','-C',str(toolroot),'rev-parse','HEAD'],text=True).strip()==plan['toolSource'],
              'collector actual tool commit differs')
    a.require(not subprocess.check_output(['git','-C',str(toolroot),'status','--porcelain'],text=True).strip(),
              'collector tool worktree dirty')
    import sys
    sys.path.insert(0,plan['ownerDirectory'])
    from build_resources import gate,sample,heavy
    from artifact_owner import admit_cancel
    out=Path(plan['output']);a.require(not out.exists(),'fresh companion output required')
    cancel=Path(go['cancel']);admit_cancel(cancel)
    samples=[]
    gate(samples,lambda:None)
    out.mkdir(parents=True)
    state=dict(state='started',samples=samples)
    def save():
        (out/'execution.json').write_text(json.dumps(state,indent=2),encoding='utf8')
    last=[0.0]
    def monitor(force=False):
        admit_cancel(cancel)
        if not force and time.monotonic()-last[0]<5:return
        last[0]=time.monotonic()
        s=sample();samples.append(s);save()
        a.require(s['freeKiB']>3*1024**2 and not any(heavy(p) for p in s['processes']),'collector runtime gate')
    previous=a.IO_MONITOR;a.IO_MONITOR=monitor
    try:
        def sourcecheck():
            repo=plan['sdkRepository']
            a.require(subprocess.check_output([plan['git'],'-C',repo,'rev-parse','HEAD'],text=True).strip()==a.SOURCE,'collector source changed')
            a.require(subprocess.check_output([plan['git'],'-C',repo,'rev-parse','HEAD^{tree}'],text=True).strip()==a.TREE,'collector tree changed')
            a.require(not subprocess.check_output([plan['git'],'-C',repo,'status','--porcelain'],text=True).strip(),'collector source dirty')
        sourcecheck()
        before=[dict(ref) for ref in plan['inputs']]
        for ref in before:a.verify(ref)
        sourcecheck()
        item=plan['producers'][plan['stage']]
        entries=[]
        for request in plan['companions']:
            entry=dict(request,current=a.identity(request['producerOutputPath']),
                evidenceTiming=TIMING,firstObservedUtc=datetime.datetime.now(datetime.timezone.utc).isoformat())
            entries.append(entry)
        receipt=dict(schema=SCHEMA,origin=origin(item,plan['stage']),evidenceTiming=TIMING,
                     driftBefore=before,driftAfter=before,companions=entries)
        validate(receipt,item,plan['stage'])
        for entry in entries:a.verify(entry['current'])
        for ref in before:a.verify(ref)
        monitor(force=True)
        state['state']='verified_collection_no_processes_launched';save()
        a.write(out/'receipt.json',receipt)
        a.write(out/'seal.json',dict(schema='b5f-companion-seal-v1',receipt=a.identity(out/'receipt.json')))
    except BaseException as exc:
        state.update(state='failed_partial',error=repr(exc));save();raise
    finally:a.IO_MONITOR=previous


if __name__=='__main__':
    import argparse
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--plan',required=True);p.add_argument('--plan-sha256',required=True)
    p.add_argument('--go',required=True);p.add_argument('--go-sha256',required=True)
    args=p.parse_args()
    a.require(a.identity(args.plan)['sha256']==args.plan_sha256,'collector plan identity')
    a.require(a.identity(args.go)['sha256']==args.go_sha256,'collector GO identity')
    collect(a.small_json(args.plan),a.small_json(args.go))
