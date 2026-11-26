"""Two closed commands through the unchanged strong artifact owner.

No GO means no execution; this is not a packaging or SDK qualification.
"""
from pathlib import Path
import argparse
import datetime as dt
import hashlib
import json
import os
import subprocess
import sys
import re

SOURCE='b5f078f52a8db9bb8eb3b347b155679c852d5c32'
ROOT='01a20286-30e2-7a20-a134-d11895959c44'
HERE=Path(__file__).resolve().parent
REPO=HERE.parents[1]
OUTPUT_ROOT=Path('D:/Codex/artifacts/task-20261125-b5f-sdk-package-tooling/evidence')
PYTHON='C:/Users/1/AppData/Local/Programs/Python/Python312/python.exe'
READOBJ='C:/Users/1/AppData/Local/Android/Sdk/ndk/28.2.13676358/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-readobj.exe'
GIT='C:/Program Files (x86)/Git/cmd/git.exe'
PE='C:/Windows/System32/where.exe'
PE_SHA='f51fab8041e5023d7290b540a2106f051ef0bd2bc3443c9daf318628b560fa29'
ENV_POLICY={
 'schema':'artifact-owner-environment-binding-v2',
 'comparisonEncoding':'sha256-json-sort-keys-default-utf8',
 'valueOnlyKey':'CODEX_TOOL_CALL_ID',
 'comparisonMarker':'<ARTIFACT_PREFLIGHT_TOOL_CALL_VALUE_V1>',
 'keyPresence':'exact-preserve-absence',
 'presentEmptyToolCallValue':'normalize-like-any-present-value',
 'allOtherKeysAndValues':'exact'}
PROTECTED={
 'artifact_owner.py':'83ded9e155a62c1b59e5e778dfb861d1c6678c3e10c33fd8783025377648fb20',
 'api_context.py':'3c216415d9d7e99dc6edf9b37fbe8077f8f0bcf6b93752fa26f2807159f4774b',
 'adapter.py':'f29334763b117fe4bfe5b94ddfad90b3ba942c572968864931475437b15da697',
 'contracts.py':'bb8aa6170a6d08d32492d13bf235fe9794751483e9239bb1c5e98f156fb3cf90',
 'companion.py':'ff11ddc10f7dee58f03abe22f7a2fab1cae303598c9679e28b369a522e9b8bb5',
 'static_proof.py':'811b15714d6ae0c8f18b57df5ceb519f2c7b9945cd6bad85c5006a5ce95b0175'}
SUPPORT={
 'kernel_job.py':'492b7ac00096aaa0c05164010b9cc19e7a6a3273689d2ae61917b387ef93950e',
 'build_resources.py':'db0b53eb89facac3f6551dfb0b2a8e141f13089ed00b2c040a76aac8ad637575',
 'qa_owner_safety.py':'7d79b97dd704949aa4b9869072b7408c9f9b3d9ac0c44536cdf3aa93cfe0fca6'}


def require(value,message):
    if not value:raise ValueError(message)


def pin(path):
    path=Path(path)
    require(path.is_file() and not path.is_symlink(),'regular pin required')
    with path.open('rb') as f:sha=hashlib.file_digest(f,'sha256').hexdigest()
    return dict(path=str(path),sha256=sha,bytes=path.stat().st_size)


def verify(ref):
    require(isinstance(ref,dict) and isinstance(ref.get('path'),str) and Path(ref['path']).is_absolute()
            and type(ref.get('bytes')) is int and ref['bytes']>0
            and isinstance(ref.get('sha256'),str) and re.fullmatch('[0-9a-f]{64}',ref['sha256']),
            'preflight pin unbound or malformed')
    value=pin(ref['path'])
    require(value['sha256']==ref['sha256'] and value['bytes']==ref['bytes'],'preflight pin changed')


def load(path):
    require(Path(path).stat().st_size<=1024*1024,'preflight metadata limit')
    return json.loads(Path(path).read_text(encoding='utf-8-sig'))


def commands():
    return [[PYTHON,'-I','-S',str(HERE/'preflight_marker.py')],
            [READOBJ,'--file-headers',PE]]


