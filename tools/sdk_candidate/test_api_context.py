"""API failure context tests: real frozen support AST, fake APIs only."""
import ast
from pathlib import Path
import tempfile
import types
import unittest
from unittest.mock import patch
import artifact_owner as owner
from api_context import Context

SUPPORT=Path('D:/Codex/artifacts/task-20261122-v102-api-upstream-integration/evidence/d3cc-validation-tools-v8/kernel_job.py')


def method(cls,name):
    tree=ast.parse(SUPPORT.read_text(encoding='utf8'))
    c=next(n for n in tree.body if isinstance(n,ast.ClassDef) and n.name==cls)
    fn=next(n for n in c.body if isinstance(n,ast.FunctionDef) and n.name==name)
    return fn


class ContextTests(unittest.TestCase):
    def test_frozen_support_api_methods_multitarget5_31(self):
        class Value:
            def __init__(self,value=0):self.value=value
        for family,alias,native_name in [('opened','openProcess','OpenProcess'),
            ('info','times','GetProcessTimes'),('info','pidOf','GetProcessId'),
            ('member','isMember','IsProcessInJob'),('active','query','QueryInformationJobObject'),
            ('signaled','waitOne','WaitForSingleObject'),('code','exit','GetExitCodeProcess'),('close','_close','CloseHandle')]:
            for target in (77,88):
                for code in (5,31):
                    class FakeError(OSError):
                        def __init__(self,n):super().__init__(n,'injected');self.winerror=n
                    C=types.SimpleNamespace(get_last_error=lambda:code,WinError=FakeError,byref=lambda x:x,
                                            c_int=Value,sizeof=lambda x:48)
                    ns=dict(C=C,U64=Value,U32=Value,ACCOUNTING=Value)
                    exec(compile(ast.Module(body=[method('Win64API','ok'),method('Win64API',family)],type_ignores=[]),
                                 'frozen-support-api','exec'),ns)
                    record={};ctx=Context(record,lambda:None,[],['root'],lambda:code)
                    ctx.rows([dict(ProcessId=target,CreationDate='birth')],2)
                    ctx.handles[str(target)]=(target,'birth','member')
                    api=types.SimpleNamespace(ok=lambda x:ns['ok'](None,x))
                    def fail(*args):raise FakeError(code)
                    setattr(api,alias,ctx.native(native_name,fail))
                    if family=='info' and alias=='pidOf':api.times=lambda *args:1
                    args=(target,) if family not in ('member',) else (target,'job')
                    with self.assertRaises(FakeError):ctx.method(family,lambda *x:ns[family](api,*x),*args)
                    events=[x for x in record['apiCalls'] if x['operation']==native_name and x['status']=='exception']
                    self.assertTrue(events)
                    self.assertEqual(events[0]['targetPID'],target if family!='active' else None)
                    self.assertEqual(events[0]['lastError'],code)

    def test_actual_observe_owner_multi_target_errors5_31(self):
        ns=dict(key=lambda p:(p['ProcessId'],p['birth']),identity=lambda p:(p['ProcessId'],p['CreationDate'],p.get('CommandLine')))
        exec(compile(ast.Module(body=[method('KernelDomain','observe')],type_ignores=[]),'frozen-support-observe','exec'),ns)
        rows=[dict(ProcessId=n,birth=n,CreationDate='birth-'+str(n),ParentProcessId=1,Name='java.exe') for n in (501,502)]
        for target in (501,502):
            for code in (5,31):
                with self.subTest(target=target,code=code),tempfile.TemporaryDirectory() as d:
                    state=dict(zero=0,wait=False,closed=False);record={};saved=[]
                    class API:
                        def opened(self,pid):
                            def native():
                                if pid==target:
                                    ex=OSError(code,'injected');ex.winerror=code;raise ex
                                return None
                            return owner.TRACE_CONTEXT.method('opened',
                                lambda p:owner.TRACE_CONTEXT.native('OpenProcess',lambda *a:native())(0x101000,False,p),pid)
                    class Child:
                        pid=42
                        def poll(self):return 0
                    class Domain:
                        def __init__(self):self.api=API();self.certified=set();self.handles={}
                        def launch(self,*args):return Child()
                        def prove_root(self):pass
                        observe=ns['observe']
                        def settled(self):state['zero']+=1;return state['zero']>=2
                        def snapshot(self):return dict(zero=state['zero'])
                        def close(self):state['closed']=True
                        def recover_root(self):return Child()
                    def remember(err,label,ex):err.append(dict(stage=label,error=repr(ex)))
                    def attempt(err,label,fn):
                        try:fn();return True
                        except BaseException as ex:remember(err,label,ex);return False
                    def wait(child,monitor,err,pause):state['wait']=True;monitor();return 0
                    runtime=(Domain,lambda:dict(freeKiB=10*1024**2,processes=rows),lambda p:True,
                             ns['identity'],lambda:'now',attempt,remember,wait)
                    def save():saved.append(len(record.get('apiCalls',[])))
                    with patch.object(owner,'runtime',return_value=runtime),patch.object(owner.time,'sleep',lambda n:None):
                        with self.assertRaises(AssertionError):
                            owner.run_owned(['exact'],Path(d)/'log',record,save,Path(d)/'cancel',lambda:None,
                                            permitted_commands=[['exact']],permitted_images=['exact'])
                    failures=[x for x in record['apiCalls'] if x['operation']=='OpenProcess' and x['status']=='exception']
                    self.assertTrue(failures and state['wait'] and state['closed'])
                    for event in failures:
                        self.assertEqual(event['targetPID'],target)
                        self.assertIsNone(event['knownBirth'])
                        self.assertEqual(event['observedBirth'],'birth-'+str(target))
                        self.assertEqual(event['requestedAccess'],0x101000)
                        self.assertIsNotNone(event['rawSampleIndex'])
                        self.assertIn('Traceback',event['traceback'])
                    self.assertTrue(saved)

    def test_all_native_families_multihandle_immediate_error_and_save_failure(self):
        families={'opened':'OpenProcess','image':'QueryFullProcessImageNameW',
                  'info':'GetProcessTimes','member':'IsProcessInJob','active':'QueryInformationJobObject',
                  'signaled':'WaitForSingleObject','code':'GetExitCodeProcess','close':'CloseHandle',
                  'job':'CreateJobObjectW','launch':'CreateProcessW','receipt':'launch_receipt',
                  'recover':'recover_root','pid':'GetProcessId'}
        for high,low in families.items():
            for target in (77,88):
                for code in (5,31):
                    record={};errors=[];last=[code];saves=[]
                    def save():
                        saves.append(len(record.get('apiCalls',[])))
                        last[0]=999 # simulates diagnostics clobbering TLS LastError
                        raise OSError('record save')
                    context=Context(record,save,errors,['exact'],lambda:last[0],lambda n:last.__setitem__(0,n))
                    context.phase='family-test';context.rows([dict(ProcessId=target,CreationDate='b'+str(target))],3)
                    context.handles[str(target)]=(target,'b'+str(target),'member')
                    def fail(*args):last[0]=code;raise OSError(code,'failure')
                    args=() if high in ('job','launch','receipt','recover') else (target,)
                    if high=='member':args=(target,'job')
                    def actual(*args):
                        native_args=(0x101000,False,target) if high=='opened' else args
                        return context.native(low,fail)(*native_args)
                    with self.assertRaises(OSError):context.method(high,actual,*args)
                    event=next(x for x in record['apiCalls'] if x['operation']==low and x['status']=='exception')
                    self.assertEqual(event['lastError'],code)
                    self.assertEqual(last[0],code)
                    self.assertEqual(event['rawSampleIndex'],3)
                    self.assertTrue(event['traceback'] and errors and saves)
                    if high in ('image','info','member','signaled','code','close','pid'):
                        self.assertEqual(event['targetPID'],target)
                    elif high in ('job','launch','receipt','recover'):
                        self.assertIsNone(event['targetPID'])

    def test_success_preserves_last_error_after_record_io(self):
        last=[31];record={}
        def save():last[0]=999
        c=Context(record,save,[],['exact'],lambda:last[0],lambda n:last.__setitem__(0,n))
        def call():last[0]=5;return 0
        self.assertEqual(c.call('OpenProcess',call,targetPID=7),0)
        self.assertEqual(last[0],5)
        self.assertEqual(record['apiCalls'][-1]['lastError'],5)

    def test_fresh_targets_rawindex_job_and_member_never_inherit(self):
        record={};c=Context(record,lambda:None,[],['root'],lambda:5)
        c.handles['11']=(77,123,'member');c.handles['22']=(88,456,'member');c.job='99'
        c.frame=dict(targetPID=77,knownBirth=123,handle='11',handleRole='member',job='old')
        c.rows([dict(ProcessId=88,CreationDate='raw88')],8)
        c.method('active',lambda h:c.native('QueryInformationJobObject',lambda *x:0)(h,1),'99')
        event=[x for x in record['apiCalls'] if x['operation']=='QueryInformationJobObject'][-1]
        self.assertIsNone(event['targetPID']);self.assertIsNone(event['knownBirth'])
        self.assertEqual((event['job'],event['handleRole'],event['rawSampleIndex']),('99','job',8))
        c.method('member',lambda h,j:c.native('IsProcessInJob',lambda *x:1)(h,j),'22','100')
        event=[x for x in record['apiCalls'] if x['operation']=='IsProcessInJob'][-1]
        self.assertEqual((event['targetPID'],event['knownBirth'],event['handle'],event['job']),(88,456,'22','100'))
        c.method('opened',lambda p:c.native('OpenProcess',lambda *x:None)(0x101000,False,p),88)
        event=[x for x in record['apiCalls'] if x['operation']=='OpenProcess'][-1]
        self.assertEqual(event['targetPID'],88);self.assertIsNone(event['knownBirth'])
        self.assertIsNone(event['handle']);self.assertEqual(event['observedBirth'],'raw88')


if __name__=='__main__':unittest.main()
