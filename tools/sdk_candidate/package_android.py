"""Android-only b5f source artifact. Default inspection never executes a tool.

Two sealed Android producers; capabilities are pending phone observation.
The original full package mode remains independent.
"""
from pathlib import Path
import argparse
import datetime as dt
import hashlib
import importlib.util
import json
import os
import re
import subprocess
import sys
import time
import unicodedata
import zipfile
import adapter as a
import companion
from android_elf import runtime_identity, source_marker
from suspended_root import PROFILE, FLAGS

SCHEMA = 'orca-android-sdk-v1'
PLAN = 'orca-android-sdk-plan-v1'
ROOT = '01a20286-30e2-7a20-a134-d11895959c44'
TBB = dict(sha256='cc093ffa23653600950960121ab5c4261e6586aa14f7dadc91aab6773daaa038',bytes=726384)
CGAL_SHA = 'eebd737d9b7f0199647ba5c1f9f6c7a3d0651aacef9c2fd4dedc52da6c25edbe'
STAGES = ('model-android','slicing-android')
NAMES = dict(zip(STAGES, ('liborca_model_sdk_c.so','liborca_mobile_slicing_sdk_c.so')))
SYSTEM = {'liblog.so','libm.so','libdl.so','libc.so','libandroid.so','libz.so','libEGL.so','libGLESv2.so'}
ORIGINS = {
 'model-android': dict(seal='1e6a88d930f12e208a68347fda510ada180cb46069553966e01cd34fa328f3e7',
   rawSha='90b81c248670999ef071432f489138d65dad329295f9966a3d86eb00a5c76dd8',rawBytes=81377656),
 'slicing-android': dict(seal='32cca4d5383acc6a6ad11e9b68d767a917493d896da2300ce45535af4eb76b81',
   rawSha='eafe048e5196ae43fd71f2d556168679219c38d28bfaee3eaf6ec37696cda847',rawBytes=704366320)}
HERE = Path(__file__).resolve().parent
need = a.require


def key(path):return str(Path(path).resolve())


def pins(value):
    if isinstance(value,dict):
        if set(('path','bytes','sha256')) <= value.keys():yield value
        for child in value.values():yield from pins(child)
    elif isinstance(value,list):
        for child in value:yield from pins(child)


def exact_refs(refs):
    out = {}
    for ref in refs:
        need(isinstance(ref,dict) and Path(ref['path']).is_absolute()
             and type(ref['bytes']) is int and ref['bytes'] >= 0
             and re.fullmatch('[0-9a-f]{64}',ref['sha256']), 'malformed Android pin')
        k = key(ref['path'])
        need(k not in out or out[k] == ref, 'conflicting Android identity')
        out[k] = ref
    return out


def life(plan, enabled=False):
    return dict(schema='android-artifact-lifecycle-v1',enabled=enabled,profile=PROFILE,
        effectiveCreationFlags=FLAGS,neverResumedAbortExitCode=74,custodian='CURRENT_CREATING_PROCESS',
        source=a.SOURCE,toolSource=plan['toolSource'],cwd=str(Path(plan['sdkRepository'])),
        commandTimeoutSeconds=7200,commands=commands(plan),toolFiles=plan['toolFiles'])


def validate_life(plan, go=None):
    value = plan.get('lifecycle')
    need(isinstance(value,dict) and type(value.get('enabled')) is bool
         and all(type(value.get(k)) is int for k in ('effectiveCreationFlags','neverResumedAbortExitCode','commandTimeoutSeconds')),
         'typed Android lifecycle required')
    need(value == life(plan,True), 'explicit enabled Android lifecycle required')
    if go is not None:
        need(go.get('lifecycle') == value, 'Android GO lifecycle differs')
        other=go['lifecycle']
        need(type(other['enabled']) is bool and all(type(other[k]) is int for k in
             ('effectiveCreationFlags','neverResumedAbortExitCode','commandTimeoutSeconds')), 'typed GO lifecycle required')
    return value


def runtime_inputs(plan):
    rows=plan['runtimeInputs']
    need(isinstance(rows,dict) and set(NAMES.values()) <= set(rows) and 'libtbb.so' in rows,
         'two SDKs and approved TBB runtime required')
    for name,row in rows.items():
        need(re.fullmatch(r'lib[A-Za-z0-9_+.-]+\.so',name) and Path(row['source']['path']).name == name,
             'unsafe or renamed Android runtime')
        need(row['stage'] in STAGES, 'runtime producer stage differs')
    return rows


def commands(plan):
    result=[]
    for name,row in sorted(runtime_inputs(plan).items()):
        raw=str(Path(row['source']['path']));out=str(Path(plan['output'])/'android/arm64-v8a'/name)
        if name != 'libtbb.so':
            result.append([plan['strip'],'--strip-debug' if name==NAMES['model-android'] else '--strip-unneeded',raw,'-o',out])
        for path in (raw,out):
            result += [[plan['readelf'],'-d',path],[plan['readelf'],'--dyn-syms','--wide',path]]
    return result


