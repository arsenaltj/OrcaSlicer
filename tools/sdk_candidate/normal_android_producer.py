"""Exact current-task normal incremental Android producer validation.

The historical clean/recovery producer contract remains independent.
No tool invocation, process control or SDK execution is performed here.
"""
from pathlib import Path
import re
import json

SCHEMA = 'orca-android-normal-incremental-producer-v1'
TERMINAL_SCHEMA = 'orca-android-normal-incremental-terminal-v1'
SOURCE = '0a86364f85d807ee24250789b74440b22f74c1b5'
TREE = '5c4b2e9105e066995408d989f6f8296b55b7a9b2'
GO_SHA = '2b87145cb7df65a96126728de4d9646866a60d261af88a59a0be86c43e3ce20d'
BASE = Path('D:/Codex/artifacts/task-20261124-v102-desktop-sdk-sync/evidence/normal-clean-build-b5f-v1')
CACHES = {
 'model-android': 'D:/Codex/tmp/task-20261124-v102-desktop-sdk-sync/build-b5f-clean-model-android-fix2',
 'slicing-android': 'D:/Codex/tmp/task-20261124-v102-desktop-sdk-sync/build-b5f-clean-slicing-android'}
BASELINES = {
 'model-android': ('model-android-fix2-v1','key-seal.json',
 '1e6a88d930f12e208a68347fda510ada180cb46069553966e01cd34fa328f3e7',
 'c113ce9560e4c85cb476222db87ebb04c70ee9c395ce6bd77244003168494cfd',25),
 'slicing-android': ('slicing-android-source-recovery-fix3-v1','actual-recovery-seal.json',
 '32cca4d5383acc6a6ad11e9b68d767a917493d896da2300ce45535af4eb76b81',
 'e255ce7c7aefe3b564892cf9de3155a9d9336061010ab0db3bae7e43b4a6e963',549)}


def need(ok,message):
    if not ok:raise ValueError(message)


def key(path):return str(Path(path).resolve()).replace('\\','/').lower()


def selected(terminal):
    schema=terminal.get('schema')
    need(schema is None or schema==TERMINAL_SCHEMA,'unknown normal producer schema')
    return schema==TERMINAL_SCHEMA


def validate_terminal(stage,terminal,raw):
    need(stage in CACHES and terminal.get('schema')==TERMINAL_SCHEMA
         and raw.get('schema')==SCHEMA, 'wrong normal Android stage/schema')
    need(terminal.get('source')==raw.get('source')==SOURCE and raw.get('sourceTree')==TREE,
         'wrong normal Android source/tree')
    need(raw.get('go',{}).get('sha256')==GO_SHA,'wrong normal Android GO')
    need(raw.get('cleanBuild') is False and type(raw.get('copiedPredecessorObjects')) is int
         and raw['copiedPredecessorObjects']==0,'normal Android cannot claim clean/copied objects')
    need(type(raw.get('priorObjectsReused')) is bool or raw.get('priorObjectsReused')=='UNKNOWN',
         'reuse observation must be explicit')
    need(key(Path(raw['buildPath']).parent)==key(CACHES[stage])
         and key(raw['ownedCache'])==key(CACHES[stage]),'foreign normal cache')
    need(terminal.get('sourceClean') is True and terminal.get('sdkExecuted') is False,
         'normal source unclean or SDK executed')
    need(terminal.get('naturalReturn') is True,'normal command root not naturally returned')
    need(terminal.get('cancelled') is False and type(terminal.get('memoryViolation')) is list
         and not terminal['memoryViolation'],'normal resource observation failed')
    need(type(terminal.get('minimumFreeBytes')) is int and terminal['minimumFreeBytes']>3221225472,
         'normal memory runtime floor failed')
    records=terminal.get('commands')
    need(type(records) is list and [r.get('name') for r in records]==[stage+'-configure',stage+'-build'],
         'normal configure/build family differs')
    for row in records:
        need(type(row.get('exitCode')) is int and row['exitCode']==0,
             'normal command did not return successfully')
        need(type(row.get('rootPid')) is int and row['rootPid']>0
             and type(row.get('started')) is str and row['started']
             and type(row.get('ended')) is str and row['ended'],
             'normal command identity/timing missing')
    return True


def inventory(rows,root):
    need(type(rows) is list and rows,'object inventory empty')
    result={}
    for row in rows:
        need(type(row) is dict and type(row.get('bytes')) is int and row['bytes']>0
             and type(row.get('sha256')) is str and re.fullmatch('[0-9a-f]{64}',row['sha256']),
             'object identity malformed')
        path=Path(row['path']).resolve()
        need(path.is_relative_to(Path(root).resolve()) and path.suffix.lower() in ('.o','.obj'),
             'object outside owned cache')
        name=key(path);need(name not in result,'duplicate object identity');result[name]=row
    return result


