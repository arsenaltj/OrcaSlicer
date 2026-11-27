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
import math
import os
import re
import subprocess
import sys
import time
import unicodedata
import zipfile
import adapter as a
import companion
import artifact_owner as resource_owner
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
 'model-android': dict(seal='eb9f643fb24fae66623f2ba18db77e31bf51a57c4c13ac0cb3162cfd8f24f1f8',
   originSha='f2b73e28e49d3df24d31192defc0e1c0524b9ec817f43db7cff8eb15c1facf39',
   rawSha='07341b8047de06eb77ceae89dbb5dbc1fa07f48686c0d9ad2de298fa5c50b39f',rawBytes=81379184),
 'slicing-android': dict(seal='eb9f643fb24fae66623f2ba18db77e31bf51a57c4c13ac0cb3162cfd8f24f1f8',
   originSha='6ad55d970ab5dddfecb5011ef51751cf65acaeb652b89bcbbf5368b486991ef1',
   rawSha='5c5fd410c04678c9a30d955174a55151e0ebb477987663ab5c4552566689a644',rawBytes=704433912)}

HERE = Path(__file__).resolve().parent
need = a.require
RECEIPT_LIMIT = 16*1024*1024
MANIFEST_LIMIT = 32*1024*1024


def typed_json(value):
    """Canonical JSON values with bool/int/float and array order preserved."""
    def check(obj):
        kind=type(obj)
        need(kind in (dict,list,str,int,float,bool,type(None)),'non-JSON value')
        if kind is dict:
            need(all(type(k) is str for k in obj),'non-string JSON key')
            for child in obj.values():check(child)
        elif kind is list:
            for child in obj:check(child)
        elif kind is float:need(math.isfinite(obj),'nonfinite JSON number')
    check(value)
    return json.dumps(value,sort_keys=True,ensure_ascii=False,allow_nan=False,separators=(',',':'))


def typed_equal(left,right):return typed_json(left)==typed_json(right)


def typed_member(value,values):
    target=typed_json(value)
    return any(typed_json(other)==target for other in values)


def typed_contains(value,target):
    if type(value) is dict:
        return (all(k in value and typed_equal(value[k],v) for k,v in target.items())
                or any(typed_contains(child,target) for child in value.values()))
    return type(value) is list and any(typed_contains(child,target) for child in value)


def role_shape(value,role,stage=None,producer=None):
    typed_json(value)
    need(type(value) is dict,'JSON role object required')
    if role=='receipt':
        need(value.get('schema')==companion.SCHEMA and type(value.get('origin')) is dict
             and value['origin'].get('stage')==stage and stage in STAGES,'wrong Android receipt role/stage')
        if producer is not None:
            need(typed_equal(value['origin'],companion.origin(producer,stage)),'typed receipt origin differs')
    elif role=='manifest':
        need(value.get('schemaVersion')==SCHEMA and type(value.get('files')) is list,'wrong Android manifest role')
    elif role=='evidence-index':
        need(value.get('schema')=='android-provenance-index-v1' and type(value.get('entries')) is list,
             'wrong Android evidence index role')
    else:need(False,'unknown Android JSON role')


def role_bytes(value,role,stage=None,producer=None):
    role_shape(value,role,stage,producer)
    data=(json.dumps(value,indent=2,ensure_ascii=False,allow_nan=False)+'\n').encode('utf8')
    need(len(data)<=(MANIFEST_LIMIT if role=='manifest' else RECEIPT_LIMIT),'Android JSON role size limit')
    return data


def read_role(ref,role,stage=None,producer=None):
    need(role in ('receipt','manifest','evidence-index'),'unknown Android JSON role')
    identity=next(iter(exact_refs([ref]).values()))
    path=Path(ref['path'])
    need(safe_target(path.parent,path.name)==path.absolute(),'Android JSON role path changed')
    need(path.name=={'receipt':'receipt.json','manifest':'manifest.json','evidence-index':'evidence-index.json'}[role],
         'wrong Android JSON role filename')
    limit=MANIFEST_LIMIT if role=='manifest' else RECEIPT_LIMIT
    need(0<identity['bytes']<=limit and path.is_file() and not path.is_symlink(),'Android JSON role bound/file')
    need(path.stat().st_size==identity['bytes'],'Android JSON role declared length differs')
    digest=hashlib.sha256();blocks=[];size=0
    with path.open('rb') as stream:
        while block:=stream.read(min(1024*1024,limit+1-size)):
            a.IO_MONITOR();size+=len(block);need(size<=limit,'Android JSON role size limit')
            digest.update(block);blocks.append(block)
    need(size==identity['bytes'] and digest.hexdigest()==identity['sha256'],'Android JSON role bytes/SHA differ')
    def unique(pairs):
        obj={}
        for k,v in pairs:
            need(k not in obj,'duplicate JSON object key');obj[k]=v
        return obj
    value=json.loads(b''.join(blocks).decode('utf-8-sig'),object_pairs_hook=unique,
                     parse_constant=lambda text:(_ for _ in ()).throw(ValueError('nonfinite JSON number')))
    role_shape(value,role,stage,producer)
    return value