def original_graph(plan, stage, *, verify_all=False):
    item=plan['producers'][stage];expected=ORIGINS[stage]
    need(item is not None and item['seal']['sha256']==expected['seal'], 'unqualified Android producer seal')
    sealed=a.small_json(a.verify(item['seal']))
    need(sealed==item['fullIndex'], 'Android seal/fullIndex differs')
    refs=sealed.get('files',sealed.get('refs'))
    need(isinstance(refs,list) and refs and len(exact_refs(refs))==len(refs), 'incomplete original index')
    def member(name):
        matches=[r for r in refs if Path(r['path']).name==name]
        need(len(matches)==1,'missing/ambiguous original '+name)
        return matches[0]
    for name,expected_value in [('actual-terminal.json',item['terminal']),('raw-source.json',item['rawSource'])]:
        need(a.small_json(a.verify(member(name)))==expected_value,'original Android metadata substituted')
    a.terminal_ok(stage,item['terminal'],item['rawSource'])
    raw=item['rawSource']['raw']
    need(raw['sha256']==expected['rawSha'] and raw['bytes']==expected['rawBytes']
         and Path(raw['path']).name==NAMES[stage] and Path(item['rawSource']['buildPath']).name==NAMES[stage],
         'old or incorrect Android raw identity')
    origins=[r for r in refs if Path(r['path']).name=='recovery-origin.json']
    original=a.small_json(a.verify(origins[0]))['originalRecords'] if origins else {}
    need(original==item.get('originalRecords',{}),'original recovery records substituted')
    graph=exact_refs(refs+list(original.values()))
    summary_ref=plan['resourceSummaries'][stage]
    need(graph.get(key(summary_ref['path']))==summary_ref,'resource summary outside original graph')
    summary=a.small_json(a.verify(summary_ref))
    record=graph.get(key(Path(summary_ref['path']).parent/'execution.json'))
    need(record is not None and summary['source']==a.SOURCE,'same-directory original execution missing')
    rows=summary.get('records') if stage=='model-android' else summary.get('commands')
    need(isinstance(rows,list) and [r['name'] for r in rows]==['configure','actual-ninja-commands','build','actual-ninja-deps'],
         'complete original Android commands required')
    for row in rows:
        need(row['state']=='returned' and row['exitCode']==0 and not row['monitorErrors']
             and not row['runtimeViolations'] and row.get('cancelReason') is None,'failed original Android resources')
    if stage=='slicing-android':
        need(summary['originalRecordSha256']==record['sha256'] and summary['originalRecordBytes']==record['bytes'],
             'Slicing original execution identity differs')
    else:
        need(item['terminal']['owner29816Absent'] is True and item['terminal']['sourceClean'] is True,
             'unclosed Model Android source/owner')
    source=a.small_json(a.verify(member('actual-compiler-inputs.json')))
    inventory=a.small_json(a.verify(member('new-object-inventory.json')))
    member('actual-source-command-flags.json')
    all_refs=list(graph.values())+list(pins(source))+list(pins(inventory))+[raw,
        dict(raw,path=item['rawSource']['buildPath']),item['seal']]
    if verify_all:
        for ref in exact_refs(all_refs).values():a.verify(ref) # execution streamed, never JSON-loaded
    return item,source,all_refs


def check_roots(plan):
    roots=[Path(plan[k]).resolve() for k in ('output','debug','evidence')]
    need(len(set(roots))==3 and all(not p.exists() for p in roots),'three fresh Android roots required')
    forbidden=[Path(plan['sdkRepository']).resolve(),HERE.parents[1]]
    for stage,item in plan['producers'].items():
        forbidden += [Path(item['rawSource']['buildPath']).resolve().parent,
                      Path(item['seal']['path']).resolve().parent,
                      Path(item['rawSource']['raw']['path']).resolve().parent]
    for x in roots:
        need(all(x==y or not x.is_relative_to(y) for y in roots), 'nested Android roots')
        need(all(not x.is_relative_to(y) and not y.is_relative_to(x) for y in forbidden),'Android output/source overlap')
    need(re.fullmatch('sdk-android-'+a.SOURCE[:12]+r'-r[1-9][0-9]*',roots[0].name),'Android bundle namespace required')


def support(plan, stage, source, *, required=True):
    pair=plan.get('companions',{}).get(stage)
    if not required and pair is None:return []
    need(pair is not None,'Android cache/approved-binding companion unbound')
    return companion.accepted(dict(companion=pair),plan['producers'][stage],stage)


