import tempfile
from pathlib import Path
from unittest.mock import patch
import unittest
import artifact_owner as o
import contracts
import adapter


class ArtifactTests(unittest.TestCase):
    def fake(self, *, bad=False, fast=False, error=False):
        state=dict(samples=0,settled=0,closed=False,wait=False,raw=[])
        class Child:
            pid=42
            def poll(self):return 0 if fast else (None if state['samples']==0 else 0)
        class Domain:
            def launch(self,*args):return Child()
            def prove_root(self):
                if error:raise ValueError('image')
            def observe(self,rows,heavy):
                if error:raise ValueError('birth')
                return {'exact-job-member'}
            def allowed(self,row,owned,images):return row.get('own',False)
            def settled(self):state['settled']+=1;return state['settled']>=3
            def recover_root(self):return Child()
            def snapshot(self):return dict(consistentZeroChecks=2)
            def close(self):state['closed']=True
        def sample():
            state['samples']+=1
            return dict(freeKiB=1 if bad else 10*1024**2,processes=[dict(Name='java.exe',own=False)] if bad else [])
        def attempt(errors,label,fn):
            try:fn();return True
            except BaseException as ex:errors.append(dict(stage=label,error=repr(ex)));return False
        def remember(errors,label,ex):errors.append(dict(stage=label,error=repr(ex)))
        def wait(child,monitor,errors,pause):state['wait']=True;monitor();return 0
        return (Domain,sample,lambda p:True,lambda p:p['Name'],lambda:'now',attempt,remember,wait),state

    def test_success_fastroot_and_childdrain(self):
        for fast in (False,True):
            with tempfile.TemporaryDirectory() as d:
                runtime,state=self.fake(fast=fast);record={}
                with patch.object(o,'runtime',return_value=runtime),patch.object(o.time,'sleep',lambda x:None):
                    o.run_owned(['exact-tool'],Path(d)/'log',record,lambda:None,Path(d)/'cancel',lambda:None,
                                permitted_commands=[['exact-tool']],permitted_images=['exact-tool'])
                self.assertTrue(state['closed'] and state['wait'])
                self.assertGreaterEqual(state['settled'],3)
                self.assertEqual(record['state'],'returned')
                self.assertTrue(record['rawSamples'])

    def test_exception_and_foreign_or_tail_still_wait_close(self):
        for kwargs in (dict(bad=True),dict(error=True),dict(fast=True,bad=True)):
            with tempfile.TemporaryDirectory() as d:
                runtime,state=self.fake(**kwargs);record={}
                with patch.object(o,'runtime',return_value=runtime),patch.object(o.time,'sleep',lambda x:None):
                    with self.assertRaises(AssertionError):
                        o.run_owned(['exact-tool'],Path(d)/'log',record,lambda:None,Path(d)/'cancel',lambda:None,
                                    permitted_commands=[['exact-tool']],permitted_images=['exact-tool'])
                self.assertTrue(state['closed'] and state['wait'])
                self.assertEqual(record['state'],'failed')
                self.assertTrue(record['rawSamples'])

    def test_save_errors_never_skip_natural_wait(self):
        with tempfile.TemporaryDirectory() as d:
            runtime,state=self.fake();record={}
            def save():raise OSError('diagnostic')
            with patch.object(o,'runtime',return_value=runtime),patch.object(o.time,'sleep',lambda x:None):
                with self.assertRaises(AssertionError):
                    o.run_owned(['exact-tool'],Path(d)/'log',record,save,Path(d)/'cancel',lambda:None,
                                permitted_commands=[['exact-tool']],permitted_images=['exact-tool'])
            self.assertTrue(state['closed'] and state['wait'])

    def test_cancel_and_unknown_command_prevent_runtime(self):
        with tempfile.TemporaryDirectory() as d,patch.object(o,'runtime',side_effect=AssertionError('must not import kernel')):
            with self.assertRaises(AssertionError):
                o.run_owned(['compiler'],Path(d)/'log',{},lambda:None,Path(d)/'cancel',lambda:None,
                            permitted_commands=[['exact-tool']],permitted_images=['exact-tool'])
            cancel=Path(d)/'cancel';cancel.touch()
            with self.assertRaises(AssertionError):
                o.run_owned(['exact-tool'],Path(d)/'log',{},lambda:None,cancel,lambda:None,
                            permitted_commands=[['exact-tool']],permitted_images=['exact-tool'])

    def test_legacy_owner_and_missing_protocol_rejected(self):
        for protocol in (None,'build-v5','build-v6'):
            with self.assertRaises(ValueError):contracts.owner_protocol(dict(ownerProtocol=protocol),adapter)

    def test_missing_owner_or_transitive_pin_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            plan=dict(ownerProtocol=o.PROTOCOL,artifactOwner=o.__file__,ownerDirectory=d,toolFiles=[])
            with self.assertRaisesRegex(ValueError,'missing local'):
                contracts.owner_protocol(plan,adapter)
            plan['toolFiles']=[adapter.identity(o.__file__)]
            plan['toolFiles'].append(adapter.identity(Path(o.__file__).with_name('api_context.py')))
            with self.assertRaisesRegex(ValueError,'API-v8 transitive'):
                contracts.owner_protocol(plan,adapter)

    def test_actual_adapter_command_to_artifact_owner_interface(self):
        import ast
        tree=ast.parse(Path(adapter.__file__).read_text())
        command=next(n for n in ast.walk(tree) if isinstance(n,ast.FunctionDef) and n.name=='command')
        with tempfile.TemporaryDirectory() as d:
            runtime,state=self.fake(fast=True)
            plan={k:'exact-tool' for k in ('python','node','readobj','readelf','strip','git')}
            record=dict(commands=[])
            env=dict(source_check=lambda:None,admit_cancel=o.admit_cancel,cancel_file=None,
                     cancel=Path(d)/'cancel',state=record,gate=lambda *x:None,save=lambda:None,
                     evidence=Path(d),os=o.os,run_owned=o.run_owned,commands=lambda p:[['exact-tool']],plan=plan)
            exec(compile(ast.Module(body=[command],type_ignores=[]),'actual-adapter-command','exec'),env)
            with patch.object(o,'runtime',return_value=runtime),patch.object(o.time,'sleep',lambda x:None):
                env['command'](['exact-tool'],'direct')
            self.assertTrue(state['closed'] and state['wait'])
            self.assertEqual(record['commands'][0]['protocol'],o.PROTOCOL)
            self.assertEqual(record['commands'][0]['state'],'returned')

    def test_adapter_artifact_call_signature(self):
        import ast
        tree=ast.parse(Path(adapter.__file__).read_text())
        calls=[n for n in ast.walk(tree) if isinstance(n,ast.Call) and isinstance(n.func,ast.Name) and n.func.id=='run_owned']
        self.assertEqual(len(calls),1)
        keys={k.arg for k in calls[0].keywords}
        self.assertTrue({'mode','permitted_commands','permitted_images'}<=keys)
        self.assertEqual(next(k.value.value for k in calls[0].keywords if k.arg=='mode'),'artifact')

if __name__=='__main__':unittest.main()
