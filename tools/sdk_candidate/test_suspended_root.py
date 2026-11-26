import ast
import copy
import ctypes as C
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import tempfile
import types
import unittest
from unittest.mock import patch
import suspended_root as s
import suspended_launch as launch
from api_context import instrument
import owner_preflight as legacy
import suspended_preflight as pre
import test_owner_preflight

SUPPORT=Path('D:/Codex/artifacts/task-20261122-v102-api-upstream-integration/evidence/d3cc-validation-tools-v8/kernel_job.py')

class Held(BaseException):pass
class Fn:
    def __init__(self,fn):self.fn=fn
    def __call__(self,*a):return self.fn(*a)

class Fixture:
    def __init__(self,root,variant=None,count=1):
        self.path='C:/fixed/python.exe';self.variant=variant;self.count=count;self.events=[];self.last=122;self.counter=10;self.closed=[];self.record={}
        spec=importlib.util.spec_from_file_location('pure_suspended_kernel',SUPPORT)
        self.k=importlib.util.module_from_spec(spec)
        resource=types.ModuleType('build_resources');resource.rank=lambda p:0;resource.identity=lambda p:p
        with patch.dict('sys.modules',{'build_resources':resource}):spec.loader.exec_module(self.k)
        self.ctx=s.SuspendedContext(self.record,self.save,[],['C:/fixed/python.exe'],lambda:self.last,
            lambda n:setattr(self,'last',n),journal_path=root/'journal')
        api=self.api=self.k.Win64API.__new__(self.k.Win64API);api.launchCleanupErrors=[]
        api.ok=lambda v:v if v else self.fail('BOOL')
        api.createJob=lambda *a:9;api.query=self.query;api.init=self.init;api.update=lambda *a:1
        api.delete=lambda *a:self.events.append('delete');api.create=self.create
        api._close=lambda h:self.closed.append(h) or 1;api.current=lambda:1;api.stdInput=lambda n:None;api.duplicate=self.duplicate
        api.openProcess=lambda *a:self.fail('OpenProcess forbidden')
        api.isMember=self.member;api.pidOf=lambda h:77 if variant!='pid' else 88;api.times=self.times
        api.waitOne=lambda *a:258 if variant=='wait' else 0
        api.exit=lambda h,out:setattr(out._obj,'value',74) or 1
        api.image=self.image
        instrument(api,self.ctx);self.ctx.job='9';api.artifact_job=9
        api.resume=self.ctx.native('ResumeThread',self.resume)
        api.terminate=self.ctx.native('TerminateProcess',self.terminate)
        api.launchReceipt=self.k.PROCESSINFO()
        self.life=api.suspended=s.Lifecycle(api,self.ctx,9,os.path.normcase('C:/fixed/python.exe'),lambda:variant=='cancel',self.hold)
        self.ctx.after_create=self.life.created
        for name in ('C','H','SZ','PROCESSINFO','STARTUPINFOEX','subprocess'):setattr(launch,name,getattr(self.k,name))
    def fail(self,msg):raise OSError(msg)
    def hold(self,status):self.events.append('hold');raise Held()
    def save(self):
        if self.variant=='save' and self.ctx.root_image:raise OSError('snapshot fail')
    def duplicate(self,*a):self.counter+=1;a[3]._obj.value=self.counter;return 1
    def init(self,*a):a[3]._obj.value=64;self.last=122;return int(a[0] is not None)
    def create(self,*a):
        self.events.append(('create',a[5]));self.last=5
        if self.variant=='create-false':return 0
        pi=a[-1]._obj;pi.process=500;pi.thread=501;pi.pid=77;pi.tid=78
        if self.variant=='partial':pi.tid=0
        if self.variant=='unknown':raise OSError('unknown native outcome')
        return 1
    def times(self,h,*a):
        self.events.append('times');a[0]._obj.value=116444736000000000+1230000;return 1
    def member(self,h,j,b):self.events.append('member');b._obj.value=self.variant!='member';return 1
    def query(self,h,kind,data,*a):
        if kind==1:data._obj.active=1 if self.variant=='job' else 0
        return 1
    def image(self,h):
        self.events.append('image')
        def native(*a):
            if self.variant in ('image5','image31'):self.last=int(self.variant[5:]);return 0
            return 1
        result=self.ctx.native('QueryFullProcessImageNameW',native)(h,0,None,None)
        self.api.ok(result)
        if self.variant=='journal':self.ctx.broken=True
        return 'C:/wrong.exe' if self.variant=='wrong-image' else self.path
    def resume(self,h):
        self.events.append(('resume',h));self.last=31
        assert 501 not in self.closed
        if self.count=='exception':raise OSError('resume unknown')
        return self.count
    def terminate(self,h,code):
        self.events.append(('terminate',h,code))
        if self.variant=='terminate':return 0
        if self.variant=='terminate-exception':raise OSError('abort unknown')
        return 1
    def run(self):
        fake=types.ModuleType('msvcrt');fake.get_osfhandle=lambda n:2
        with patch.dict('sys.modules',{'msvcrt':fake}),patch.object(self.k.C,'get_last_error',lambda:self.last,create=True):
            return launch.launch(self.api,9,['C:/fixed/python.exe'],'C:/fixed',{'EXACT':'unchanged'},types.SimpleNamespace(fileno=lambda:1),types.SimpleNamespace(fileno=lambda:1))
    def finish(self):
        try:self.ctx.finish()
        except (OSError,ValueError):pass