def validate(plan, *, companions=True, verify_all=False):
    need(plan['schema']==PLAN and plan['source']==a.SOURCE and plan['tree']==a.TREE,
         'Android source/schema mismatch')
    need(set(plan['producers'])==set(STAGES) and set(plan['resourceSummaries'])==set(STAGES),
         'exact two Android producers required')
    need(re.fullmatch('[0-9a-f]{40}',plan['toolSource']) and plan['toolSource']!=a.SOURCE,'separate tool commit required')
    need(plan.get('functionalQualification') is None,'phone qualification cannot be inherited')
    need(set(plan.get('qualification',{}))=={'review','qa'},'Android tool qualification required')
    check_roots(plan)
    runtime_inputs(plan)
    allowed={}
    closure=[]
    for stage in STAGES:
        item,source,refs=original_graph(plan,stage,verify_all=verify_all)
        extra=support(plan,stage,source,required=companions)
        allowed[stage]=(item,source,extra)
        closure += refs
        if companions:
            cache=next((r for r in extra if Path(r['path']).name=='CMakeCache.txt'),None)
            binding=next((r for r in extra if Path(r['path']).name=='approved-tbb-binding.json'),None)
            need(cache is not None and binding is not None,'cache and approved-binding companion required')
            closure += companion.closure(dict(companion=plan['companions'][stage]))
    for stage,name in NAMES.items():
        need(plan['runtimeInputs'][name]==dict(stage=stage,source=plan['producers'][stage]['rawSource']['raw']),
             'canonical Android raw must match qualified producer')
    for name,row in runtime_inputs(plan).items():
        if name in NAMES.values():continue
        item,source,extra=allowed[row['stage']]
        need(companion.contains(source,row['source']) or row['source'] in extra,'runtime outside exact source/companion graph')
    tbb=plan['runtimeInputs']['libtbb.so']['source']
    need(all(tbb[k]==v for k,v in TBB.items()),'approved TBB original bytes required')
    return list(exact_refs(closure).values())


def tool_refs(plan):
    refs=exact_refs(plan['toolFiles'])
    need(len(refs)==len(plan['toolFiles']),'duplicate Android tool pins')
    local=['package_android.py','android_elf.py','adapter.py','companion.py','artifact_owner.py','api_context.py',
           'suspended_root.py','suspended_launch.py']
    required=[HERE/n for n in local]
    import artifact_owner
    required += [Path(plan['ownerDirectory'])/n for n in artifact_owner.SUPPORT]
    repo=Path(plan['sdkRepository'])
    required += [repo/n for n in ('src/orca_model_sdk/tools/strip_android_model.py',
        'src/orca_model_sdk/repair_core/verify_approved_tbb.py','src/orca_model_sdk/repair_core/approved_tbb.json',
        'mobile-slicing/build_android_arm64.cjs')]
    for name,base in [('python','python.exe'),('git','git.exe'),('readelf','llvm-readelf.exe'),('strip','llvm-strip.exe')]:
        need(Path(plan[name]).name.lower()==base,'Android tool image role differs')
        required.append(Path(plan[name]))
    need(key(plan['python'])==key(sys.executable),'running Android Python image differs')
    required += [Path(plan['dependencyManifest']['path']),Path(plan['cgalArchive']['path'])]
    for path in required:
        need(key(path) in refs,'missing Android tool closure '+str(path));a.verify(refs[key(path)])
        if path.parent==HERE:
            module=sys.modules.get(path.stem)
            need(module is None or key(getattr(module,'__file__',''))==key(path),'shadowed Android source module')
    for name,digest in artifact_owner.SUPPORT.items():
        need(refs[key(Path(plan['ownerDirectory'])/name)]['sha256']==digest,'original artifact core changed')
    need(plan['artifactOwner']==str(HERE/'artifact_owner.py'),'actual artifact owner required')
    for role,ref in plan['qualification'].items():
        receipt=a.small_json(a.verify(ref))
        need(receipt['schema']=='android-artifact-qualification-v1' and receipt['role']==role
             and receipt['outcome']=='passed' and receipt['toolSource']==plan['toolSource']
             and json.dumps(receipt['toolFiles'],sort_keys=True)==json.dumps(plan['toolFiles'],sort_keys=True)
             and json.dumps(receipt['lifecycle'],sort_keys=True)==json.dumps(plan['lifecycle'],sort_keys=True)
             and json.dumps(receipt['inputs'],sort_keys=True)==json.dumps(input_contract(plan),sort_keys=True),
             'Android concrete-contract qualification differs')
        a.verify(receipt['report'])
    return list(refs.values())


def input_contract(plan):
    return {k:plan.get(k) for k in ('producers','resourceSummaries','companions','runtimeInputs',
                                   'sourceAppendices','observationRequests')}


def owner_contract(plan):
    validate_life(plan)
    need(set(plan['producers'])==set(STAGES),'Android owner two-stage scope')
    tool_refs(plan)
    return life(plan,True)