def write_role(path,value,role,stage=None,producer=None):
    data=role_bytes(value,role,stage,producer) # enforce before the first output write
    target=safe_target(Path(path).parent,Path(path).name)
    need(target.name=={'receipt':'receipt.json','manifest':'manifest.json','evidence-index':'evidence-index.json'}[role],
         'wrong Android JSON role filename')
    with target.open('xb') as out:out.write(data)


def key(path):return str(Path(path).resolve())


def pins(value):
    if isinstance(value,dict):
        if set(('path','bytes','sha256')) <= value.keys():yield value
        for child in value.values():yield from pins(child)
    elif isinstance(value,list):
        for child in value:yield from pins(child)


def exact_refs(refs):
    """Unique physical identities, independent of original provenance annotations."""
    out = {}
    for ref in refs:
        need(isinstance(ref,dict) and isinstance(ref.get('path'),str) and Path(ref['path']).is_absolute()
             and type(ref.get('bytes')) is int and ref['bytes'] >= 0
             and isinstance(ref.get('sha256'),str)
             and re.fullmatch('[0-9a-f]{64}',ref['sha256']), 'malformed Android pin')
        k = key(ref['path'])
        identity=dict(path=k,bytes=ref['bytes'],sha256=ref['sha256'])
        need(k not in out or out[k] == identity, 'conflicting Android identity')
        out[k] = identity
    return out


def original_refs(refs):
    """Keep every original dictionary shape after checking physical conflicts.

    Only byte-for-byte JSON-equivalent duplicate records are omitted. This list
    is for strict companion drift membership, never the unique GO pin index.
    """
    exact_refs(refs)
    seen=set();result=[]
    for ref in refs:
        shape=typed_json(ref)
        if shape not in seen:
            seen.add(shape);result.append(ref)
    return result


def life(plan, enabled=False):
    policy=plan.get('resourcePolicy');memory=resource_owner.memory_only(policy)
    result=dict(schema='android-artifact-lifecycle-v3' if memory else 'android-artifact-lifecycle-v2',enabled=enabled,profile=PROFILE,timingPolicy=timing_policy(),
        effectiveCreationFlags=FLAGS,neverResumedAbortExitCode=74,custodian='CURRENT_CREATING_PROCESS',
        source=a.SOURCE,toolSource=plan['toolSource'],cwd=str(Path(plan['sdkRepository'])),
        commandTimeoutSeconds=7200,commands=commands(plan),toolFiles=plan['toolFiles'])
    if memory:result['resourcePolicy']=policy
    return result


def go_schema(plan):return 'android-artifact-go-v3' if resource_owner.memory_only(plan.get('resourcePolicy')) else 'android-artifact-go-v2'


