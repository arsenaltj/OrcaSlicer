import ast
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import types
import unittest
from unittest.mock import patch
import artifact_owner as o
import adapter
import owner_preflight as p
import test_artifact_owner
import test_owner_preflight

FIXTURE=Path(__file__).with_name('env_empty_fixture.py')
KEYS=('CODEX_ENV_TEST_MISSING','CODEX_ENV_TEST_EMPTY','CODEX_ENV_TEST_TEXT','CODEX_ENV_TEST_UNICODE',
      'GIT_CONFIG_COUNT','GIT_CONFIG_KEY_0','GIT_CONFIG_VALUE_0')


def env_hash(value):
    return hashlib.sha256(json.dumps(value,sort_keys=True).encode()).hexdigest()


def native_child(env=None,cwd=None):
    result=subprocess.run([sys.executable,'-I','-S',str(FIXTURE)],env=env,cwd=cwd,capture_output=True,text=True,encoding='utf8',check=True)
    return json.loads(result.stdout)


class EmptyEnvironment(unittest.TestCase):
    def test_fixed_child_missing_empty_text_unicode_and_parent_native_unchanged(self):
        before=dict(os.environ);cwd=os.getcwd();native_before=native_child()
        child=dict(before);child.pop('CODEX_ENV_TEST_MISSING',None)
        child.update(CODEX_ENV_TEST_EMPTY='',CODEX_ENV_TEST_TEXT='synthetic-text',CODEX_ENV_TEST_UNICODE='中文测试🙂',
                     GIT_CONFIG_COUNT='1',GIT_CONFIG_KEY_0='safe.directory',GIT_CONFIG_VALUE_0='')
        with tempfile.TemporaryDirectory() as d:
            observed=native_child(child,d)
            for key in KEYS:
                self.assertEqual(observed['environment'][key]['present'],key in child)
                if key in child:
                    self.assertEqual(observed['environment'][key]['empty'],child[key]=='')
                    self.assertEqual(observed['environment'][key]['sha256'],hashlib.sha256(child[key].encode()).hexdigest())
            self.assertEqual(observed['cwdSha256'],hashlib.sha256(d.encode()).hexdigest())
            self.assertEqual(observed['nativeEnvironmentSha256'],hashlib.sha256(json.dumps(child,sort_keys=True).encode()).hexdigest())
        self.assertEqual(env_hash(dict(os.environ)),env_hash(before));self.assertEqual(os.getcwd(),cwd)
        self.assertEqual(native_child(),native_before)

    def test_real_owner_launch_receives_explicit_map_without_parent_changes(self):
        before=dict(os.environ);oldcwd=os.getcwd()
        runtime,state=test_artifact_owner.ArtifactTests().fake(fast=True)
        Domain=runtime[0];observed=[]
        class Capture(Domain):
            def launch(self,argv,cwd,env,*rest):
                observed.append((dict(env),cwd));return super().launch(argv,cwd,env,*rest)
        env=dict(before);env.update(CODEX_ENV_TEST_EMPTY='',CODEX_ENV_TEST_UNICODE='中文')
        with tempfile.TemporaryDirectory() as d,patch.object(o,'runtime',return_value=(Capture,)+runtime[1:]),patch.object(o.time,'sleep',lambda n:None):
            record={}
            o.run_owned(['exact-tool'],Path(d)/'log',record,lambda:None,Path(d)/'cancel',lambda:None,
                        permitted_commands=[['exact-tool']],permitted_images=['exact-tool'],env=env,cwd=d)
            self.assertEqual([(env_hash(x),y) for x,y in observed],[(env_hash(env),d)])
            self.assertEqual(record['launchEnvironmentSha256'],hashlib.sha256(json.dumps(env,sort_keys=True).encode()).hexdigest())
        self.assertEqual(env_hash(dict(os.environ)),env_hash(before));self.assertEqual(os.getcwd(),oldcwd)

    def test_preflight_success_error_expiry_cancel_preserve_parent(self):
        before=dict(os.environ);oldcwd=os.getcwd();native_before=native_child()
        for mode in ('success','error','expiry','cancel'):
            with self.subTest(mode=mode),tempfile.TemporaryDirectory() as d:
                plan,go,now=test_owner_preflight.PreflightTests().fixture(Path(d))
                env=dict(before);env['CODEX_ENV_TEST_EMPTY']='';env['CODEX_ENV_TEST_UNICODE']='中文'
                plan['environmentBinding']=p.prepare_environment_binding(env)
                calls=[]
                resources=types.ModuleType('build_resources');resources.gate=lambda *x:None
                resources.sample=lambda:dict(freeKiB=10*1024**2,processes=[]);resources.heavy=lambda r:True
                owner=types.ModuleType('artifact_owner')
                def cancel(path):
                    if mode=='cancel' and calls:raise ValueError('cancel')
                owner.admit_cancel=cancel
                def run(argv,path,step,*args,**kw):
                    self.assertEqual(env_hash(kw['env']),env_hash(env));self.assertEqual(kw['cwd'],plan['cwd'])
                    self.assertEqual(env_hash(dict(os.environ)),env_hash(before));self.assertEqual(os.getcwd(),oldcwd)
                    calls.append(kw)
                    if mode=='error':raise ValueError('first failure after natural close')
                    step.update(state='returned',exitCode=0,monitorErrors=[])
                owner.run_owned=run
                def fresh(*args):
                    if mode=='expiry' and calls:raise ValueError('expired')
                with patch.dict(p.sys.modules,{'build_resources':resources,'artifact_owner':owner}),\
                     patch.object(p,'validate',return_value=(Path(plan['output']),env)),patch.object(p,'verify',lambda *x:None),\
                     patch.object(p,'git_state',lambda *x,**kw:None),patch.object(p,'fresh',fresh):
                    if mode=='success':p.execute(plan,go,'pin')
                    else:
                        with self.assertRaises(ValueError):p.execute(plan,go,'pin')
                self.assertEqual(len(calls),2 if mode=='success' else 1)
                self.assertEqual(env_hash(dict(os.environ)),env_hash(before));self.assertEqual(os.getcwd(),oldcwd)
                self.assertEqual(native_child(),native_before)

    def test_adapter_command_explicit_override_not_global(self):
        tree=ast.parse(Path(adapter.__file__).read_text())
        fn=next(x for x in ast.walk(tree) if isinstance(x,ast.FunctionDef) and x.name=='command')
        before=dict(os.environ);cwd=os.getcwd();observed=[]
        with tempfile.TemporaryDirectory() as d:
            def run(argv,path,*args,**kw):
                observed.append(kw);self.assertEqual(env_hash(dict(os.environ)),env_hash(before));Path(path).write_text('ok')
            env=dict(source_check=lambda:None,admit_cancel=lambda x:None,cancel=Path(d)/'cancel',state={'commands':[]},
                gate=lambda *x:None,save=lambda:None,evidence=Path(d),os=os,run_owned=run,commands=lambda p:[['tool']],
                plan={k:'tool' for k in ('python','node','readobj','readelf','strip','git')},base_env=before,repo=Path(d),
                lifecycle=dict(profile='SDK_SUSPENDED_ROOT_V1',commandTimeoutSeconds=7200))
            exec(compile(ast.Module(body=[fn],type_ignores=[]),'actual-adapter-env','exec'),env)
            self.assertEqual(env['command'](['tool'],'test',{'CODEX_ENV_TEST_EMPTY':''}),'ok')
            self.assertEqual(env_hash(observed[0]['env']),env_hash(dict(before,CODEX_ENV_TEST_EMPTY='')))
            self.assertEqual(observed[0]['cwd'],Path(d))
        self.assertEqual(env_hash(dict(os.environ)),env_hash(before));self.assertEqual(os.getcwd(),cwd)

    def test_git_before_after_explicit_empty_value_kept(self):
        root=Path(__file__).resolve().parents[2]
        env=dict(os.environ)
        def git(args):
            r=subprocess.run([p.GIT,'-C',str(root),*args],env=env,cwd=root,capture_output=True)
            self.assertEqual(r.returncode,0,'Git failed, stderrSHA='+hashlib.sha256(r.stderr).hexdigest())
            return r.stdout
        before=(git(['rev-parse','HEAD']),git(['status','--porcelain']))
        native_child(dict(env,CODEX_ENV_TEST_EMPTY=''))
        after=(git(['rev-parse','HEAD']),git(['status','--porcelain']))
        self.assertEqual(after,before)


if __name__=='__main__':unittest.main()