def fresh(go):
    now=dt.datetime.now(dt.timezone.utc)
    issued=dt.datetime.fromisoformat(go['issuedUtc'].replace('Z','+00:00'))
    expiry=dt.datetime.fromisoformat(go['expiresUtc'].replace('Z','+00:00'))
    need(issued<=now<expiry and 0<(expiry-issued).total_seconds()<=360,'Android GO expired')


def source_check(plan):
    for repo,commit,tree in [(Path(plan['sdkRepository']),a.SOURCE,a.TREE),(HERE.parents[1],plan['toolSource'],None)]:
        def git(*argv):return subprocess.check_output([plan['git'],'-C',str(repo),*argv],text=True,env=dict(os.environ),cwd=repo).strip()
        need(git('rev-parse','HEAD')==commit and not git('status','--porcelain'),'Android source/tool dirty or changed')
        if tree:need(git('rev-parse','HEAD^{tree}')==tree,'SDK tree changed')


def exact_go(plan,go,plan_sha,observe=False):
    need(go.get('schema')=='android-artifact-go-v1' and go.get('authorized') is True
         and type(go['authorized']) is bool and go.get('rootConversation')==ROOT
         and go.get('artifactOnlyAuthorized') is True and go.get('allowSDKLoad') is False,
         'explicit Android artifact-only root GO required')
    fresh(go)
    need(go.get('operation')==('observe-inputs' if observe else 'package-android'), 'Android GO operation differs')
    need(go['planSha256']==plan_sha and a.identity(go['planPath'])['sha256']==plan_sha
         and key(plan['bindingPath'])==key(go['planPath']) and a.small_json(go['planPath'])==plan,'Android exact plan mismatch')
    need(key(go['cancel'])==key(Path(plan['evidence']).with_suffix('.cancel')),'dedicated Android cancellation channel required')
    validate_life(plan,go)
    closure=validate(plan,companions=not observe,verify_all=True)+tool_refs(plan)+list(pins(plan['runtimeInputs']))
    if observe:
        for stage in STAGES:
            appendix=plan['sourceAppendices'][stage]
            closure += [appendix]+companion.appendix(dict(sourceAppendix=appendix),plan['producers'][stage],stage)
        closure += list(pins(plan['observationRequests']))
    closure += list(plan['qualification'].values())
    closure += [a.small_json(a.verify(r))['report'] for r in plan['qualification'].values()]
    expected=exact_refs(closure);actual=exact_refs(go['closure'])
    need(len(actual)==len(go['closure']) and actual==expected,'exact Android GO closure differs')
    for ref in actual.values():a.verify(ref)
    source_check(plan)


def dynamic_tables(command,ref,label,kind=None):
    dynamic=command(['-d',str(ref)],label+'-needed')
    symbols=command(['--dyn-syms','--wide',str(ref)],label+'-dynsym')
    need('Dynamic section' in dynamic and "Symbol table '.dynsym'" in symbols,'full ELF dynamic tables required')
    needed=re.findall(r'Shared library: \[([^\]]+)\]',dynamic)
    need(len(needed)==len(set(needed)),'duplicate NEEDED')
    if kind:
        prefix='orca_model_' if kind=='model' else 'orca_mobile_slicing_'
        public={x.split()[-1] for x in symbols.splitlines() if len(x.split())>=8
                and x.split()[4:6]==['GLOBAL','DEFAULT'] and x.split()[6]!='UND' and x.split()[-1].startswith('orca_')}
        need(public=={prefix+n for n in a.PUBLIC[kind]},'complete Android public ABI differs')
    return dict(needed=needed,dynamic=dynamic,symbols=symbols)