def validate_life(plan, go=None):
    value = plan.get('lifecycle')
    need(isinstance(value,dict) and type(value.get('enabled')) is bool
         and all(type(value.get(k)) is int for k in ('effectiveCreationFlags','neverResumedAbortExitCode','commandTimeoutSeconds')),
         'typed Android lifecycle required')
    need(typed_equal(value,life(plan,True)), 'explicit enabled Android lifecycle required')
    if go is not None:
        need(typed_equal(go.get('lifecycle'),value), 'Android GO lifecycle differs')
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
        exact_refs([row['source']])
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
    import normal_android_producer
    if normal_android_producer.selected(item['terminal']):
        from types import SimpleNamespace
        api=SimpleNamespace(small_json=a.small_json,verify=a.verify,canonical=typed_json,
            exact_refs=exact_refs,pins=pins,original_refs=original_refs)
        result=normal_android_producer.graph(plan,stage,api,expected)
        if verify_all:
            for ref in exact_refs(result[2]).values():a.verify(ref)
        return result
    need(item is not None and item['seal']['sha256']==expected['seal'], 'unqualified Android producer seal')
    sealed=a.small_json(a.verify(item['seal']))
    need(typed_equal(sealed,item['fullIndex']), 'Android seal/fullIndex differs')
    refs=sealed.get('files',sealed.get('refs'))
    need(isinstance(refs,list) and refs and len(exact_refs(refs))==len(refs), 'incomplete original index')
    def member(name):
        matches=[r for r in refs if Path(r['path']).name==name]
        need(len(matches)==1,'missing/ambiguous original '+name)
        return matches[0]
    for name,expected_value in [('actual-terminal.json',item['terminal']),('raw-source.json',item['rawSource'])]:
        need(typed_equal(a.small_json(a.verify(member(name))),expected_value),'original Android metadata substituted')
    a.terminal_ok(stage,item['terminal'],item['rawSource'])
    raw=item['rawSource']['raw']
    need(raw['sha256']==expected['rawSha'] and raw['bytes']==expected['rawBytes']
         and Path(raw['path']).name==NAMES[stage] and Path(item['rawSource']['buildPath']).name==NAMES[stage],
         'old or incorrect Android raw identity')
    origins=[r for r in refs if Path(r['path']).name=='recovery-origin.json']
    original=a.small_json(a.verify(origins[0]))['originalRecords'] if origins else {}
    need(typed_equal(original,item.get('originalRecords',{})),'original recovery records substituted')
    provenance=refs+list(original.values())
    graph=exact_refs(provenance)
    summary_ref=plan['resourceSummaries'][stage]
    need(typed_member(summary_ref,provenance),'resource summary outside original graph')
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
    all_refs=original_refs(provenance+list(pins(source))+list(pins(inventory))+[raw,
        dict(raw,path=item['rawSource']['buildPath']),item['seal']])
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


def appendix_records(ref,item,stage):
    data=a.small_json(a.verify(ref))
    need(data.get('schema')=='b5f-companion-source-appendix-v1'
         and typed_equal(data['origin'],companion.origin(item,stage))
         and data['evidenceTiming']==companion.TIMING,'typed appendix origin/timing differs')
    need(type(data.get('entries')) is list and all(type(row) is dict
         and type(row.get('firstObservedUtc')) is str and row['firstObservedUtc'] for row in data['entries']),
         'appendix observation string required')
    # Keep all existing role/path constraints in the unchanged validator.
    return companion.appendix(dict(sourceAppendix=ref),item,stage)


def typed_receipt_guard(plan,stage,receipt,source=None):
    item=plan['producers'][stage]
    need(typed_equal(receipt['origin'],companion.origin(item,stage)),'typed receipt origin differs')
    need(typed_equal(a.small_json(a.verify(item['seal'])),item['fullIndex']),'typed companion original index differs')
    before=receipt['driftBefore']
    need(type(before) is list and before and typed_equal(before,receipt['driftAfter']),
         'typed drift before/after differs')
    exact_refs(before)
    index=companion.index(item)
    origin=next((r for r in index if Path(r['path']).name=='recovery-origin.json'),None)
    records=a.small_json(a.verify(origin)).get('originalRecords',{}) if origin else {}
    need(typed_equal(records,item.get('originalRecords',{})),'typed companion original records differ')
    if source is None:
        source=a.small_json(a.verify(next(r for r in index if Path(r['path']).name=='actual-compiler-inputs.json')))
    objects=a.small_json(a.verify(next(r for r in index if Path(r['path']).name=='new-object-inventory.json')))
    required=index+[item['seal'],item['rawSource']['raw']]+list(pins(source))+list(pins(objects))
    extra=[]
    if receipt.get('sourceAppendix'):
        expected=plan.get('sourceAppendices',{}).get(stage)
        need(expected is not None and typed_equal(receipt['sourceAppendix'],expected),'typed appendix reference differs')
        extra=appendix_records(receipt['sourceAppendix'],item,stage)
        required += [receipt['sourceAppendix']]+extra
    covered={typed_json(r) for r in before}
    need(all(typed_json(ref) in covered for ref in required),'typed original source/object/appendix coverage missing')
    entries=receipt['companions'];requests=plan['observationRequests'][stage]
    need(type(entries) is list and len(entries)==len(requests),'typed observation request count differs')
    for entry,request in zip(entries,requests):
        need(set(entry)==set(request)|{'current','firstObservedUtc','evidenceTiming'}
             and all(k in entry and typed_equal(entry[k],v) for k,v in request.items()),
             'typed observation basis/request differs')
        need(type(entry['firstObservedUtc']) is str and entry['firstObservedUtc']
             and entry['evidenceTiming']==companion.TIMING,'typed observation timing differs')
        exact_refs([entry['current']]+list(pins(entry['basis'])))
        for name in ('normalPlan','approvedRule','approvedVerifier','headerProof'):
            if name in entry['basis']:
                ref=entry['basis'][name]
                need(typed_member(ref,index) or typed_contains(source,ref) or typed_member(ref,extra),
                     'typed historical basis outside original/appendix graph')
        if entry['role']=='approved-binding':
            need(typed_equal(entry['current'],entry['basis']['binding']),'typed current approved binding differs')
            basis=entry['basis'];binding=a.small_json(a.verify(basis['binding']))
            rule=a.small_json(a.verify(basis['approvedRule']))
            expected_binding=dict(schemaVersion=1,runtimePath=str(Path(binding['runtimePath']).resolve()),
                headerRoot=str(Path(binding['headerRoot']).resolve()),approvalManifestSha256=basis['approvedRule']['sha256'],
                runtime=rule['runtime'],upstream=rule['upstream'],headerCount=len(rule['headers']),
                headersSha256=hashlib.sha256(json.dumps(rule['headers'],sort_keys=True).encode()).hexdigest(),
                runtimeBuiltByThisProducer=False)
            need(typed_equal(binding,expected_binding),'typed approved binding metadata differs')
            headers=a.small_json(a.verify(basis['headerProof']))
            need(typed_equal(headers['headers'],rule['headers']),'typed approved header inventory differs')
    return extra


