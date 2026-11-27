import copy
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import tempfile
import types
import unittest
from unittest.mock import patch
import owner_preflight as p


class PreflightTests(unittest.TestCase):
    def fixture(self,root):
        now=dt.datetime(2026,11,26,tzinfo=dt.timezone.utc)
        plan=dict(schema='artifact-owner-preflight-plan-v2',source=p.SOURCE,nonce='unique',cwd=str(p.REPO),
            output=str(root/'owner-preflight-test'),supportDirectory=str(root/'support'),commandTimeoutSeconds=60,
            commands=p.commands(),envOverrides={'PYTHONNOUSERSITE':'1','PYTHONPATH':''},toolCommit='a'*40)
        env=dict(os.environ);env.update(plan['envOverrides'])
        plan['environmentSha256']=hashlib.sha256(json.dumps(env,sort_keys=True).encode()).hexdigest()
        plan['environmentBinding']=p.prepare_environment_binding(env)
        names=[str(p.HERE/n) for n in p.PROTECTED]+[str(root/'support'/n) for n in p.SUPPORT]+[str(Path(p.__file__).resolve()),str(p.HERE/'preflight_marker.py'),p.PYTHON,p.READOBJ,p.PE,p.GIT]
        refs=[]
        for name in dict.fromkeys(names):
            digest=p.PROTECTED.get(Path(name).name,p.SUPPORT.get(Path(name).name,'b'*64))
            refs.append(dict(path=name,bytes=65536 if name==p.PE else 1,sha256=p.PE_SHA if name==p.PE else digest))
        plan['files']=refs
        go=dict(schema='artifact-owner-preflight-go-v2',rootConversation=p.ROOT,authorized=True,
            issuedUtc=now.isoformat(),expiresUtc=(now+dt.timedelta(minutes=5)).isoformat(),source=p.SOURCE,
            planSha256='pin',nonce='unique',output=plan['output'],cancel=str(Path(plan['output']).with_suffix('.cancel')),
            cwd=plan['cwd'],commands=copy.deepcopy(plan['commands']),environmentSha256=plan['environmentSha256'],
            commandTimeoutSeconds=60,files=refs)
        go['environmentBinding']=copy.deepcopy(plan['environmentBinding'])
        return plan,go,now

    def test_closed_commands_and_guard_negatives_no_runtime(self):
        with tempfile.TemporaryDirectory() as d,patch.object(p,'OUTPUT_ROOT',Path(d)),patch.object(p,'verify',lambda r:None), \
             patch.object(p.subprocess,'check_output',side_effect=lambda argv,**kw:'a'*40+'\n' if argv[-1]=='HEAD' else ''):
            plan,go,now=self.fixture(Path(d));p.validate(plan,go,'pin',now)
            for change in (lambda a,b:b.update(authorized=False),lambda a,b:b.update(rootConversation='other'),
                lambda a,b:a.update(source='old'),lambda a,b:b.update(planSha256='wrong'),
                lambda a,b:a['commands'][0].append('extra'),lambda a,b:a.update(files=[]),
                lambda a,b:a.update(output=str(Path(d)/'wrong')),lambda a,b:a['envOverrides'].update(PYTHONPATH='untrusted')):
                a,b=copy.deepcopy(plan),copy.deepcopy(go);change(a,b)
                with self.assertRaises(ValueError):p.validate(a,b,'pin',now)
            with self.assertRaises(ValueError):p.validate(plan,None,'pin',now)
            with self.assertRaises(ValueError):p.validate(plan,go,'pin',now+dt.timedelta(minutes=6))

    def test_actual_new_entry_fake_serial_owner_and_first_error(self):
        for fail in (False,True):
            with tempfile.TemporaryDirectory() as d:
                plan,go,now=self.fixture(Path(d));events=[]
                resources=types.ModuleType('build_resources')
                resources.gate=lambda *x:events.append('double_gate')
                resources.sample=lambda:dict(freeKiB=10*1024**2,processes=[])
                resources.heavy=lambda row:True
                owner=types.ModuleType('artifact_owner');owner.admit_cancel=lambda x:None
                def run(argv,log,step,save,cancel,callback,**kwargs):
                    events.append(argv)
                    self.assertEqual(kwargs['mode'],'artifact');self.assertEqual(kwargs['timeout'],60)
                    if fail:raise ValueError('fake API31 after natural drain')
                    step.update(state='returned',exitCode=0,monitorErrors=[],ownClosure='retained_handles_signaled_and_job_double_zero')
                    save()
                owner.run_owned=run
                with patch.dict(p.sys.modules,{'build_resources':resources,'artifact_owner':owner}),\
                     patch.object(p,'validate',return_value=(Path(plan['output']),dict(os.environ,**plan['envOverrides']))),\
                     patch.object(p,'verify',lambda ref:None),patch.object(p,'fresh',lambda *x:None),\
                     patch.object(p,'git_state',lambda *x,**kw:None):
                    if fail:
                        with self.assertRaises(ValueError):p.execute(plan,go,'pin')
                    else:p.execute(plan,go,'pin')
                result=json.loads((Path(plan['output'])/'execution.json').read_text())
                self.assertEqual(len(result['commands']),1 if fail else 2)
                self.assertEqual(result['state'],'failed_first_error_no_retry' if fail else 'preflight_passed_not_sdk')

    def test_expiry_blocks_second_and_tail_resource_is_failure(self):
        for tail in (False,True):
            with tempfile.TemporaryDirectory() as d:
                plan,go,now=self.fixture(Path(d));state=dict(calls=0)
                resources=types.ModuleType('build_resources');resources.gate=lambda *x:None
                resources.sample=lambda:dict(freeKiB=1,processes=[])
                resources.heavy=lambda p:True
                owner=types.ModuleType('artifact_owner');owner.admit_cancel=lambda p:None
                def run(argv,log,step,*args,**kwargs):
                    state['calls']+=1;step.update(state='returned',exitCode=0,monitorErrors=[])
                def check(*args):
                    if not tail and state['calls']>=1:raise ValueError('expired')
                owner.run_owned=run
                with patch.dict(p.sys.modules,{'build_resources':resources,'artifact_owner':owner}),\
                     patch.object(p,'validate',return_value=(Path(plan['output']),dict(os.environ,**plan['envOverrides']))),\
                     patch.object(p,'verify',lambda *x:None),patch.object(p,'git_state',lambda *x,**kw:None),\
                     patch.object(p,'fresh',check):
                    with self.assertRaises(ValueError):p.execute(plan,go,'pin')
                self.assertEqual(state['calls'],2 if tail else 1)
                record=json.loads((Path(plan['output'])/'execution.json').read_text())
                self.assertEqual(record['state'],'failed_first_error_no_retry')

    def test_marker_is_fixed_and_does_not_spawn(self):
        import ast
        tree=ast.parse((p.HERE/'preflight_marker.py').read_text())
        self.assertEqual(len(tree.body),1)
        self.assertEqual(tree.body[0].value.func.id,'print')
        self.assertEqual(tree.body[0].value.args[0].value,'ARTIFACT_OWNER_PREFLIGHT_FIXED_ROOT_V1')

if __name__=='__main__':unittest.main()