def process_libraries(plan, command, monitor):
    out=Path(plan['output'])/'android/arm64-v8a';out.mkdir(parents=True)
    debug=Path(plan['debug']);debug.mkdir(parents=True)
    proof={};needed={}
    for name,row in sorted(runtime_inputs(plan).items()):
        raw=a.verify(row['source']);released=out/name
        before,raw_debug=runtime_identity(raw,monitor)
        kind='model' if name==NAMES['model-android'] else ('slicing' if name==NAMES['slicing-android'] else None)
        if kind:need(source_marker(raw,a.SOURCE,monitor),'Android raw source marker missing')
        first=dynamic_tables(lambda flags,label:command([plan['readelf'],*flags],label),raw,name+'-raw',kind)
        if name=='libtbb.so':
            a.stream_copy(raw,released)
            actual=a.identity(released)
            need(all(actual[k]==row['source'][k] for k in ('sha256','bytes')),'approved TBB copy bytes differ')
        else:
            if kind=='model':
                need(raw_debug,'Model debug sections required')
                a.stream_copy(raw,debug/'liborca_model_sdk_c.unstripped.so')
                need(a.identity(debug/'liborca_model_sdk_c.unstripped.so')['sha256']==row['source']['sha256'],
                     'preserved Model debug raw differs')
            command([plan['strip'],'--strip-debug' if kind=='model' else '--strip-unneeded',str(raw),'-o',str(released)],name+'-strip')
        after,released_debug=runtime_identity(released,monitor)
        need(before==after,'ALLOC/PT_LOAD/loader/runtime bytes changed')
        if kind=='model':need(not released_debug,'Model debug sections retained')
        if kind:need(source_marker(released,a.SOURCE,monitor),'Android release source marker missing')
        second=dynamic_tables(lambda flags,label:command([plan['readelf'],*flags],label),released,name+'-release',kind)
        need(first==second,'NEEDED/dynsym changed after strip')
        a.verify(row['source'])
        needed[name]=first['needed']
        proof[name]=dict(raw=row['source'],release=a.identity(released),runtimeIdentity=after,
            removedDebugSections=raw_debug if kind=='model' else [],operation='copy-byte-exact' if name=='libtbb.so' else
            ('strip-debug' if kind=='model' else 'strip-unneeded'),sourceMarker=bool(kind),dynamicTablesEqual=True)
    reachable=set(NAMES.values());todo=list(reachable)
    while todo:
        for name in needed[todo.pop()]:
            if name in SYSTEM:continue
            need(name in needed,'unresolved Android NEEDED '+name)
            if name not in reachable:reachable.add(name);todo.append(name)
    need(reachable==set(needed),'extra unrequested runtime outside NEEDED closure')
    return dict(schema='android-static-elf-v1',libraries=proof,needed=needed,systemLibraries=sorted(SYSTEM),sdkLoaded=False)


def cache_values(path):
    return {x.split(':',1)[0]:x.split('=',1)[1] for x in Path(path).read_text(encoding='utf8').splitlines()
            if not x.startswith(('#','//')) and ':' in x and '=' in x}


def archive_name(name):
    """Canonical portable names are checked without rewriting source identities."""
    need(isinstance(name,str) and name,'empty archive name')
    a.relative(name)
    need(not any(ord(c)<32 or ord(c)==127 or c in '<>:"\\|?*' for c in name),
         'nonportable archive name')
    for part in name.split('/'):
        need(not part.endswith((' ','.')),'archive component ends in space/dot')
        stem=part.split('.',1)[0].upper()
        need(stem not in {'CON','PRN','AUX','NUL','CONIN$','CONOUT$','CLOCK$'}
             and not re.fullmatch('(?:COM|LPT)[1-9¹²³]',stem),
             'reserved Windows device name')
    return name


def register_names(names):
    """One file namespace: exact/case/Unicode aliases and file/dir clashes fail."""
    seen=set()
    for name in names:
        archive_name(name)
        folded=unicodedata.normalize('NFC',name).casefold()
        need(folded not in seen,'duplicate/case/Unicode archive name')
        seen.add(folded)
    for name in seen:
        parts=name.split('/')
        need(all('/'.join(parts[:n]) not in seen for n in range(1,len(parts))),
             'archive file/directory prefix conflict')


def safe_target(root, name):
    archive_name(name)
    base=Path(root).absolute();target=base.joinpath(*name.split('/'))
    need(target.resolve().is_relative_to(base.resolve()),'archive target escapes root')
    # Check existing ancestors, including the root, before mkdir/open and reads.
    for path in [target,*target.parents]:
        need(not path.is_symlink() and not getattr(path,'is_junction',lambda:False)(),
             'archive target/ancestor is a link')
        if path!=target and path.exists():need(path.is_dir(),'archive parent is not a directory')
    need(not target.exists() or target.is_file(),'archive target is not a regular file')
    return target


def payload_files(root):
    out=Path(root);files=[]
    for path in out.rglob('*'):
        name=path.relative_to(out).as_posix()
        safe_target(out,name) if not path.is_dir() else safe_target(out,name+'/.path-check')
        if path.is_file():files.append((name,path))
    register_names([name for name,_ in files])
    return sorted(files)


def write_bundle(out, bundle):
    """Validate the final complete namespace and declared bytes before opening ZIP."""
    out=Path(out)
    data=a.small_json(safe_target(out,'manifest.json'))
    declared=data['files']
    names=[row['path'] for row in declared]+['manifest.json']
    register_names(names)
    actual=payload_files(out)
    need({name for name,_ in actual}==set(names),'final Android archive file set differs')
    for row in declared:
        need(type(row['sizeBytes']) is int and row['sizeBytes']>=0,'invalid manifest file size')
        a.verify(dict(path=str(safe_target(out,row['path'])),bytes=row['sizeBytes'],sha256=row['sha256']))
    destination=safe_target(Path(bundle).parent,Path(bundle).name)
    with zipfile.ZipFile(destination,'x',zipfile.ZIP_DEFLATED,compresslevel=1) as z:
        for name,_ in actual:
            a.zip_copy(z,safe_target(out,name),archive_name(name))