def companion_receipt(plan,stage,source=None):
    pair=plan['companions'][stage]
    need(pair is not None,'Android cache/approved-binding companion unbound')
    seal=a.small_json(a.verify(pair['seal']))
    need(seal.get('schema')=='b5f-companion-seal-v1' and typed_equal(seal['receipt'],pair['receipt']),
         'typed companion receipt/seal differs')
    receipt=read_role(pair['receipt'],'receipt',stage,plan['producers'][stage])
    extra=typed_receipt_guard(plan,stage,receipt,source)
    return receipt,extra


def companion_closure(plan,stage):
    receipt,extra=companion_receipt(plan,stage)
    pair=plan['companions'][stage]
    refs=[pair['seal'],pair['receipt']]+receipt['driftBefore']
    if receipt.get('sourceAppendix'):refs += [receipt['sourceAppendix']]+extra
    refs += list(pins(receipt['companions']))
    return refs


def support(plan, stage, source, *, required=True):
    pair=plan.get('companions',{}).get(stage)
    if not required and pair is None:return []
    need(pair is not None,'Android cache/approved-binding companion unbound')
    receipt,_=companion_receipt(plan,stage,source)
    companion.validate(receipt,plan['producers'][stage],stage)
    for ref in exact_refs(receipt['driftBefore']).values():a.verify(ref)
    return [entry['current'] for entry in receipt['companions']]


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
            closure += companion_closure(plan,stage)
    for stage,name in NAMES.items():
        need(typed_equal(plan['runtimeInputs'][name],dict(stage=stage,source=plan['producers'][stage]['rawSource']['raw'])),
             'canonical Android raw must match qualified producer')
    for name,row in runtime_inputs(plan).items():
        if name in NAMES.values():continue
        item,source,extra=allowed[row['stage']]
        need(typed_contains(source,row['source']) or typed_member(row['source'],extra),'runtime outside exact source/companion graph')
    tbb=plan['runtimeInputs']['libtbb.so']['source']
    need(all(tbb[k]==v for k,v in TBB.items()),'approved TBB original bytes required')
    return list(exact_refs(closure).values())


def tool_refs(plan):
    refs=exact_refs(plan['toolFiles'])
    need(len(refs)==len(plan['toolFiles']),'duplicate Android tool pins')
    local=['package_android.py','execution_journal.py','android_elf.py','adapter.py','companion.py','artifact_owner.py','api_context.py',
           'suspended_root.py','suspended_launch.py','normal_android_producer.py']
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
             and typed_equal(receipt['toolFiles'],plan['toolFiles'])
             and typed_equal(receipt['lifecycle'],plan['lifecycle'])
             and typed_equal(receipt['inputs'],input_contract(plan)),
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


def timing_policy():
    return dict(schema='android-dispatch-io-deadline-v1',entryFreshnessMaxSeconds=360,
        operationIssueBudgetMaxSeconds=7200,deadlineEvent='command-dispatch-and-io-publication-admission',
        clockPolicy='utc-and-once-derived-monotonic',renewalAllowed=False,
        childCompletionDeadlineEnforced=False,hostReleaseDeadlineEnforced=False)


def utc_now():return dt.datetime.now(dt.timezone.utc)


def utc_value(value):
    need(type(value) is str and value.endswith('Z'),'canonical aware UTC string required')
    parsed=dt.datetime.fromisoformat(value[:-1]+'+00:00')
    need(parsed.isoformat().replace('+00:00','Z')==value,'noncanonical UTC time')
    return parsed