def stamp(value):
    parsed=dt.datetime.fromisoformat(value.replace('Z','+00:00'))
    require(parsed.tzinfo is not None,'timezone required')
    return parsed


def fresh(go,now):
    issued,expires=stamp(go['issuedUtc']),stamp(go['expiresUtc'])
    require(issued<=now<expires and 0<(expires-issued).total_seconds()<=360,'expired or invalid preflight GO')


def git_state(plan,env=None,cwd=None):
    env=dict(os.environ) if env is None else env
    cwd=plan['cwd'] if cwd is None else cwd
    require(subprocess.check_output([GIT,'-C',str(REPO),'rev-parse','HEAD'],text=True,env=env,cwd=cwd).strip()==plan['toolCommit'],'preflight tool commit differs')
    require(not subprocess.check_output([GIT,'-C',str(REPO),'status','--porcelain'],text=True,env=env,cwd=cwd).strip(),'preflight checkout dirty')


def environment_hashes(env):
    """Private comparison copy only. Never return a normalized launch map."""
    private=dict(env)
    full=hashlib.sha256(json.dumps(private,sort_keys=True).encode()).hexdigest()
    if 'CODEX_TOOL_CALL_ID' in private:private['CODEX_TOOL_CALL_ID']=ENV_POLICY['comparisonMarker']
    normalized=hashlib.sha256(json.dumps(private,sort_keys=True).encode()).hexdigest()
    return full,normalized


def prepare_environment_binding(env):
    full,normalized=environment_hashes(env)
    return dict(ENV_POLICY,prepareFullSha256=full,normalizedSha256=normalized)


def check_environment_binding(env,binding):
    require(isinstance(binding,dict) and set(binding)==set(ENV_POLICY)|{'prepareFullSha256','normalizedSha256'},'explicit exact environment binding v2 required')
    require(all(binding[k]==v for k,v in ENV_POLICY.items()),'unsupported environment normalization policy')
    for key in ('prepareFullSha256','normalizedSha256'):
        require(isinstance(binding[key],str) and re.fullmatch('[0-9a-f]{64}',binding[key]),'unbound environment digest')
    full,normalized=environment_hashes(env)
    audit=dict(schema=ENV_POLICY['schema'],prepareFullSha256=binding['prepareFullSha256'],
                currentFullSha256=full,prepareNormalizedSha256=binding['normalizedSha256'],
                currentNormalizedSha256=normalized,rawValuesRecorded=False)
    require(normalized==binding['normalizedSha256'],'current private environment comparison identity differs; hashAudit='+json.dumps(audit,sort_keys=True))
    return audit


def validate(plan,go,plan_sha,now):
    require(go is not None and go['schema']=='artifact-owner-preflight-go-v2'
            and go['rootConversation']==ROOT and go['authorized'] is True,'specific root preflight GO required')
    fresh(go,now)
    require(plan['schema']=='artifact-owner-preflight-plan-v2' and plan['source']==go['source']==SOURCE,'wrong preflight source')
    require(go['planSha256']==plan_sha and plan['nonce']==go['nonce'] and plan['nonce'],'preflight binding differs')
    require(plan['commands']==commands() and go['commands']==plan['commands'],'extra or altered argv')
    require(plan['commandTimeoutSeconds']==60 and go['commandTimeoutSeconds']==60,'short budget required')
    require(plan['cwd']==str(REPO) and go['cwd']==plan['cwd'],'wrong exact cwd')
    out=Path(plan['output']).resolve()
    require(out==Path(go['output']).resolve() and out.parent==OUTPUT_ROOT.resolve()
            and out.name.startswith('owner-preflight-') and not out.exists(),'fresh dedicated evidence output required')
    require(Path(go['cancel']).resolve()==out.with_suffix('.cancel'),'wrong dedicated cancel channel')
    require(plan['envOverrides']=={'PYTHONNOUSERSITE':'1','PYTHONPATH':''},'unexpected environment overrides')
    env=dict(os.environ);env.update(plan['envOverrides'])
    require(plan['environmentBinding']==go['environmentBinding'],'plan/GO environment policy differs')
    require(plan['environmentSha256']==go['environmentSha256']==plan['environmentBinding']['prepareFullSha256'],
            'prepare full environment identity differs')
    check_environment_binding(env,plan['environmentBinding'])
    key=lambda path:str(Path(path).resolve())
    expected={key(HERE/name):digest for name,digest in PROTECTED.items()}
    expected.update({key(Path(plan['supportDirectory'])/name):digest for name,digest in SUPPORT.items()})
    required=set(expected)|{key(__file__),key(HERE/'api_context.py'),key(HERE/'preflight_marker.py'),key(PYTHON),key(READOBJ),key(PE),key(GIT)}
    refs={key(r['path']):r for r in plan['files']}
    require(set(refs)==required and len(refs)==len(plan['files']),'complete exact preflight tool/input closure required')
    require(go['files']==plan['files'],'GO closure differs')
    for path,digest in expected.items():require(refs[path]['sha256']==digest,'protected owner/support bytes differ')
    require(refs[key(PE)]['sha256']==PE_SHA and 0<refs[key(PE)]['bytes']<=1024*1024,'fixed OS fixture only')
    for ref in refs.values():verify(ref)
    # Check fixed source/Git before monitor import; no shell, no arbitrary probe.
    git_state(plan,env=env,cwd=plan['cwd'])
    for name in SUPPORT:
        loaded=sys.modules.get(Path(name).stem)
        require(loaded is None or Path(getattr(loaded,'__file__','')).resolve()==Path(plan['supportDirectory'],name).resolve(),'shadowed support module')
    for name in ('artifact_owner','api_context'):
        loaded=sys.modules.get(name)
        require(loaded is None or Path(getattr(loaded,'__file__','')).resolve()==HERE/(name+'.py'),'shadowed local owner module')
    return out,env


