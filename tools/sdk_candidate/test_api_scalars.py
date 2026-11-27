"""Arbitration repair2 probes; all native callables are fake."""
import unittest
import ast
import types
from api_context import Context
import test_api_context


class ScalarTests(unittest.TestCase):
    def context(self):
        record={}
        c=Context(record,lambda:None,[],['exact'],lambda:31)
        c.handles['77']=(501,123,'member');c.job='900'
        return c,record

    def test_review_wait_poll_vs_wait_and_duplicate_zero_flags(self):
        traces=[]
        for timeout in (0,1000):
            c,r=self.context()
            c.method('signaled',lambda h:c.native('WaitForSingleObject',lambda *args:258)(h,timeout),'77')
            traces.append(r['apiCalls'])
            self.assertEqual(traces[-1][-1]['status'],'outcome')
            native=next(x for x in traces[-1] if x['operation']=='WaitForSingleObject')
            self.assertEqual(native['actualParameters']['timeoutMilliseconds'],timeout)
        self.assertNotEqual(traces[0],traces[1])
        c,r=self.context()
        c.native('DuplicateHandle',lambda *args:1)('current','77','current',None,0,True,2)
        event=r['apiCalls'][-1]
        self.assertEqual(event['requestedAccess'],0)
        self.assertEqual(event['actualParameters'],dict(sourceProcessHandle='current',sourceHandle='77',
            targetProcessHandle='current',targetHandleOutput=dict(present=False,type='null',identityProven=False),
            desiredAccess=0,inheritHandle=True,options=2))

    def test_all_native_scalar_signatures_multitarget5_31(self):
        operations={
            'OpenProcess':lambda h:(0x101000,False,int(h)),
            'WaitForSingleObject':lambda h:(h,0),
            'QueryInformationJobObject':lambda h:('900',1,None,48,None),
            'DuplicateHandle':lambda h:('current',h,'current',None,0,True,2),
            'CreateProcessW':lambda h:('python.exe','python.exe exact.py',None,None,True,0x80400,'SECRET_ENV',r'D:\task',None,None),
            'CreateJobObjectW':lambda h:(None,None),
            'InitializeProcThreadAttributeList':lambda h:(None,2,0,None),
            'UpdateProcThreadAttribute':lambda h:(None,0,0x2000D,None,8,None,None),
            'DeleteProcThreadAttributeList':lambda h:(None,),
            'GetCurrentProcess':lambda h:(), 'GetStdHandle':lambda h:(0xFFFFFFF6,),
            'QueryFullProcessImageNameW':lambda h:(h,0,None,None),
            'GetProcessTimes':lambda h:(h,None,None,None,None),
            'GetProcessId':lambda h:(h,), 'IsProcessInJob':lambda h:(h,'900',None),
            'GetExitCodeProcess':lambda h:(h,None), 'CloseHandle':lambda h:(h,),
        }
        for operation,args_for in operations.items():
            for handle in ('77','88'):
                for code in (5,31):
                    with self.subTest(operation=operation,handle=handle,code=code):
                        c,r=self.context()
                        def fail(*args):raise OSError(code,'injected')
                        with self.assertRaises(OSError):c.native(operation,fail)(*args_for(handle))
                        self.assertEqual([x['status'] for x in r['apiCalls']],['attempt','exception'])
                        params=r['apiCalls'][0]['actualParameters']
                        self.assertNotIn('parameterContract',params)
                        self.assertNotIn('SECRET_ENV',str(r))
                        self.assertEqual(r['apiCalls'][1]['actualParameters'],params)
                        self.assertIn('Traceback',r['apiCalls'][1]['traceback'])

    def test_close_success_failure_reopen_and_unknown_reuse(self):
        c,r=self.context()
        c.native('CloseHandle',lambda h:1)('77')
        self.assertNotIn('77',c.handles)
        self.assertEqual(c.closed_history[-1]['status'],'closed')
        c.method('image',lambda h:'path','77')
        self.assertIsNone(r['apiCalls'][-1]['targetPID']);self.assertIsNone(r['apiCalls'][-1]['knownBirth'])
        c.rows([dict(ProcessId=999,CreationDate='raw999')],4)
        c.method('opened',lambda pid:'77',999)
        self.assertEqual(c.handles['77'],(999,None,'member-candidate'))
        c.method('image',lambda h:'path','77')
        self.assertEqual(r['apiCalls'][-1]['targetPID'],999)
        self.assertIsNone(r['apiCalls'][-1]['knownBirth'])
        c.method('info',lambda h:(999,456,None),'77')
        self.assertEqual(c.handles['77'][:2],(999,456))
        def fail(h):raise OSError(5,'closefailed')
        history=len(c.closed_history)
        with self.assertRaises(OSError):c.native('CloseHandle',fail)('77')
        self.assertIn('77',c.handles);self.assertEqual(len(c.closed_history),history)
        c.native('CloseHandle',lambda h:1)('900')
        self.assertIsNone(c.job)
        self.assertEqual(c.closed_history[-1]['role'],'job')

    def test_frozen_wait_and_close_api_scalar_and_lifecycle_controlflow(self):
        C=types.SimpleNamespace(get_last_error=lambda:5,WinError=lambda n:OSError(n,'api'))
        ns=dict(C=C)
        exec(compile(ast.Module(body=[test_api_context.method('Win64API','ok'),
            test_api_context.method('Win64API','signaled'),test_api_context.method('Win64API','close')],
            type_ignores=[]),'frozen-api-request-scalars','exec'),ns)
        c,r=self.context()
        api=types.SimpleNamespace(ok=lambda x:ns['ok'](None,x),
            waitOne=c.native('WaitForSingleObject',lambda *args:258),_close=c.native('CloseHandle',lambda h:1))
        for timeout in (0,1000):
            self.assertFalse(c.method('signaled',lambda h,t:ns['signaled'](api,h,t),'77',timeout))
        native=[x for x in r['apiCalls'] if x['operation']=='WaitForSingleObject' and x['status']=='attempt']
        self.assertEqual([x['actualParameters']['timeoutMilliseconds'] for x in native],[0,1000])
        c.method('close',lambda h:ns['close'](api,h),'77')
        self.assertNotIn('77',c.handles)
        c.method('image',lambda h:'unknown','77')
        self.assertIsNone(r['apiCalls'][-1]['knownBirth'])


if __name__=='__main__':unittest.main()