def fresh(go, *, now=None):
    now=utc_now() if now is None else now
    issued=utc_value(go['issuedUtc']);expiry=utc_value(go['expiresUtc'])
    need(issued<=now<expiry and 0<(expiry-issued).total_seconds()<=360,'Android GO expired')


class OperationBudget:
    """One nondurable envelope admission; never a child-completion deadline."""
    FIELDS=('issuedUtc','expiresUtc','operationDeadlineUtc','resourceAdmissionWindowEndUtc')
    def __init__(self,go):
        need(all(k in go for k in self.FIELDS),'Android timing fields required')
        self.entryUtc=utc_now();self.entryMono=time.monotonic()
        fresh(go,now=self.entryUtc)
        memory=resource_owner.memory_only(go['lifecycle'].get('resourcePolicy'))
        need(go.get('schema')==('android-artifact-go-v3' if memory else 'android-artifact-go-v2'),'versioned Android timing GO required')
        need(typed_equal(go['lifecycle']['timingPolicy'],timing_policy()),'typed Android timing policy differs')
        self.go=go;self.bound={k:go[k] for k in self.FIELDS}
        self.policy=typed_json(go['lifecycle']['timingPolicy'])
        self.resource=typed_json(go['lifecycle'].get('resourcePolicy'))
        issued=utc_value(go['issuedUtc']);deadline=utc_value(go['operationDeadlineUtc'])
        resource_end=utc_value(go['resourceAdmissionWindowEndUtc'])
        need(self.entryUtc<deadline<=min(issued+dt.timedelta(seconds=7200),resource_end),
             'Android operation deadline outside issue/resource window')
        self.deadline=deadline
        self.monoDeadline=self.entryMono+(deadline-self.entryUtc).total_seconds()
        self.check()

    def check(self):
        need(typed_equal({k:self.go[k] for k in self.FIELDS},self.bound)
             and typed_json(self.go['lifecycle']['timingPolicy'])==self.policy
             and typed_json(self.go['lifecycle'].get('resourcePolicy'))==self.resource,'Android timing/resource authority changed; no renewal')
        need(utc_now()<self.deadline and time.monotonic()<self.monoDeadline,'Android operation deadline elapsed')

    def receipt(self):
        return dict(entryUtc=self.entryUtc.isoformat().replace('+00:00','Z'),
            entryMonotonic=self.entryMono,monotonicDeadline=self.monoDeadline,**self.bound,
            earlyAdmissionDurable=False,earlyPhase='envelope_admitted_source_verification_pending',
            fullSourceAdmissionCompleted=True,timingPolicy=timing_policy())


def source_check(plan):
    for repo,commit,tree in [(Path(plan['sdkRepository']),a.SOURCE,a.TREE),(HERE.parents[1],plan['toolSource'],None)]:
        def git(*argv):return subprocess.check_output([plan['git'],'-C',str(repo),*argv],text=True,env=dict(os.environ),cwd=repo).strip()
        need(git('rev-parse','HEAD')==commit and not git('status','--porcelain'),'Android source/tool dirty or changed')
        if tree:need(git('rev-parse','HEAD^{tree}')==tree,'SDK tree changed')