def snapshot(plan, monitor):
    repo=Path(plan['sdkRepository']);out=Path(plan['output'])
    tracked=subprocess.check_output([plan['git'],'-C',str(repo),'ls-files','-z'],env=dict(os.environ),cwd=repo).decode('utf8').split('\0')
    files=[(n,repo/n) for n in tracked if n and (n.startswith(('src/','mobile-slicing/','tests/','deps_src/','deps/','cmake/','scripts/','resources/'))
         or n in ('LICENSE.txt','CMakeLists.txt') or n.startswith(('Docs/','docs/')))]
    need(any(n=='mobile-slicing/tests/honor_multihead_thermal_test.cpp' for n,_ in files),'permanent thermal source missing')
    deps=a.dependency_json(plan['dependencyManifest'])
    need(deps,'dependency source inputs required')
    caches=[]
    for stage in STAGES:
        receipt=a.small_json(a.verify(plan['companions'][stage]['receipt']))
        cache=next(x['current'] for x in receipt['companions'] if x['role']=='cache')
        caches.append(cache_values(a.verify(cache)))
    keys=[item['key'] for item in deps]
    for namespace in keys:
        need(isinstance(namespace,str) and re.fullmatch('[A-Z][A-Z0-9_]*',namespace),
             'canonical single-component dependency namespace required')
        archive_name(namespace)
    register_names(keys)
    for item in deps:
        root=Path(item['path']).resolve()
        need(any(Path(c.get(item['key'],'.')).resolve()==root for c in caches),'dependency/cache input mismatch')
        for ref in item['files']:
            rel=archive_name(ref['path']);path=safe_target(root,rel)
            need(path.resolve().is_relative_to(root),'dependency source escape')
            need(a.identity(path)['sha256']==ref['sha256'],'dependency source drift')
            name='dependencies/'+item['key']+'/'+rel
            files.append((name,path))
    metadata=['source-files.json','dependency-source-inputs.json']
    register_names([name for name,_ in files]+metadata)
    notices=[('licenses/'+name,path) for name,path in files if
        Path(name).name.upper().split('.')[0] in {'LICENSE','COPYING','COPYRIGHT','NOTICE'}
        or 'LICENSES' in [part.upper() for part in Path(name).parts]]
    headers=[('src/orca_model_sdk/include/orca/model/model_sdk_c.h','include/orca/mobile/model_sdk_c.h'),
             ('src/orca_model_sdk/include/orca/model/model_sdk_c.h','include/orca/model/model_sdk_c.h'),
             ('src/orca_mobile_slicing_sdk/include/orca/mobile/slicing_sdk_c.h','include/orca/mobile/slicing_sdk_c.h')]
    generated=['source-snapshot.zip','licenses/nlohmann-json-LICENSE.MIT','dependencies/CGAL-6.2.1.zip']
    planned=[name for name,_ in notices]+[name for _,name in headers]+generated
    # The entire source ZIP and every future license/header/archive destination
    # must pass before the first snapshot or notice write.
    register_names(planned)
    existing=[name for name,_ in payload_files(out)]
    register_names(existing+planned+['manifest.json'])
    for name,path in files:
        archive_name(name);safe_target(repo,name) if path.is_relative_to(repo) else None
    for name in planned:safe_target(out,name)
    sources=[]
    with zipfile.ZipFile(safe_target(out,'source-snapshot.zip'),'x',zipfile.ZIP_DEFLATED,compresslevel=1) as z:
        for name,path in sorted(files):
            archive_name(name)
            need(safe_target(path.parent,path.name)==path.absolute(),'source ZIP input path changed')
            monitor();before=a.identity(path);a.zip_copy(z,path,name)
            need(a.identity(path)==before,'snapshot source changed')
            sources.append(dict(path=name,sha256=before['sha256']))
        z.writestr(archive_name('source-files.json'),json.dumps(sources,indent=2))
        z.writestr(archive_name('dependency-source-inputs.json'),json.dumps(deps,indent=2))
    # Preserve all notices listed by the complete dependency inventory and repo.
    for name,path in notices:
        dst=safe_target(out,name);dst.parent.mkdir(parents=True,exist_ok=True);a.stream_copy(path,safe_target(out,name))
    need((out/'licenses/LICENSE.txt').is_file(),'Orca source license absent')
    text=(repo/'deps_src/nlohmann/json.hpp').read_text(encoding='utf8').split('*/',1)[0]+'*/\n'
    safe_target(out,'licenses/nlohmann-json-LICENSE.MIT').write_text(text,encoding='utf8')
    for source,dest in headers:
        path=safe_target(out,dest);path.parent.mkdir(parents=True,exist_ok=True);a.stream_copy(safe_target(repo,source),safe_target(out,dest))
    archive=a.verify(plan['cgalArchive'])
    need(plan['cgalArchive']['sha256']==CGAL_SHA,'fixed CGAL archive required')
    target=safe_target(out,'dependencies/CGAL-6.2.1.zip');target.parent.mkdir(parents=True,exist_ok=True)
    a.stream_copy(archive,safe_target(out,'dependencies/CGAL-6.2.1.zip'))
    return sources


