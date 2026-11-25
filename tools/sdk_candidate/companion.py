"""Append-only first-observation companions; never compiler-read evidence.

Only collector GO permits real observation. Import and validation are inert.
"""
from pathlib import Path
import datetime
import json
import xml.etree.ElementTree as ET
import time
import importlib.util
import re
import ntpath
import adapter as a

SCHEMA='b5f-producer-companion-v1'
TIMING='FIRST_OBSERVED_NOW_NOT_HISTORICAL_COMPILE_TIME_HASH'
ROLES={'cache','runner','import-lib','runtime','approved-binding'}
APPROVED_VERIFIER_SHA='6ca38df04890d75c54594cdd79788473b164e3680c048032591734f1099b12a5'
APPROVED_RULE_SHA='6134e9f2c1a5ad901505723fc1deff1c97be52d675e907247db723a86296f012'


def build_root(producer,stage):
    parent=Path(producer['rawSource']['buildPath']).resolve().parent
    return parent.parent if stage.endswith('windows') else parent


def separate_output(output,producers,source):
    out=Path(output).resolve()
    for root in [Path(source).resolve()]+[build_root(p,s) for s,p in producers.items()]:
        a.require(not out.is_relative_to(root) and not root.is_relative_to(out),'collector output overlaps producer/source root')


def project_outputs(path,target,role):
    """Limited CMake MSVC Release|x64 properties, no MSBuild execution."""
    root=ET.fromstring(Path(path).read_bytes())
    values={'Configuration':'Release','Platform':'x64','ProjectDir':str(Path(path).parent)+'/',
            'ProjectName':target,'TargetName':target}
    outputs=[]
    def enabled(node):
        cond=node.get('Condition','')
        return not cond or cond.replace(' ','') in ("'$(Configuration)|$(Platform)'=='Release|x64'",)
    def visit(node):
        if not enabled(node):return
        tag=node.tag.split('}')[-1]
        if node.text and node.text.strip() and tag in ('OutDir','TargetName','TargetExt','OutputFile','ImportLibrary','AdditionalDependencies'):
            text=node.text.strip()
            for _ in range(6):
                old=text
                for key,value in values.items():text=text.replace('$('+key+')',value)
                if old==text:break
            if tag in ('OutDir','TargetName','TargetExt'):values[tag]=text
            else:outputs.append((tag,text))
        for child in node:visit(child)
    visit(root)
    if role in ('runner','runtime') and all(k in values for k in ('OutDir','TargetName','TargetExt')):
        outputs.append(('OutputFile',values['OutDir']+values['TargetName']+values['TargetExt']))
    selected=[v for k,v in outputs if k==('ImportLibrary' if role=='import-lib' else 'OutputFile')]
    a.require(selected,'missing exact project link output')
    a.require(all('$(' not in v for v in selected),'unresolved Release output macros')
    return selected


def link_path(value):
    return str(value).strip().strip('"').replace('\\','/').lower()


def log_paths(path):
    data=Path(path).read_bytes()
    text=data.decode('utf-16') if data.startswith((b'\xff\xfe',b'\xfe\xff')) else data.decode('utf8')
    paths=set()
    for quoted,bare in re.findall(r'"([^"\r\n]+)"|([^\s;"]+)',text):
        value=(quoted or bare).lstrip('^')
        for prefix in ('/OUT:','/IMPLIB:'):
            if value.upper().startswith(prefix):value=value[len(prefix):]
        paths.add(link_path(value))
    return paths


def sdk_dependency(sdkproject,dependency,import_lib):
    """Only active Release XML roles; comments/plain text cannot grant an edge."""
    a.require(isinstance(import_lib,str) and import_lib.strip() and import_lib.strip()==import_lib
              and import_lib.lower().endswith('.lib') and ';' not in import_lib,'empty/invalid dependency import lib')
    root=ET.fromstring(Path(sdkproject).read_bytes())
    matched=False
    tokens=set()
    def expand(value):
        for key,val in {'Configuration':'Release','Platform':'x64','ProjectDir':str(Path(sdkproject).parent)+'/'}.items():
            value=value.replace('$('+key+')',val)
        return value
    def visit(node,in_link=False):
        nonlocal matched
        cond=node.get('Condition','').replace(' ','')
        if cond and cond!="'$(Configuration)|$(Platform)'=='Release|x64'":return
        tag=node.tag.split('}')[-1]
        if tag=='ProjectReference':
            value=expand(node.get('Include',''))
            candidate=Path(value.replace('\\','/'))
            if not candidate.is_absolute():candidate=Path(sdkproject).parent/candidate
            disabled=any(x.tag.split('}')[-1]=='LinkLibraryDependencies' and x.text and x.text.strip().lower()=='false' for x in node)
            if '$(' not in value and candidate.resolve()==Path(dependency).resolve() and not disabled:matched=True
        if tag=='AdditionalDependencies' and in_link and node.text:
            for token in node.text.split(';'):
                value=expand(token.strip()).strip('"')
                if value and '$(' not in value and '%(' not in value:tokens.add(link_path(value))
                if value and '$(' not in value and link_path(value)==link_path(import_lib):matched=True
        for child in node:visit(child,in_link or tag=='Link')
    visit(root)
    a.require(matched,'Release SDK dependency edge missing')
    return tokens