def exact_go(plan,go,plan_sha,observe=False,admission=None):
    need(go.get('schema')==go_schema(plan) and go.get('authorized') is True
         and type(go['authorized']) is bool and go.get('rootConversation')==ROOT
         and go.get('artifactOnlyAuthorized') is True and go.get('allowSDKLoad') is False,
         'explicit Android artifact-only root GO required')
    budget=OperationBudget(go) if admission is None else admission
    need(type(budget) is OperationBudget and budget.go is go,'different Android admission token')
    budget.check()
    need(go.get('operation')==('observe-inputs' if observe else 'package-android'), 'Android GO operation differs')
    need(go['planSha256']==plan_sha and a.identity(go['planPath'])['sha256']==plan_sha
         and key(plan['bindingPath'])==key(go['planPath']) and typed_equal(a.small_json(go['planPath']),plan),'Android exact plan mismatch')
    need(key(go['cancel'])==key(Path(plan['evidence']).with_suffix('.cancel')),'dedicated Android cancellation channel required')
    validate_life(plan,go)
    closure=validate(plan,companions=not observe,verify_all=True)+tool_refs(plan)+list(pins(plan['runtimeInputs']))
    if observe:
        for stage in STAGES:
            appendix=plan['sourceAppendices'][stage]
            closure += [appendix]+appendix_records(appendix,plan['producers'][stage],stage)
        closure += list(pins(plan['observationRequests']))
    closure += list(plan['qualification'].values())
    closure += [a.small_json(a.verify(r))['report'] for r in plan['qualification'].values()]
    expected=exact_refs(closure);actual=exact_refs(go['closure'])
    need(len(actual)==len(go['closure']) and actual==expected,'exact Android GO closure differs')
    for ref in actual.values():a.verify(ref)
    source_check(plan)
    budget.check()


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
    root=Path(plan['output']);safe_target(root.parent,root.name).mkdir(parents=True)
    out=root/'android/arm64-v8a';out.mkdir(parents=True)
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
        need(typed_equal(before,after),'ALLOC/PT_LOAD/loader/runtime bytes changed')
        if kind=='model':need(not released_debug,'Model debug sections retained')
        if kind:need(source_marker(released,a.SOURCE,monitor),'Android release source marker missing')
        second=dynamic_tables(lambda flags,label:command([plan['readelf'],*flags],label),released,name+'-release',kind)
        need(typed_equal(first,second),'NEEDED/dynsym changed after strip')
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
    data=read_role(a.identity(safe_target(out,'manifest.json')),'manifest')
    declared=data['files']
    names=[row['path'] for row in declared]+['manifest.json']
    register_names(names)
    actual=payload_files(out)
    need({name for name,_ in actual}==set(names),'final Android archive file set differs')
    for row in declared:
        need(type(row['sizeBytes']) is int and row['sizeBytes']>=0,'invalid manifest file size')
        a.verify(dict(path=str(safe_target(out,row['path'])),bytes=row['sizeBytes'],sha256=row['sha256']))
    destination=safe_target(Path(bundle).parent,Path(bundle).name)
    need(not destination.resolve().is_relative_to(out.resolve()),'final bundle archive overlaps payload root')
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
        receipt=read_role(plan['companions'][stage]['receipt'],'receipt',stage,plan.get('producers',{}).get(stage))
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
            need(typed_equal(a.identity(path),before),'snapshot source changed')
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


def provenance_index(plan,closure_refs=()):
    """Complete original references, with a separate bounded distribution policy."""
    occurrences=list(pins(plan['producers']))+list(pins(plan.get('companions',{})))+list(closure_refs)
    roles={}
    def assign(ref,role):roles.setdefault(key(ref['path']),set()).add(role)
    producer_roles={'actual-terminal.json':'producer-terminal','raw-source.json':'producer-raw-reference',
        'recovery-origin.json':'producer-recovery-origin','actual-compiler-inputs.json':'compiler-input-metadata',
        'actual-source-command-flags.json':'compiler-command-metadata','new-object-inventory.json':'object-inventory-metadata',
        'clean-origin.json':'producer-clean-origin','admission.json':'producer-admission','exact-argv.json':'producer-argv'}
    for stage,item in plan['producers'].items():
        _,_,refs=original_graph(plan,stage,verify_all=False);occurrences+=refs
        assign(item['seal'],'producer-seal')
        for ref in companion.index(item):
            if Path(ref['path']).name in producer_roles:assign(ref,producer_roles[Path(ref['path']).name])
        pair=plan.get('companions',{}).get(stage)
        if pair is not None:
            receipt,_=companion_receipt(plan,stage)
            occurrences+=list(pins(receipt))+[pair['receipt'],pair['seal']]
            assign(pair['receipt'],'companion-receipt');assign(pair['seal'],'companion-seal')
            if receipt.get('sourceAppendix'):
                ref=receipt['sourceAppendix'];assign(ref,'source-appendix')
                appendix=a.small_json(a.verify(ref))
                for row in appendix['entries']:
                    if row['role'] in {'normal-plan','approved-rule','header-manifest'}:assign(row['pin'],row['role'])
            for entry in receipt['companions']:
                if entry['role'] in {'cache','approved-binding'}:assign(entry['current'],entry['role'])
    physical=exact_refs(occurrences);records={}
    for ref in original_refs(occurrences):records.setdefault(key(ref['path']),[]).append(ref)
    entries=[]
    for n,(path,identity) in enumerate(sorted(physical.items())):
        names=sorted(roles.get(path,set()));name=Path(path).name
        forbidden=(name.lower()=='execution.json' or Path(path).suffix.lower() in
                   {'.so','.dll','.exe','.obj','.o','.pch','.lib','.a','.log','.tlog','.pdb'})
        copied=bool(names) and not forbidden and 0<identity['bytes']<=RECEIPT_LIMIT
        reason=('explicit small metadata role within16MiB' if copied else
                ('raw/execution/object/tool/log remains original external evidence' if forbidden else
                 ('metadata exceeds16MiB distribution budget' if names else 'no declared distribution role')))
        entries.append(dict(identity=identity,originalRefs=records[path],roles=names,
            distribution='distributed-copy' if copied else 'external-only',reason=reason,
            payloadPath='provenance/records/'+str(n)+'-'+name if copied else None))
    return dict(schema='android-provenance-index-v1',entries=entries,
        originalsPreservedExternally=True,allOriginalRefsRetained=True,
        distributionScope='Original evidence copies under provenance/records; release/source payloads are independently listed in manifest/static proof',
        sourceVerification='All original/external references still require full actual pre/post verification; distribution is not qualification')


