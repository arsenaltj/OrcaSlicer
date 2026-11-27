import io
import json
from pathlib import Path
import tempfile
import types
import ast
import os
import importlib.util
import unittest
from unittest.mock import patch
from api_context import Context,Journal,instrument
import artifact_owner as owner
import test_artifact_owner


class EarlyJournal(unittest.TestCase):
    def test_real_owner_fsync_failure_still_natural_wait_and_close(self):
        with tempfile.TemporaryDirectory() as d:
            runtime,state=test_artifact_owner.ArtifactTests().fake(fast=True)
            record={}
            with patch.object(owner,'runtime',return_value=runtime),patch.object(owner.time,'sleep',lambda n:None),\
                 patch('api_context.os.fsync',side_effect=OSError('fsync')):
                with self.assertRaises(AssertionError):
                    owner.run_owned(['exact-tool'],Path(d)/'log',record,lambda:None,Path(d)/'cancel',lambda:None,
                                    permitted_commands=[['exact-tool']],permitted_images=['exact-tool'])
            self.assertTrue(state['wait'] and state['closed'])
            self.assertEqual(record['state'],'failed')
            self.assertEqual(record['apiJournalStatus'],'INCOMPLETE_STICKY_FAILURE')
    def test_frozen_base_launch_hook_precedes_real_finally_cleanup(self):
        resource=types.ModuleType('build_resources');resource.rank=lambda p:0;resource.identity=lambda p:p
        source=Path('D:/Codex/artifacts/task-20261122-v102-api-upstream-integration/evidence/d3cc-validation-tools-v8/kernel_job.py')
        spec=importlib.util.spec_from_file_location('frozen_launch_for_puretest',source)
        module=importlib.util.module_from_spec(spec)
        with patch.dict('sys.modules',{'build_resources':resource}):spec.loader.exec_module(module)
        api=module.Win64API.__new__(module.Win64API);api.launchCleanupErrors=[]
        events=[];last=[122];counter=[10];record={}
        c=Context(record,lambda:None,[],['root'],lambda:last[0],lambda n:last.__setitem__(0,n))
        def duplicate(*args):counter[0]+=1;args[3]._obj.value=counter[0];return 1
        def init(*args):args[3]._obj.value=64;last[0]=122;return 0 if args[0] is None else 1
        def create(*args):
            self.assertEqual(args[5],0x80400)
            args[-1]._obj.process=500;args[-1]._obj.pid=77;args[-1]._obj.thread=501
            events.append('create');last[0]=5;return 1
        def image(h):
            events.append('image')
            c.native('QueryFullProcessImageNameW',lambda *x:1)(h,0,None,None)
            return 'C:/fixed/python.exe'
        api.createJob=lambda *x:9;api.query=lambda *x:1;api.init=init;api.update=lambda *x:1
        api.delete=lambda *x:events.append('cleanup-attrs');api.create=create
        api._close=lambda h:events.append('cleanup-close') or 1
        api.current=lambda:1;api.stdInput=lambda n:None;api.duplicate=duplicate
        api.openProcess=lambda *x:None;api.isMember=lambda *x:1;api.pidOf=lambda *x:77
        api.times=lambda *x:1;api.waitOne=lambda *x:0;api.exit=lambda *x:1;api.image=image
        instrument(api,c);c.after_create=lambda pi:c.capture_root(pi,api.image)
        msvcrt=types.ModuleType('msvcrt');msvcrt.get_osfhandle=lambda n:2
        with tempfile.TemporaryDirectory() as d,patch.dict('sys.modules',{'msvcrt':msvcrt}),\
             patch.object(module.C,'get_last_error',lambda:last[0],create=True):
            with (Path(d)/'tiny.log').open('wb') as log:
                result=api.launch(9,['C:/fixed/python.exe'],'C:/fixed',{},log,log)
        self.assertEqual(result,(500,77));self.assertEqual(events[:2],['create','image'])
        self.assertTrue(all(x.startswith('cleanup') for x in events[2:]))
        self.assertEqual(last[0],5)

    def test_real_domain_root_reference_nonroot_failure_name_and_wrong_image(self):
        support=Path('D:/Codex/artifacts/task-20261122-v102-api-upstream-integration/evidence/d3cc-validation-tools-v8/kernel_job.py')
        tree=ast.parse(support.read_text());cls=next(x for x in tree.body if isinstance(x,ast.ClassDef) and x.name=='KernelDomain')
        fn=next(x for x in cls.body if isinstance(x,ast.FunctionDef) and x.name=='prove_root')
        namespace={};exec(compile(ast.Module(body=[fn],type_ignores=[]),'original-core-prove-root','exec'),namespace)
        class Core:prove_root=namespace['prove_root']
        kernel=types.ModuleType('kernel_job');kernel.KernelDomain=Core;kernel.Win64API=object;kernel.H=object;kernel.U32=object
        resource=types.ModuleType('build_resources');resource.sample=lambda:None;resource.heavy=lambda p:False
        resource.identity=lambda p:(p['ProcessId'],p['CreationDate']);resource.now=lambda:'now'
        safety=types.ModuleType('qa_owner_safety');safety.attempt=safety.remember_error=safety.wait_naturally=lambda *x:None
        with patch.dict(owner.__dict__,TRACE_CONTEXT=Context({},lambda:None,[],['root'],lambda:0)),\
             patch.dict('sys.modules',{'kernel_job':kernel,'build_resources':resource,'qa_owner_safety':safety}):
            Domain=owner.runtime()[0];c=owner.TRACE_CONTEXT
            c.capture_root(types.SimpleNamespace(process=500,pid=77),lambda h:self.query(c,h))
            calls=[]
            class API:
                artifact_job=9;launchCleanupErrors=[]
                def info(self,h):return (77,123,None) if h==500 else (88,124,None)
                def member(self,h,j):return True
                def image(self,h):calls.append(h);raise OSError(31,'nonroot image31')
            domain=Domain.__new__(Domain);domain.api=API();domain.job=9
            domain.root=types.SimpleNamespace(h=500,pid=77);domain.receipt=types.SimpleNamespace(pid=77)
            domain.handles={};domain.certified=set();domain.events=[]
            domain.expected=os.path.normcase(str(Path('C:/fixed/python.exe').resolve()))
            domain.prove_root()
            root=dict(ProcessId=77,CreationDate='birth',Name='python.exe')
            self.assertTrue(domain.allowed(root,{resource.identity(root)},{domain.expected}))
            self.assertEqual(calls,[])
            self.assertFalse(domain.allowed(dict(root,Name='wrong.exe'),{resource.identity(root)},{domain.expected}))
            domain.expected=os.path.normcase(str(Path('C:/other/python.exe').resolve()))
            with self.assertRaises(AssertionError):domain.prove_root()
            child=dict(ProcessId=88,CreationDate='child',Name='readelf.exe');domain.handles[(88,124)]=600
            with self.assertRaises(OSError):domain.allowed(child,{resource.identity(child)},set())
            self.assertEqual(calls,[600])
            domain.api.image=lambda h:'C:/fixed/python.exe'
            python_child=dict(child,Name='python.exe')
            self.assertFalse(domain.allowed(python_child,{resource.identity(python_child)},
                                           {os.path.normcase(str(Path('C:/fixed/python.exe').resolve()))}))
    def query(self,c,h,path='C:/fixed/python.exe',failure=None):
        def native(*args):
            if failure:raise OSError(failure,'image failure')
            return 1
        def image(handle):
            c.native('QueryFullProcessImageNameW',native)(handle,0,None,None)
            return path
        return c.method('image',image,h)

    def test_early_create_original_error_before_cleanup_and_later_core_proof(self):
        for code in (None,5,31):
            with self.subTest(code=code):
                last=[5];events=[];record={}
                c=Context(record,lambda:None,[],['root'],lambda:last[0],lambda x:last.__setitem__(0,x))
                pi=types.SimpleNamespace(process=500,pid=77)
                def capture(receipt):
                    events.append('query')
                    return c.capture_root(receipt,lambda h:self.query(c,h,failure=code))
                c.after_create=capture
                def create(*args):events.append('create');last[0]=5;return 1
                try:
                    if code:
                        with self.assertRaises(OSError):c.native('CreateProcessW',create)(None,types.SimpleNamespace(_obj=pi))
                    else:c.native('CreateProcessW',create)(None,types.SimpleNamespace(_obj=pi))
                    self.assertEqual(last[0],5)
                finally:events.append('cleanup')
                self.assertEqual(events,['create','query','cleanup'])
                if code:continue
                api=types.SimpleNamespace(artifact_job=10,info=lambda h:(77,123,None),member=lambda h,j:True)
                self.assertEqual(c.prior_root_image(500,api,77),'C:/fixed/python.exe')
                self.assertEqual(c.root_image['birth'],123)
                self.assertEqual(record['apiCalls'][-1]['operation'],'root.image_reference')

    def test_missing_partial_closed_reused_wrong_birth_member_and_taint(self):
        for variant in ('partial','closed','pid','birth','member','taint','no-image'):
            with self.subTest(variant=variant):
                c=Context({},lambda:None,[],['root'],lambda:0)
                if variant=='partial':
                    with self.assertRaises(AssertionError):c.capture_root(types.SimpleNamespace(process=None,pid=77),lambda h:'fake')
                    continue
                c.capture_root(types.SimpleNamespace(process=500,pid=77),lambda h:self.query(c,h))
                api=types.SimpleNamespace(artifact_job=10,info=lambda h:(77,123,None),member=lambda h,j:True)
                c.prior_root_image(500,api,77)
                if variant=='closed':c.closed(500);c.handles['500']=(77,123,'root')
                if variant=='pid':api.info=lambda h:(88,123,None)
                if variant=='birth':api.info=lambda h:(77,456,None)
                if variant=='member':api.member=lambda h,j:False
                if variant=='taint':c.root_tainted=True
                if variant=='no-image':c.root_image=None
                with self.assertRaises(AssertionError):c.prior_root_image(500,api,77)

    def test_first_api_error_full_snapshot_and_later_real_failure_cannot_fallback(self):
        with tempfile.TemporaryDirectory() as d:
            record={};snapshots=[]
            c=Context(record,lambda:snapshots.append(json.loads(json.dumps(record))),[],['root'],lambda:31,
                      journal_path=Path(d)/'api.jsonl')
            c.capture_root(types.SimpleNamespace(process=500,pid=77),lambda h:self.query(c,h))
            api=types.SimpleNamespace(artifact_job=9,info=lambda h:(77,123,None),member=lambda *x:True)
            c.prior_root_image(500,api,77)
            with self.assertRaises(OSError):self.query(c,500,failure=31)
            self.assertTrue(snapshots and snapshots[0]['apiCalls'][-1]['status']=='exception')
            with self.assertRaises(AssertionError):c.prior_root_image(500,api,77)
            c.finish()

    def test_journal_attempt_durable_before_action_exact_replay_and_closed_history(self):
        with tempfile.TemporaryDirectory() as d:
            record={};c=Context(record,lambda:None,[],['root'],lambda:31,journal_path=Path(d)/'api.jsonl')
            def action():
                rows=[json.loads(x) for x in c.journal.path.read_text().splitlines()]
                self.assertEqual(next(x for x in rows if x['kind']=='api')['value']['status'],'attempt')
                return 1
            c.call('fakeAPI',action,targetPID=77)
            c.handles['5']=(77,123,'root');c.closed(5);c.finish()
            self.assertTrue(record['apiJournal']['fullTraceEquivalent'])
            self.assertEqual(record['apiJournal']['apiCalls'],2)
            self.assertEqual(record['apiJournalStatus'],'COMPLETE_VERIFIED')

    def test_write_flush_fsync_corruption_and_sequence_fail_closed(self):
        for failure in ('write','short-write','flush','fsync'):
            with self.subTest(failure=failure),tempfile.TemporaryDirectory() as d:
                record={};errors=[];c=Context(record,lambda:None,errors,['root'],lambda:0,journal_path=Path(d)/'api.jsonl')
                stream=c.journal.stream
                class Fail:
                    def write(self,x):
                        if failure=='write':raise OSError('write')
                        if failure=='short-write':return stream.write(x[:2])
                        return stream.write(x)
                    def flush(self):
                        if failure=='flush':raise OSError('flush')
                        return stream.flush()
                    def fileno(self):return stream.fileno()
                    def close(self):stream.close()
                c.journal.stream=Fail()
                with patch('api_context.os.fsync',side_effect=OSError('fsync') if failure=='fsync' else lambda n:None):
                    launched=[]
                    with self.assertRaises(OSError):c.call('CreateProcessW',lambda:launched.append(1))
                    self.assertEqual(launched,[])
                    cleaned=[];c.call('CloseHandle',lambda:cleaned.append(1))
                    self.assertEqual(cleaned,[1])
                    with self.assertRaises(OSError):c.finish()
                self.assertTrue(errors and c.broken)
        for bad in ('partial','sequence','checksum'):
            with tempfile.TemporaryDirectory() as d:
                r={};c=Context(r,lambda:None,[],['root'],lambda:0,journal_path=Path(d)/'api.jsonl')
                c.call('api',lambda:1);c.journal.stream.flush()
                p=c.journal.path;data=p.read_bytes()
                if bad=='partial':p.write_bytes(data[:-1])
                elif bad=='sequence':p.write_bytes(data.replace(b'"seq": 0',b'"seq": 7',1))
                else:p.write_bytes(data.replace(b'"status": "attempt"',b'"status": "changed"',1))
                with self.assertRaises((ValueError,OSError)):c.finish()

if __name__=='__main__':unittest.main()