class SuspendedTests(unittest.TestCase):
    def fixture(self,d,**kw):return Fixture(Path(d),**kw)
    def test_complete_proof_before_resume_thread_lifetime_trace_and_error(self):
        with tempfile.TemporaryDirectory() as d:
            f=self.fixture(d);self.assertEqual(f.run(),(500,77))
            self.assertEqual(f.events[:5],[('create',s.FLAGS),'times','member','image',('resume',501)])
            self.assertIn(501,f.closed);self.assertNotIn(500,f.closed);self.assertEqual(f.last,5)
            proof=f.record['rootPreResumeProof'];self.assertEqual((proof['pid'],proof['birth'],proof['image']),(77,123,'C:/fixed/python.exe'))
            self.assertEqual(set(proof['directEvents']),{'GetProcessTimes','GetProcessId','IsProcessInJob','QueryFullProcessImageNameW'})
            self.assertEqual(f.ctx.prior_root_image(500,f.api,77),proof['image'])
            self.assertEqual(f.events.count('times'),1);self.assertEqual(f.events.count('member'),1)
            resume=[e for e in f.record['apiCalls'] if e['operation']=='ResumeThread' and e['status']=='outcome'][0]
            self.assertEqual(resume['lastError'],31);self.assertEqual(resume['actualParameters'],{'threadHandle':'501'})
            self.assertLess(proof['directEvents']['QueryFullProcessImageNameW']['completedMonotonicNs'],resume['invokedMonotonicNs'])
            f.ctx.finish();self.assertTrue(f.record['apiJournal']['fullTraceEquivalent'])
    def test_never_resumed_proof_failures_precise_single_abort_and_firstfailure(self):
        for variant in ('image5','image31','pid','member','wrong-image','journal','save','cancel'):
            with self.subTest(variant=variant),tempfile.TemporaryDirectory() as d:
                f=self.fixture(d,variant=variant)
                with self.assertRaises((AssertionError,OSError)):f.run()
                self.assertEqual([e for e in f.events if isinstance(e,tuple) and e[0]=='terminate'],[('terminate',500,74)])
                self.assertFalse(f.life.resume_attempted);self.assertTrue(f.life.safe_to_release)
                self.assertIn(501,f.closed);self.assertNotIn(500,f.closed)
                self.assertEqual(f.life.status['qualification'],'FAILED');self.assertTrue(f.life.first_failure)
                self.assertIn('JOB_DOUBLE_ZERO',f.life.status['cleanup']);f.finish()
    def test_resume_bad_counts_exception_hold_no_abort_or_second_resume(self):
        for count in (0,2,0xffffffff,'exception'):
            with self.subTest(count=count),tempfile.TemporaryDirectory() as d:
                f=self.fixture(d,count=count)
                with self.assertRaises(Held):f.run()
                self.assertEqual(f.events.count(('resume',501)),1)
                self.assertFalse(f.life.terminate_attempted);self.assertNotIn(501,f.closed);self.assertNotIn(500,f.closed)
                self.assertEqual(f.life.status['cleanup'],'CLEANUP_BLOCKED');f.finish()
    def test_partial_unknown_create_hold_without_abort(self):
        for variant in ('partial','unknown'):
            with self.subTest(variant=variant),tempfile.TemporaryDirectory() as d:
                f=self.fixture(d,variant=variant)
                with self.assertRaises(Held):f.run()
                self.assertFalse(f.life.resume_attempted or f.life.terminate_attempted)
                self.assertNotIn(501,f.closed);self.assertNotIn(500,f.closed);f.finish()
    def test_false_create_does_not_control_any_process(self):
        with tempfile.TemporaryDirectory() as d:
            f=self.fixture(d,variant='create-false')
            with self.assertRaises(OSError):f.run()
            self.assertFalse(f.life.resume_attempted or f.life.terminate_attempted);f.finish()
    def test_abort_failed_or_unknown_wait_job_hold_handles(self):
        for variant in ('terminate','terminate-exception','wait','job'):
            with self.subTest(variant=variant),tempfile.TemporaryDirectory() as d:
                f=self.fixture(d,variant=variant);f.life.cancel=lambda:True
                with self.assertRaises(Held):f.run()
                self.assertEqual(f.events.count(('terminate',500,74)),1)
                self.assertNotIn(501,f.closed);self.assertNotIn(500,f.closed)
                self.assertEqual(f.life.status['cleanup'],'CLEANUP_BLOCKED');f.finish()
    def test_later_contradictions_and_close_reuse_permanently_taint(self):
        for kind in ('info','member','image','closed','birth','pid'):
            with self.subTest(kind=kind),tempfile.TemporaryDirectory() as d:
                f=self.fixture(d);f.run()
                if kind=='closed':f.ctx.closed(500);f.ctx.handles['500']=(77,123,'root')
                elif kind=='birth':f.ctx.handles['500']=(77,999,'root')
                elif kind=='pid':f.ctx.handles['500']=(88,123,'root')
                else:
                    bad={'info':(77,999,None),'member':False,'image':'C:/other.exe'}[kind]
                    with self.assertRaises(AssertionError):f.ctx.method(kind,lambda *a:bad,500,*([9] if kind=='member' else []))
                    self.assertTrue(f.ctx.root_tainted)
                with self.assertRaises(AssertionError):f.ctx.prior_root_image(500,f.api,77)
                f.finish()
    def test_journal_short_write_flush_fsync_after_create_never_resume(self):
        for kind in ('write','short','flush','fsync'):
            with self.subTest(kind=kind),tempfile.TemporaryDirectory() as d:
                f=self.fixture(d);original=f.create
                def create(*a):
                    # Change only after BOOL success, before native outcome is persisted.
                    stream=f.ctx.journal.stream
                    class Bad:
                        def write(self,b):
                            if kind=='write':raise OSError('write')
                            return stream.write(b[:1] if kind=='short' else b)
                        def flush(self):
                            if kind=='flush':raise OSError('flush')
                            stream.flush()
                        def fileno(self):return stream.fileno()
                        def close(self):stream.close()
                    f.ctx.journal.stream=Bad()
                    if kind=='fsync':fs.start()
                    return original(*a)
                fs=patch('api_context.os.fsync',side_effect=OSError('fsync'))
                # existing native create wrapper calls the original stored function
                f.api.create=f.ctx.native('CreateProcessW',create)
                try:
                    with self.assertRaises((AssertionError,OSError)):f.run()
                finally:
                    if kind=='fsync':fs.stop()
                    f.finish()
                self.assertFalse(f.life.resume_attempted);self.assertEqual(f.events.count(('terminate',500,74)),1)
    def test_resume_outcome_journal_failure_holds_without_abort(self):
        with tempfile.TemporaryDirectory() as d:
            f=self.fixture(d)
            def resume(h):f.events.append(('resume',h));f.ctx.broken=True;return 1
            f.api.resume=f.ctx.native('ResumeThread',resume)
            with self.assertRaises(Held):f.run()
            self.assertFalse(f.life.terminate_attempted);self.assertNotIn(501,f.closed);f.finish()
    def test_actual_owner_optin_runtime_success_abort_and_original_monitor(self):
        import artifact_owner as owner
        for fail in (False, True):
            with self.subTest(fail=fail),tempfile.TemporaryDirectory() as d:
                f=self.fixture(d,variant='image5' if fail else None);f.path=legacy.PYTHON
                f.api.k=types.SimpleNamespace(ResumeThread=Fn(f.resume),TerminateProcess=Fn(f.terminate))
                f.api.exit=f.ctx.native('GetExitCodeProcess',lambda h,out:setattr(out._obj,'value',74 if f.ctx.lifecycle.terminate_attempted else 0) or 1)
                class Base(f.k.KernelDomain):
                    def __init__(self):super().__init__(f.api)
                    def allowed(self,*a):raise AssertionError('no rows in this fixture')
                sample=lambda:dict(freeKiB=10*1024**2,processes=[])
                def attempt(errors,label,fn):
                    try:fn();return True
                    except BaseException as exc:errors.append(dict(stage=label,error=repr(exc)));return False
                def remember(errors,label,exc):errors.append(dict(stage=label,error=repr(exc)))
                def wait(child,monitor,errors,pause):monitor();return child.poll()
                rt=(Base,sample,lambda p:True,lambda p:p,lambda:'now',attempt,remember,wait)
                f.record['monitorErrors']=f.ctx.errors
                fake=types.ModuleType('msvcrt');fake.get_osfhandle=lambda n:2
                with patch.dict('sys.modules',{'kernel_job':f.k,'msvcrt':fake}),patch.object(owner,'runtime',return_value=rt), \
                     patch.object(s,'SuspendedContext',return_value=f.ctx),patch.object(f.k.C,'get_last_error',lambda:f.last,create=True):
                    args=(legacy.commands()[0],Path(d)/'log',f.record,f.save,Path(d)/'cancel',lambda:None)
                    kw=dict(timeout=60,permitted_commands=legacy.commands(),permitted_images=[legacy.PYTHON,legacy.READOBJ],
                            cwd=str(legacy.REPO),env={'EXACT':'unchanged'},lifecycle_profile=s.PROFILE)
                    if fail:
                        with self.assertRaises(AssertionError):owner.run_owned(*args,**kw)
                    else:
                        try:owner.run_owned(*args,**kw)
                        except AssertionError:raise AssertionError(json.dumps(f.record.get('monitorErrors')))
                self.assertEqual(f.record['state'],'failed' if fail else 'returned')
                self.assertEqual(f.record['suspendedRoot']['qualification'],'FAILED' if fail else 'QUALIFIED')
                self.assertEqual(f.record['ownClosure'],'retained_handles_signaled_and_job_double_zero')
                self.assertTrue(f.record['rawSamples'] and f.record['apiJournal']['fullTraceEquivalent'])
                self.assertTrue({500,501,9}.issubset(set(f.closed)))
                self.assertEqual(f.events.count('times'),1)
    def test_resume_attempt_write_failure_aborts_before_native_resume(self):
        with tempfile.TemporaryDirectory() as d:
            f=self.fixture(d);original=f.ctx.journal.append
            def append(kind,value):
                if kind=='api' and value['operation']=='ResumeThread' and value['status']=='attempt':
                    raise OSError('resume attempt short write')
                return original(kind,value)
            f.ctx.journal.append=append
            with self.assertRaises(AssertionError):f.run()
            self.assertFalse(f.life.resume_attempted);self.assertEqual(f.events.count(('terminate',500,74)),1);f.finish()
    def test_thread_close_failure_holds_original_resources(self):
        with tempfile.TemporaryDirectory() as d:
            f=self.fixture(d)
            def close(h):
                if h==501:raise OSError('close failed')
                f.closed.append(h);return 1
            f.api._close=f.ctx.native('CloseHandle',close)
            with self.assertRaises(Held):f.run()
            self.assertFalse(f.life.terminate_attempted);self.assertNotIn(500,f.closed);self.assertNotIn(501,f.closed);f.finish()
    def test_copy_diff_is_limited_and_legacy_flags_unchanged(self):
        base=ast.parse(SUPPORT.read_text());fn=next(x for x in next(x for x in base.body if isinstance(x,ast.ClassDef) and x.name=='Win64API').body if isinstance(x,ast.FunctionDef) and x.name=='launch')
        local=ast.parse(Path(launch.__file__).read_text()).body[1]
        # Main launch path exactly identical except explicit flag constant.
        old=next(x for x in fn.body if isinstance(x,ast.Try)).body
        new=next(x for x in local.body if isinstance(x,ast.Try)).body
        normalized=copy.deepcopy(new)
        for x in ast.walk(ast.Module(body=normalized,type_ignores=[])):
            if isinstance(x,ast.Constant) and x.value==s.FLAGS:x.value=0x80400
        self.assertEqual(ast.dump(ast.Module(body=old,type_ignores=[])),ast.dump(ast.Module(body=normalized,type_ignores=[])))
        self.assertIn('0x80400',SUPPORT.read_text());self.assertNotIn('0x80404',SUPPORT.read_text())
    def test_hold_policy_never_returns_even_on_interrupt_and_keeps_objects(self):
        # Structural oracle: no return/raise/close/OS APIs in production hold; Event.wait loop.
        tree=ast.parse(Path(s.__file__).read_text());fn=next(x for x in tree.body if isinstance(x,ast.FunctionDef) and x.name=='hold_creator')
        self.assertFalse(any(isinstance(x,(ast.Return,ast.Raise)) for x in ast.walk(fn)))
        self.assertTrue(any(isinstance(x,ast.While) for x in ast.walk(fn)))
        self.assertIn('event.wait()',ast.unparse(fn));self.assertIn('except BaseException',ast.unparse(fn))