def frozen_value(actual,expected,kind):
    if actual==expected:return True
    if kind not in ('PATH','FILEPATH') or not isinstance(actual,str):return False
    if not (ntpath.isabs(actual) and ntpath.isabs(expected)):return False
    return ntpath.normcase(ntpath.normpath(actual))==ntpath.normcase(ntpath.normpath(expected))


def linked_records(records,historical,sdkproject,stage,import_path,sdk_output,dependency_tokens):
    """One typed MSVC record across command/read/write; never cwd inference."""
    a.require(isinstance(records,list) and len(records)==3,'three SDK link record roles required')
    by_role={r['role']:r for r in records}
    a.require(set(by_role)=={'command','read','write'},'missing/duplicate SDK link record role')
    target=Path(sdkproject).stem
    directory=Path(sdkproject).parent/(target+'.dir')/'Release'/(target+'.tlog')
    keys=[];paths={}
    for role,row in by_role.items():
        a.require(row['stage']==stage and row['target']==target and row['configuration']=='Release|x64','cross-target/config link records')
        file=historical(row['pin'])
        a.require(file.resolve()==(directory/('link.'+role+'.1.tlog')).resolve(),'wrong target/config tlog location')
        data=file.read_bytes()
        text=data.decode('utf-16') if data.startswith((b'\xff\xfe',b'\xfe\xff')) else data.decode('utf8')
        lines=text.splitlines()
        headers=[line for line in lines if line.startswith('^')]
        a.require(len(headers)==1 and lines[0]==headers[0] and len(lines)>1,'ambiguous/missing complete link record key')
        keys.append(headers[0]);paths[role]=log_paths(file)
    a.require(keys[0]==keys[1]==keys[2],'complete link object-set key differs')
    expected_lib=link_path(import_path);expected_sdk=link_path(sdk_output)
    a.require(expected_lib in paths['read'] and expected_sdk in paths['write'] and expected_sdk in paths['command'],
              'linked read/write actual input/output differs')
    # The relative command token is not resolved to projectDir/cwd. Matching
    # record read evidence is authoritative for its absolute input identity.
    name=ntpath.basename(expected_lib)
    commands=[x for x in paths['command'] if ntpath.basename(x)==name]
    reads=[x for x in paths['read'] if ntpath.basename(x)==name]
    a.require(len(commands)==1 and len(reads)==1 and reads[0]==expected_lib,'ambiguous or wrong linked library')
    a.require(commands[0]==expected_lib or commands[0] in dependency_tokens,'relative command library not declared by Release SDK')
    return dict(recordKeySha256=__import__('hashlib').sha256(keys[0].encode()).hexdigest(),recordKeyBytes=len(keys[0].encode()))


def appendix(receipt,producer,stage):
    pair=receipt.get('sourceAppendix')
    if pair is None:return []
    data=a.small_json(a.verify(pair))
    a.require(data['schema']=='b5f-companion-source-appendix-v1' and data['origin']==origin(producer,stage)
              and data['evidenceTiming']==TIMING,'appendix origin/timing differs')
    allowed={'normal-plan','project','link-tlog','approved-rule','approved-verifier','header-manifest','dependency-project'}
    for x in data['entries']:
        a.require(x['role'] in allowed and x['firstObservedUtc'],'unapproved appendix role')
        a.verify(x['pin'])
        if x['role'] in ('project','dependency-project','link-tlog'):
            a.require(Path(x['pin']['path']).resolve().is_relative_to(build_root(producer,stage)), 'appendix output not this producer')
    return [x['pin'] for x in data['entries']]