def manifest(plan, sources, static_ref):
    out=Path(plan['output'])
    files=[]
    entries=payload_files(out)
    register_names([name for name,_ in entries]+['manifest.json'])
    for name,p in entries:
        r=a.identity(p);files.append(dict(path=name,sha256=r['sha256'],sizeBytes=r['bytes']))
    return dict(schemaVersion=SCHEMA,bundleId=out.name,platformScope=['android-arm64-v8a'],
        sourceCommit=a.SOURCE,buildSourceCommit=a.SOURCE,packagingSourceCommit=a.SOURCE,toolSource=plan['toolSource'],
        sourceStatus='clean',uncommitted=False,android=dict(abi='arm64-v8a',api=24,ndk='28.2.13676358'),
        files=files,sourceFiles=sources,sourceSnapshot=hashlib.sha256(json.dumps(sources,sort_keys=True).encode()).hexdigest(),
        sourceArchive='source-snapshot.zip',producerProofs=dict(origins=plan['producers'],
            resourceSummaries=plan['resourceSummaries'],companions=plan['companions']),androidStaticProof=static_ref,
        androidModelPostprocessing=dict(proof=static_ref,library='liborca_model_sdk_c.so',operation='strip-debug',runtimeEqual=True),
        runtimeCapabilities=dict(status='pending-phone',evidence=None),functionalQualification=None,
        knownGaps=['Android runtime business behavior pending phone validation','Physical printing and affected AI/GLB flows remain unqualified'],
        locks=dict(device='unchanged',ai='unchanged',ios='unchanged'))


def execute(plan, go, plan_sha,observe=False):
    need(go.get('authorized') is True and go.get('artifactOnlyAuthorized') is True
         and go.get('allowSDKLoad') is False,'Android artifact GO required before resource admission')
    validate_life(plan,go)
    # Verify the small inert cores before importing their admission monitor.
    import artifact_owner
    refs=exact_refs(plan['toolFiles'])
    for name,digest in artifact_owner.SUPPORT.items():
        ref=refs.get(key(Path(plan['ownerDirectory'])/name))
        need(ref is not None and ref['sha256']==digest,'Android admission core pin missing')
        a.verify(ref)
        loaded=sys.modules.get(Path(name).stem)
        need(loaded is None or key(getattr(loaded,'__file__',''))==key(ref['path']),'shadowed Android core')
    sys.path.insert(0,plan['ownerDirectory'])
    from build_resources import gate,sample,heavy
    from artifact_owner import admit_cancel,run_owned
    cancel=Path(go['cancel']);admit_cancel(cancel)
    entry=[];gate(entry,lambda:None)
    preflight_samples=[];preflight_last=[0.0]
    def preflight_monitor():
        admit_cancel(cancel)
        if time.monotonic()-preflight_last[0]<5:return
        preflight_last[0]=time.monotonic();s=sample();preflight_samples.append(s)
        need(s['freeKiB']>3*1024**2 and not any(heavy(p) for p in s['processes']),'Android source admission IO gate')
    previous_preflight=a.IO_MONITOR;a.IO_MONITOR=preflight_monitor
    try:exact_go(plan,go,plan_sha,observe)
    finally:a.IO_MONITOR=previous_preflight
    evidence=Path(plan['evidence']);evidence.mkdir(parents=True)
    state=dict(state='started',commands=[],entry=entry,sourceAdmissionSamples=preflight_samples,ioSamples=[])
    def save():(evidence/'execution.json').write_text(json.dumps(state,indent=2),encoding='utf8')
    last=[0.0]
    def monitor(force=False):
        admit_cancel(cancel)
        if not force and time.monotonic()-last[0]<5:return
        last[0]=time.monotonic();s=sample();state['ioSamples'].append(s);save()
        need(s['freeKiB']>3*1024**2 and not any(heavy(p) for p in s['processes']),'Android IO resource gate')
    def command(argv,label):
        admit_cancel(cancel);source_check(plan);fresh(go)
        step=dict(argv=argv,label=label);state['commands'].append(step);gate(step.setdefault('admission',[]),save)
        path=evidence/(label+'.log')
        run_owned(argv,path,step,save,cancel,lambda:None,timeout=7200,mode='artifact',
            permitted_commands=commands(plan),permitted_images=[plan['strip'],plan['readelf']],env=dict(os.environ),
            cwd=plan['sdkRepository'],lifecycle_profile=PROFILE,android_plan=plan)
        return path.read_text(encoding='utf8')
    previous=a.IO_MONITOR;a.IO_MONITOR=monitor
    try:
        if observe:
            observe_inputs(plan,evidence,monitor)
            source_check(plan);monitor(True)
            state['state']='android_inputs_observed_not_packaged';save()
            return
        # Full streamed source closure and approved TBB/header validation before any copy.
        validate(plan,verify_all=True);source_check(plan)
        proof=process_libraries(plan,command,monitor)
        a.write(evidence/'android-static-proof.json',proof)
        sources=snapshot(plan,monitor)
        provenance=Path(plan['output'])/'provenance';provenance.mkdir()
        a.write(provenance/'binding.json',plan)
        for n,ref in enumerate(exact_refs(list(pins(plan['producers']))+list(pins(plan['companions']))).values()):
            a.stream_copy(a.verify(ref),provenance/(str(n)+'-'+Path(ref['path']).name))
        a.stream_copy(evidence/'android-static-proof.json',provenance/'android-static-proof.json')
        static_ref=dict(path='provenance/android-static-proof.json',**{k:v for k,v in a.identity(provenance/'android-static-proof.json').items() if k!='path'})
        a.write(safe_target(plan['output'],'manifest.json'),manifest(plan,sources,static_ref))
        bundle=evidence/(Path(plan['output']).name+'.zip')
        write_bundle(plan['output'],bundle)
        validate_originals=lambda:[original_graph(plan,s,verify_all=True) for s in STAGES]
        validate_originals();source_check(plan);monitor(True)
        state['state']='android_candidate_phone_pending';save()
        a.publish_result(evidence,dict(state=state['state'],manifest=a.identity(Path(plan['output'])/'manifest.json'),archive=a.identity(bundle)),
                         lambda:(validate_originals(),source_check(plan),monitor(True)),save)
    except BaseException as ex:
        state.update(state='failed_partial_no_retry',error=repr(ex));save();raise
    finally:a.IO_MONITOR=previous