def distribute_provenance(plan,closure_refs=()):
    out=Path(plan['output']);index=provenance_index(plan,closure_refs)
    role_bytes(index,'evidence-index')
    paths=[entry['payloadPath'] for entry in index['entries'] if entry['distribution']=='distributed-copy']
    register_names(paths+['provenance/evidence-index.json'])
    for name in paths:safe_target(out,name)
    for entry in index['entries']:
        if entry['distribution']!='distributed-copy':continue
        source=a.verify(entry['identity']);destination=safe_target(out,entry['payloadPath'])
        destination.parent.mkdir(parents=True,exist_ok=True)
        a.stream_copy(source,safe_target(out,entry['payloadPath']))
        copied=a.identity(destination)
        need(typed_equal({k:copied[k] for k in ('bytes','sha256')},
                         {k:entry['identity'][k] for k in ('bytes','sha256')}),'distributed metadata bytes differ')
        a.verify(entry['identity'])
    target=safe_target(out,'provenance/evidence-index.json');target.parent.mkdir(parents=True,exist_ok=True)
    write_role(target,index,'evidence-index')
    ref=a.identity(target)
    return dict(path='provenance/evidence-index.json',bytes=ref['bytes'],sha256=ref['sha256'])


def verify_closure(refs):
    """Physical verification includes external-only evidence; never a payload filter."""
    for ref in exact_refs(refs).values():a.verify(ref)