def execute(plan,go,plan_sha):
    now=lambda:dt.datetime.now(dt.timezone.utc)
    out,env=validate(plan,go,plan_sha,now())
    require(not Path(go['cancel']).exists(),'preexisting cancellation')
    sys.path.insert(0,plan['supportDirectory'])
    from build_resources import gate,sample,heavy
    from artifact_owner import run_owned,admit_cancel
    admission=[]
    gate(admission,lambda:None)
    fresh(go,now());admit_cancel(go['cancel'])
    out.mkdir()
    state=dict(state='started',source=SOURCE,toolCommit=plan['toolCommit'],admission=admission,commands=[],
        qualification='OWNER_COMPATIBILITY_ONLY_NOT_SDK',shortChildObservation='NOT_CLAIMED',
        environmentAudit=check_environment_binding(env,plan['environmentBinding']))
    def save():(out/'execution.json').write_text(json.dumps(state,indent=2),encoding='utf8')
    try:
        for i,argv in enumerate(commands()):
            fresh(go,now());admit_cancel(go['cancel'])
            git_state(plan,env=env,cwd=plan['cwd'])
            for ref in plan['files']:verify(ref)
            step=dict(index=i);state['commands'].append(step)
            gate(step.setdefault('admission',[]),save)
            fresh(go,now());admit_cancel(go['cancel'])
            run_owned(argv,out/(str(i)+'.log'),step,save,go['cancel'],lambda:None,
                      timeout=60,mode='artifact',permitted_commands=commands(),permitted_images=[PYTHON,READOBJ],env=env,cwd=plan['cwd'])
            require(step['state']=='returned' and step['exitCode']==0 and not step.get('monitorErrors'), 'owner did not return successfully')
        for ref in plan['files']:verify(ref)
        git_state(plan,env=env,cwd=plan['cwd'])
        tail=sample();state['tailSample']=tail;save()
        require(tail['freeKiB']>3*1024**2 and not any(heavy(p) for p in tail['processes']),'tail resource first failure')
        state['state']='preflight_passed_not_sdk';save()
    except BaseException as exc:
        state.update(state='failed_first_error_no_retry',error=repr(exc));save();raise


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--plan',required=True);p.add_argument('--plan-sha256',required=True)
    p.add_argument('--go',required=True);p.add_argument('--go-sha256',required=True)
    args=p.parse_args()
    require(pin(args.plan)['sha256']==args.plan_sha256 and pin(args.go)['sha256']==args.go_sha256,'preflight request identity differs')
    execute(load(args.plan),load(args.go),args.plan_sha256)