def actions(log,root):
    result=set()
    for line in log.splitlines():
        match=re.fullmatch(r'\[\d+/\d+\] Building (?:CXX|C|ASM) object (.+\.(?:o|obj))',line.strip())
        if match:
            path=Path(root)/match.group(1)
            need(path.resolve().is_relative_to(Path(root).resolve()),'Ninja action outside cache')
            result.add(key(path))
    return result


def classify(old,new,rebuilt):
    need(not(set(old)-set(new)),'old object inventory unexpectedly lost entries')
    result={'retained':[],'rebuiltUnchanged':[],'changed':[],'new':[]}
    for name,row in new.items():
        previous=old.get(name)
        same=previous is not None and all(previous[k]==row[k] for k in ('bytes','sha256'))
        if same:result['rebuiltUnchanged' if name in rebuilt else 'retained'].append(name)
        else:
            need(name in rebuilt,'changed/new object lacks actual rebuild action')
            result['changed' if previous is not None else 'new'].append(name)
    need(rebuilt<=set(new),'actual rebuild output missing from post inventory')
    return result


def checked_baseline(stage,refs,verify,read):
    sub,seal_name,seal_sha,inventory_sha,count=BASELINES[stage]
    def ref_for(path):
        rows=[r for r in refs if key(r['path'])==key(path)]
        need(len(rows)==1,'normal baseline reference missing/ambiguous');return rows[0]
    sealref=ref_for(BASE/sub/seal_name);need(sealref['sha256']==seal_sha,'wrong qualified baseline seal')
    sealed=read(verify(sealref));members=sealed.get('files',sealed.get('refs',[]))
    oldref=ref_for(BASE/sub/'new-object-inventory.json')
    need(oldref in members and oldref['sha256']==inventory_sha,'unqualified old inventory')
    rows=read(verify(oldref));need(len(rows)==count,'qualified object count differs')
    return inventory(rows,CACHES[stage])


def profile(stage,raw,verify,read):
    go=read(verify(raw['go']))
    need(raw['go']['sha256']==GO_SHA and go.get('source')==SOURCE and go.get('sourceTree')==TREE,
         'normal source authority differs')
    need(go.get('authorized') is True and go.get('plan')==raw['normalPlan'],
         'normal approved plan substituted')
    plan=read(verify(raw['normalPlan']))
    need(plan.get('codeRef')==SOURCE and set(plan.get('stages',{}))==set(CACHES),
         'normal frozen source/stages differ')
    row=plan['stages'][stage]
    need(key(row['newBuildRoot'])==key(CACHES[stage]) and row.get('sourceCommit')==SOURCE,
         'normal plan cache/source differs')
    return row


def command_summary(stage,terminal,raw,execution,normal_plan):
    validate_terminal(stage,terminal,raw)
    need(execution.get('source')==SOURCE and execution.get('sourceTree')==TREE
         and execution.get('copiedObjects')==0 and type(execution.get('copiedObjects')) is int,
         'normal execution source/copy mismatch')
    need(type(execution.get('memoryViolations')) is list and not execution['memoryViolations'],
         'normal execution memory violated')
    records=[r for r in execution.get('commands',[]) if r.get('name','').startswith(stage+'-')]
    need(json.dumps(records,sort_keys=True)==json.dumps(terminal['commands'],sort_keys=True),
         'normal original command records substituted')
    for row,kind in zip(records,('configure','build')):
        need(row.get('argv')==normal_plan[kind+'Argv'], 'normal actual argv differs from approved plan')
        need(key(row.get('cwd',''))==key(Path(raw['sourceRoot'])), 'normal command cwd differs')
    return True


def dependencies(text,root):
    result={};current=None
    for line in text.splitlines():
        if line.startswith('    ') and current is not None:
            path=Path(line.strip());path=path if path.is_absolute() else Path(root)/path
            result[current].add(key(path))
        elif line.strip():
            match=re.fullmatch(r'(.+\.(?:o|obj)): #deps \d+, deps mtime .+ \(VALID\)',line.strip())
            current=None
            if match:
                path=Path(match.group(1));path=path if path.is_absolute() else Path(root)/path
                current=key(path);need(current not in result,'duplicate Ninja dependency object')
                result[current]=set()
    return result


def input_rebuilds(old_source,new_source,before_deps,after_deps,rebuilt,post_objects):
    def inputs(value):
        rows=value.get('readInputs');need(type(rows) is list and rows,'source graph empty')
        result={}
        for row in rows:
            name=key(row['path']);need(name not in result,'duplicate source input path')
            result[name]=(row['bytes'],row['sha256'])
        return result
    old=inputs(old_source);new=inputs(new_source)
    changed={name for name in set(old)|set(new) if old.get(name)!=new.get(name)}
    affected=[]
    for obj in post_objects:
        deps=before_deps.get(obj,set())|after_deps.get(obj,set())
        if changed&deps:
            need(obj in rebuilt,'changed dependency retained without actual rebuild')
            affected.append(obj)
    return dict(changedInputs=sorted(changed),affectedObjects=sorted(affected))