def execute(plan, go, plan_sha,observe=False):
    need(go.get('schema')==go_schema(plan) and go.get('authorized') is True and go.get('artifactOnlyAuthorized') is True
         and go.get('allowSDKLoad') is False and go.get('rootConversation')==ROOT
         and go.get('operation')==('observe-inputs' if observe else 'package-android'),
         'Android artifact GO required before resource admission')
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
    policy=plan.get('resourcePolicy')
    def admission(rows,save):return resource_owner.resource_admission(rows,save,policy,gate,sample,heavy)
    entry=[];admission(entry,lambda:None)
    budget=OperationBudget(go) # capture once AFTER resource wait, before full source verification
    preflight_samples=[];preflight_last=[0.0]
    def preflight_monitor(force=False):
        admit_cancel(cancel);budget.check()
        if not force and time.monotonic()-preflight_last[0]<5:return
        preflight_last[0]=time.monotonic();s=sample();preflight_samples.append(s)
        need(resource_owner.resource_available(s,policy,heavy),'Android source admission IO gate')
        budget.check()
    previous_preflight=a.IO_MONITOR;a.IO_MONITOR=preflight_monitor
    try:
        exact_go(plan,go,plan_sha,observe,admission=budget)
        if not observe:
            # Both complete source passes require all three roots still absent.
            preflight_monitor(True);validate(plan,verify_all=True);source_check(plan);preflight_monitor(True)
        check_roots(plan);preflight_monitor(True)
    finally:a.IO_MONITOR=previous_preflight
    budget.check();admit_cancel(cancel)
    evidence=Path(plan['evidence']);safe_target(evidence.parent,evidence.name).mkdir(parents=True)
    state=dict(state='started',commands=[],entry=entry,sourceAdmissionSamples=preflight_samples,ioSamples=[],
               timingAdmission=budget.receipt())
    from execution_journal import ExecutionJournal
    journal=ExecutionJournal(evidence)
    def save():journal.save(state)
    last=[0.0]
    def monitor(force=False):
        admit_cancel(cancel);budget.check()
        if not force and time.monotonic()-last[0]<5:return
        last[0]=time.monotonic();s=sample();state['ioSamples'].append(s);save()
        need(resource_owner.resource_available(s,policy,heavy),'Android IO resource gate')
        budget.check()
    def command(argv,label):
        admit_cancel(cancel);budget.check();source_check(plan);budget.check()
        step=dict(argv=argv,label=label);state['commands'].append(step);admission(step.setdefault('admission',[]),save)
        path=evidence/(label+'.log')
        admit_cancel(cancel);budget.check()
        step['dispatchAdmissionUtc']=utc_now().isoformat().replace('+00:00','Z')
        resource_kwargs=dict(resource_policy=policy) if resource_owner.memory_only(policy) else {}
        run_owned(argv,path,step,save,cancel,lambda:None,timeout=7200,mode='artifact',
            permitted_commands=commands(plan),permitted_images=[plan['strip'],plan['readelf']],env=dict(os.environ),
            cwd=plan['sdkRepository'],lifecycle_profile=PROFILE,android_plan=plan,**resource_kwargs)
        journal.command_finished(state)
        admit_cancel(cancel);budget.check()
        result=path.read_text(encoding='utf8');budget.check();return result
    previous=a.IO_MONITOR;a.IO_MONITOR=monitor
    try:
        if observe:
            observe_inputs(plan,evidence,monitor)
            verify_closure(go['closure'])
            source_check(plan);monitor(True)
            state['state']='android_inputs_observed_not_packaged';save()
            journal.finish(state)
            admit_cancel(cancel);budget.check()
            return
        # Both full source passes completed before exclusive evidence publication.
        monitor(True)
        proof=process_libraries(plan,command,monitor)
        monitor(True)
        a.write(evidence/'android-static-proof.json',proof)
        sources=snapshot(plan,monitor)
        monitor(True)
        provenance=Path(plan['output'])/'provenance';provenance.mkdir()
        a.write(provenance/'binding.json',plan)
        index_ref=distribute_provenance(plan,go['closure'])
        a.stream_copy(evidence/'android-static-proof.json',provenance/'android-static-proof.json')
        static_ref=dict(path='provenance/android-static-proof.json',**{k:v for k,v in a.identity(provenance/'android-static-proof.json').items() if k!='path'})
        final_manifest=manifest(plan,sources,static_ref)
        final_manifest['provenanceIndex']=index_ref
        write_role(safe_target(plan['output'],'manifest.json'),final_manifest,'manifest')
        monitor(True)
        bundle=evidence/(Path(plan['output']).name+'.zip')
        write_bundle(plan['output'],bundle)
        monitor(True)
        validate_originals=lambda:([original_graph(plan,s,verify_all=True) for s in STAGES],verify_closure(go['closure']))
        validate_originals();source_check(plan);monitor(True)
        state['state']='android_candidate_phone_pending';save()
        a.publish_result(evidence,dict(state=state['state'],manifest=a.identity(Path(plan['output'])/'manifest.json'),archive=a.identity(bundle)),
                         lambda:(monitor(True),validate_originals(),source_check(plan),monitor(True)),
                         lambda:(save(),admit_cancel(cancel),budget.check(),journal.finish(state),admit_cancel(cancel),budget.check()))
    except BaseException as ex:
        state.update(state='failed_partial_no_retry',error=repr(ex))
        try:journal.finish(state)
        except BaseException as persistence_error:
            state['journalPersistenceError']=repr(persistence_error)
            if not journal.terminal['attempted']:
                try:journal.terminate()
                except BaseException as cleanup_error:state['journalCleanupError']=repr(cleanup_error)
            state['journalTerminal']=dict(journal.terminal)
            state['finalReceiptTerminal']=journal.final_terminal
            state['journalReconstructionComplete']=False
            state['journalRecovery']=journal.recovery(state)
            ex.add_note('execution persistence failed: '+repr(persistence_error))
            try:journal.write_final(state)
            except BaseException as final_error:ex.add_note('failure receipt unavailable: '+repr(final_error))
        raise
    finally:
        a.IO_MONITOR=previous
        first_error=sys.exc_info()[1]
        try:
            if not journal.terminal['attempted']:journal.terminate()
        except BaseException as close_error:
            if first_error is None:raise
            first_error.add_note('journal close failed: '+repr(close_error))


def observe_inputs(plan,evidence,monitor):
    """Append actual first observations of the existing two successful producers.

    The original graphs are preserved; no build or native tool is invoked.
    """
    need(set(plan['observationRequests'])==set(STAGES) and set(plan['sourceAppendices'])==set(STAGES),
         'exact Android observation stages required')
    for stage in STAGES:
        item,source,refs=original_graph(plan,stage,verify_all=True)
        appendix=plan['sourceAppendices'][stage]
        refs += [appendix]+appendix_records(appendix,item,stage)
        before=original_refs(refs)
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
        typed_receipt_guard(plan,stage,receipt,source)
        companion.validate(receipt,item,stage)
        for ref in before:a.verify(ref)
        for entry in entries:a.verify(entry['current'])
        source_check(plan);monitor(True)
        out=Path(evidence)/stage;out.mkdir()
        write_role(out/'receipt.json',receipt,'receipt',stage,item)
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