def observe_inputs(plan,evidence,monitor):
    """Append actual first observations of the existing two successful producers.

    The original graphs are preserved; no build or native tool is invoked.
    """
    need(set(plan['observationRequests'])==set(STAGES) and set(plan['sourceAppendices'])==set(STAGES),
         'exact Android observation stages required')
    for stage in STAGES:
        item,source,refs=original_graph(plan,stage,verify_all=True)
        appendix=plan['sourceAppendices'][stage]
        refs += [appendix]+companion.appendix(dict(sourceAppendix=appendix),item,stage)
        before=list(exact_refs(refs).values())
        for ref in before:a.verify(ref)
        entries=[]
        requests=plan['observationRequests'][stage]
        need([x['role'] for x in requests]==['cache','approved-binding'],'only exact Android cache/binding observation')
        for request in requests:
            need(request['stage']==stage,'observation stage differs')
            current=a.identity(request['producerOutputPath'])
            entry=dict(request,current=current,firstObservedUtc=dt.datetime.now(dt.timezone.utc).isoformat(),
                       evidenceTiming=companion.TIMING)
            entries.append(entry)
        receipt=dict(schema=companion.SCHEMA,origin=companion.origin(item,stage),sourceAppendix=appendix,
            evidenceTiming=companion.TIMING,driftBefore=before,driftAfter=before,companions=entries)
        companion.validate(receipt,item,stage)
        for ref in before:a.verify(ref)
        for entry in entries:a.verify(entry['current'])
        source_check(plan);monitor(True)
        out=Path(evidence)/stage;out.mkdir()
        a.write(out/'receipt.json',receipt)
        a.write(out/'seal.json',dict(schema='b5f-companion-seal-v1',receipt=a.identity(out/'receipt.json')))


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--binding',required=True);p.add_argument('--binding-sha256',required=True)
    p.add_argument('--go');p.add_argument('--go-sha256')
    p.add_argument('--observe-inputs',action='store_true')
    args=p.parse_args();need(a.identity(args.binding)['sha256']==args.binding_sha256,'Android binding SHA mismatch')
    plan=a.small_json(args.binding)
    if args.go:
        need(a.identity(args.go)['sha256']==args.go_sha256,'Android GO SHA mismatch')
        execute(plan,a.small_json(args.go),args.binding_sha256,args.observe_inputs)
    else:
        need(plan['schema']==PLAN and set(plan['producers'])==set(STAGES),'Android two-stage plan required')
        print('ANDROID_METADATA_ONLY_NO_EXECUTION; COMPLETE_CLOSURE_AND_NEW_GO_REQUIRED')


if __name__=='__main__':main()