class SchemaTests(unittest.TestCase):
    def fixture(self,root):
        plan,go,now=test_owner_preflight.PreflightTests().fixture(root)
        plan['schema']=s.PLAN_SCHEMA;go['schema']=s.GO_SCHEMA
        contract=dict(profile=s.PROFILE,enabled=True,effectiveCreationFlags=s.FLAGS,neverResumedAbortExitCode=74,custodian='CURRENT_CREATING_PROCESS')
        plan['lifecycle']=copy.deepcopy(contract);go['lifecycle']=copy.deepcopy(contract);go['custodianResponseSeconds']=300
        for ref in plan['files']:
            if Path(ref['path']).name in pre.PROTECTED:ref['sha256']=pre.PROTECTED[Path(ref['path']).name]
        plan['files'] += [dict(path=str(pre.HERE/n),bytes=1,sha256='b'*64) for n in ('suspended_preflight.py','suspended_root.py','suspended_launch.py')]
        go['files']=copy.deepcopy(plan['files'])
        return plan,go,now
    def test_profile_schema_flag_enable_go_and_closure_mismatch_refused(self):
        with tempfile.TemporaryDirectory() as d,patch.object(pre,'OUTPUT_ROOT',Path(d)),patch.object(pre,'verify',lambda r:None),patch.object(pre,'git_state',lambda *a,**k:None):
            p,g,n=self.fixture(Path(d));pre.validate(p,g,'pin',n)
            changes=[lambda p,g:p.update(schema='artifact-owner-preflight-plan-v2'),lambda p,g:g.update(schema='artifact-owner-preflight-go-v2'),lambda p,g:p.pop('lifecycle'),lambda p,g:g['lifecycle'].update(enabled=False),lambda p,g:p['lifecycle'].update(effectiveCreationFlags=0x80400),lambda p,g:g['lifecycle'].update(profile='B'),lambda p,g:g.pop('custodianResponseSeconds'),lambda p,g:p['files'].pop()]
            for change in changes:
                a,b=copy.deepcopy(p),copy.deepcopy(g);change(a,b)
                with self.assertRaises((ValueError,KeyError)):pre.validate(a,b,'pin',n)
            with self.assertRaises(ValueError):pre.validate(p,None,'pin',n)
    def test_new_entry_serial_firsterror_expiry_and_tail_resource_guards(self):
        with patch.object(test_owner_preflight,'p',pre):
            test_owner_preflight.PreflightTests().test_actual_new_entry_fake_serial_owner_and_first_error()
            test_owner_preflight.PreflightTests().test_expiry_blocks_second_and_tail_resource_is_failure()

    def test_fixed_commands_env_cwd_budget_unchanged_and_new_module_source(self):
        self.assertEqual(pre.commands(),legacy.commands());self.assertEqual(pre.ENV_POLICY,legacy.ENV_POLICY)
        for module in (s,launch,pre):
            self.assertEqual(Path(module.__file__).resolve().parent,pre.HERE)
            self.assertEqual(len(hashlib.sha256(Path(module.__file__).read_bytes()).hexdigest()),64)

if __name__=='__main__':unittest.main()