def graph(plan,stage,api,expected):
    """Validate the new producer; do not feed historical object bytes into live drift."""
    item=plan['producers'][stage];raw=item['rawSource'];term=item['terminal']
    validate_terminal(stage,term,raw)
    need(item['seal']['sha256']==expected['seal'],'unqualified normal producer seal')
    sealed=api.small_json(api.verify(item['seal']))
    need(api.canonical(sealed)==api.canonical(item['fullIndex']),'normal seal/fullIndex differs')
    refs=sealed.get('files',sealed.get('refs'))
    need(type(refs) is list and refs and len(api.exact_refs(refs))==len(refs),'normal full index incomplete')
    def member(name):
        rows=[ref for ref in refs if Path(ref['path']).name==name]
        need(len(rows)==1,'normal source index missing/ambiguous '+name)
        return rows[0]
    for name,value in [('actual-terminal.json',term),('raw-source.json',raw)]:
        need(api.canonical(api.small_json(api.verify(member(name))))==api.canonical(value),
             'normal sealed metadata substituted')
    need(raw['raw']['sha256']==expected['rawSha'] and raw['raw']['bytes']==expected['rawBytes'],
         'wrong normal raw identity')
    need(raw['buildRaw']==dict(raw['raw'],path=raw['buildPath']), 'normal build/archive identity differs')
    frozen=profile(stage,raw,api.verify,api.small_json)
    execution=api.small_json(api.verify(term['executionRef']))
    command_summary(stage,term,raw,execution,frozen)
    summaryref=plan['resourceSummaries'][stage]
    need(api.canonical(summaryref)==api.canonical(member('actual-terminal.json')),
         'normal resource summary outside sealed actual terminal')
    source=api.small_json(api.verify(member('actual-compiler-inputs.json')))
    need(source.get('source')==SOURCE and any(row.get('sourceBinding',{}).get('sourceCommit')==SOURCE
         for row in source.get('readInputs',[])), 'normal current source binding missing')
    for row in source.get('readInputs',[]):
        binding=row.get('sourceBinding')
        if binding is not None:
            need(binding.get('sourceCommit')==SOURCE and binding.get('rawSha256')==row['sha256']
                 and type(binding.get('rawBytes')) is int and binding['rawBytes']==row['bytes'],
                 'normal current source binding substituted')
    objects=api.small_json(api.verify(member('new-object-inventory.json')))
    current=inventory(objects,CACHES[stage])
    proof=item['normalProvenance'];historical=proof['historicalMetadata']
    old=checked_baseline(stage,historical,api.verify,api.small_json)
    sub,seal_name,_,_,_=BASELINES[stage]
    old_source_ref=next((ref for ref in historical if key(ref['path'])==key(BASE/sub/'actual-compiler-inputs.json')),None)
    need(old_source_ref is not None,'qualified historical source graph missing')
    old_seal=api.small_json(api.verify(next(ref for ref in historical if key(ref['path'])==key(BASE/sub/seal_name))))
    need(old_source_ref in old_seal.get('files',old_seal.get('refs',[])), 'historical source graph not in qualified seal')
    old_source=api.small_json(api.verify(old_source_ref))
    root=Path(item['seal']['path']).resolve().parent
    def observed(role,relative):
        ref=proof[role]
        need(key(ref['path'])==key(root/relative), 'normal observation path substituted: '+role)
        need(ref in refs,'normal observation not sealed: '+role)
        return Path(api.verify(ref)).read_text(encoding='utf-8')
    buildlogref=proof['buildLog']
    need(key(buildlogref['path'])==key(root.parent/(stage+'-build.log'))
         and buildlogref in refs,'normal build log not original stage log')
    rebuilt=actions(Path(api.verify(buildlogref)).read_text(encoding='utf-8'),CACHES[stage])
    classification=classify(old,current,rebuilt)
    before=dependencies(observed('beforeDeps','before/ninja-deps.txt'),CACHES[stage])
    after=dependencies(observed('afterDeps','after/ninja-deps.txt'),CACHES[stage])
    input_rebuilds(old_source,source,before,after,rebuilt,current)
    need(set(current)-set(after)<=set(old), 'new object missing current Ninja dependency graph')
    if classification['retained']:
        need(raw['priorObjectsReused'] is True,'retained old objects not acknowledged')
    member('actual-source-command-flags.json')
    # References to the old inventory/source JSON remain historical metadata.
    # Their embedded old mutable cache pins MUST NOT become current live pins.
    closure=refs+historical+[item['seal'],raw['raw'],raw['buildRaw'],raw['go'],raw['normalPlan']]
    closure+=list(api.pins(source))+list(api.pins(objects))
    return item,source,api.original_refs(closure)