def index(item):
    refs=item['fullIndex'].get('files',item['fullIndex'].get('refs',[]))
    return refs+list(item.get('originalRecords',{}).values())


def origin(item,stage):
    return dict(source=a.SOURCE,tree=a.TREE,stage=stage,sourceSeal=item['seal'],
        fullIndex=item['fullIndex'],originalRecords=item.get('originalRecords',{}),
        rawSource=item['rawSource'],terminal=item['terminal'])


def basis(entry,producer,stage,source_graph,extra=()):
    """Check actual provenance artifacts, not a new 'passed' flag."""
    role=entry['role'];a.require(role in ROLES,'unapproved companion role')
    a.require(entry['stage']==stage,'companion stage differs')
    path=Path(entry['current']['path']).resolve()
    build=build_root(producer,stage)
    a.require(Path(entry['producerBuildRoot']).resolve()==build,'wrong producer build root')
    a.require(entry['current']!=producer['rawSource']['raw'] and path!=Path(producer['rawSource']['buildPath']).resolve(),
              'raw SDK cannot use companion path')
    a.require(path.suffix.lower() not in ('.o','.obj','.pch','.cpp','.h','.hpp','.c'), 'source/object bypass forbidden')
    evidence=entry['basis']
    pins=index(producer)
    def historical(ref):
        a.require(ref in pins or contains(source_graph,ref) or ref in extra,'basis is not original/controlled appended producer provenance')
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
        types={}
        for arg in configure:
            if arg.startswith('-D') and '=' in arg:
                k,v=arg[2:].split('=',1);expected[k.split(':')[0]]=v;types[k.split(':')[0]]=k.partition(':')[2]
        a.require(expected and values.get('ORCA_MODEL_SOURCE_COMMIT')==a.SOURCE,'cache source not frozen')
        a.require(all(frozen_value(values.get(k),v,types[k]) for k,v in expected.items()),'cache differs from frozen configure/options/toolchain/deps')
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
        output=project_outputs(project,target,role)
        declared=str(path).replace('\\','/').lower()
        a.require(any(x.replace('\\','/').lower()==declared for x in output),'missing exact project link output')
        tlog=historical(evidence['linkTlog'])
        data=tlog.read_bytes()
        text=data.decode('utf-16') if data.startswith((b'\xff\xfe',b'\xfe\xff')) else data.decode('utf8')
        a.require(declared in text.replace('\\','/').lower(),'missing actual link tlog output')
        if role=='runner':
            with path.open('rb') as stream:
                a.require(stream.read(2)==b'MZ','runner is not PE')
            # Runner may delegate version/capabilities to the SDK DLL rather
            # than embed a commit. Source identity is the exact normal target,
            # source closure, project and actual link graph, not a new marker.
        else:
            with path.open('rb') as stream:a.require(stream.read(8)==b'!<arch>\n','import lib structure')
    elif role in ('runtime','approved-binding'):
        if stage.endswith('windows'):
            a.require(role=='runtime' and path.suffix.lower()=='.dll','invalid Windows runtime role')
            project=historical(evidence['dependencyProject'])
            target=evidence['dependencyTarget']
            a.require(project.name==target+'.vcxproj','dependency project target differs')
            output=project_outputs(project,target,'runtime')
            declared=str(path).replace('\\','/').lower()
            a.require(any(x.replace('\\','/').lower()==declared for x in output),'runtime dependency output differs')
            sdkproject=historical(evidence['project'])
            expected_sdk='orca_model_sdk.vcxproj' if stage=='model-windows' else 'orca_mobile_slicing_sdk.vcxproj'
            a.require(sdkproject.name==expected_sdk,'wrong stage SDK dependency project')
            a.require('--target' in build_argv and sdkproject.stem in build_argv[build_argv.index('--target')+1:], 'normal SDK target missing')
            requested=evidence['dependencyImportLib']
            dependency_tokens=sdk_dependency(sdkproject,project,requested)
            # The DLL project's Release import-library output and the SDK's
            # actual link input must name the same artifact, not a substring.
            xml=ET.fromstring(project.read_bytes())
            has_import=any(x.tag.split('}')[-1]=='ImportLibrary' for x in xml.iter())
            imports=project_outputs(project,target,'import-lib') if has_import else [str(path.with_suffix('.lib'))]
            a.require(len(imports)==1,'ambiguous dependency Release import lib')
            import_path=imports[0]
            requested_path=requested.replace('\\','/')
            a.require(link_path(requested_path)==link_path(import_path) or
                      ('/' not in requested_path and requested_path.lower()==Path(import_path.replace('\\','/')).name.lower()),
                      'dependency library does not belong to runtime project')
            dependency_log=log_paths(historical(evidence['linkTlog']))
            a.require(declared in dependency_log and link_path(import_path) in dependency_log,'runtime/import-lib actual link outputs missing')
            a.require(('sdkLinkTlog' in evidence)!=('sdkLinkRecords' in evidence),'conflicting/missing SDK link proof modes')
            if 'sdkLinkRecords' in evidence:
                linked_records(evidence['sdkLinkRecords'],historical,sdkproject,stage,import_path,producer['rawSource']['buildPath'],dependency_tokens)
            else:
                sdk_log=log_paths(historical(evidence['sdkLinkTlog']))
                a.require(link_path(import_path) in sdk_log and link_path(producer['rawSource']['buildPath']) in sdk_log,
                          'SDK actual link input/output relationship missing')
            with path.open('rb') as stream:a.require(stream.read(2)==b'MZ','runtime is not PE')
            return
        approved=a.small_json(historical(evidence['approvedRule']))
        a.require(evidence['approvedRule']['sha256']==APPROVED_RULE_SHA
                  and Path(evidence['approvedRule']['path']).name=='approved_tbb.json','wrong approved rule role')
        binding=a.small_json(a.verify(evidence['binding']))
        a.require(contains(source_graph,dict(path=binding['runtimePath'],sha256=approved['runtime']['sha256'],bytes=approved['runtime']['bytes'])),
                  'approved runtime not corresponding stage input')
        a.require(Path(binding['headerRoot']).is_dir(),'approved header root missing')
        headers=a.small_json(historical(evidence['headerProof']))
        a.require(Path(headers['headerRoot']).resolve()==Path(binding['headerRoot']).resolve()
                  and headers['headers']==approved['headers'],'headerRoot/approved header inventory differs')
        a.require(any(contains(source_graph,dict(path=str(Path(binding['headerRoot'])/name),sha256=digest))
                      for name,digest in approved['headers'].items()),'headerRoot not corresponding producer read provenance')
        verifier=historical(evidence['approvedVerifier'])
        a.require(verifier.name=='verify_approved_tbb.py' and evidence['approvedVerifier']['sha256']==APPROVED_VERIFIER_SHA
                  and verifier.parent==Path(evidence['approvedRule']['path']).parent,'unapproved validator role/tool identity')
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
    extra=appendix(receipt,producer,stage)
    if receipt.get('sourceAppendix'):
        a.require(receipt['sourceAppendix'] in receipt['driftBefore'] and all(x in receipt['driftBefore'] for x in extra),
                  'appendix drift coverage missing')
    for entry in receipt['companions']:
        a.require(entry['evidenceTiming']==TIMING and entry['firstObservedUtc'],'missing first observation')
        a.require(entry['current']['path'] not in seen,'duplicate companion')
        seen.add(entry['current']['path'])
        basis(entry,producer,stage,graph,extra)


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
    extra=[receipt['sourceAppendix']] if receipt.get('sourceAppendix') else []
    if extra:extra += [x['pin'] for x in a.small_json(a.verify(extra[0]))['entries']]
    linked=[r['pin'] for x in receipt['companions'] for r in x['basis'].get('sdkLinkRecords',[])]
    return [pair['seal'],pair['receipt']]+extra+linked+receipt['driftBefore']+[
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
    if plan.get('sourceAppendix'):
        extra=appendix(dict(sourceAppendix=plan['sourceAppendix']),plan['producers'][plan['stage']],plan['stage'])
        a.require(plan['sourceAppendix'] in plan['inputs'] and all(x in plan['inputs'] for x in extra),'collector appendix input closure missing')
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
    separate_output(out,plan['producers'],plan['sdkRepository'])
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
        if plan.get('sourceAppendix'):
            a.require(go['approvedSourceAppendix']==plan['sourceAppendix'],'appendix not specifically approved by collection GO')
            receipt['sourceAppendix']=plan['sourceAppendix']
        validate(receipt,item,plan['stage'])
        for entry in entries:a.verify(entry['current'])
        for ref in before:a.verify(ref)
        monitor(force=True)
        sourcecheck()
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
