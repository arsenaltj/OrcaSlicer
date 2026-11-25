"""Artifact-local native-call evidence. Never opens or changes Windows APIs."""
import hashlib
import json
import traceback
import os
import time
import datetime
from pathlib import Path


class Journal:
    """One command/thread; append complete checksummed records, never truncate."""
    def __init__(self,path):
        self.path=Path(path);self.stream=self.path.open('xb');self.rows=[];self.failed=False
    def append(self,kind,value):
        if self.failed:raise OSError('journal permanently failed')
        payload=dict(seq=len(self.rows),kind=kind,value=json.loads(json.dumps(value)))
        digest=hashlib.sha256(json.dumps(payload,sort_keys=True).encode()).hexdigest()
        row=dict(payload,sha256=digest)
        try:
            encoded=(json.dumps(row,sort_keys=True)+'\n').encode()
            if self.stream.write(encoded)!=len(encoded):raise OSError('short journal write')
            self.stream.flush();os.fsync(self.stream.fileno())
        except BaseException:
            self.failed=True;raise
        self.rows.append(row)
    def finish(self,record):
        self.stream.close()
        if self.failed:raise OSError('incomplete journal')
        rows=[]
        with self.path.open('rb') as source:
            for line in source:
                if not line.endswith(b'\n'):raise ValueError('partial journal line')
                row=json.loads(line);digest=row.pop('sha256')
                if row['seq']!=len(rows) or hashlib.sha256(json.dumps(row,sort_keys=True).encode()).hexdigest()!=digest:
                    raise ValueError('journal sequence/checksum differs')
                rows.append(row)
        expected=[dict(seq=x['seq'],kind=x['kind'],value=x['value']) for x in self.rows]
        if rows!=expected:raise ValueError('journal data differs')
        calls=[x['value'] for x in rows if x['kind']=='api']
        histories=[x['value'] for x in rows if x['kind']=='closed']
        if calls!=record.get('apiCalls',[]) or (histories[-1] if histories else [])!=record.get('closedHandleHistory',[]):
            raise ValueError('full trace/closed history not equivalent')
        with self.path.open('rb') as source:digest=hashlib.file_digest(source,'sha256').hexdigest()
        return dict(path=str(self.path),bytes=self.path.stat().st_size,sha256=digest,
                    records=len(rows),apiCalls=len(calls),closedUpdates=len(histories),fullTraceEquivalent=True,
                    apiCallsSha256=hashlib.sha256(json.dumps(calls,sort_keys=True).encode()).hexdigest(),
                    closedHistorySha256=hashlib.sha256(json.dumps(record.get('closedHandleHistory',[]),sort_keys=True).encode()).hexdigest())


class Context:
    def __init__(self, record, save, errors, argv, last_error, restore_error=lambda value:None,journal_path=None):
        self.record, self.save, self.errors = record, save, errors
        self.argv = hashlib.sha256(json.dumps(argv).encode()).hexdigest()
        self.last_error = last_error
        self.restore_error = restore_error
        self.phase = 'initialization'
        self.raw_index = None
        self.targets = {}
        self.handles = {}
        self.closed_history = []
        self.job = None
        self.frame = {}
        self.journal=Journal(journal_path) if journal_path is not None else None
        self.written_calls=0;self.written_history=None;self.broken=False
        self.after_create=None;self.root_image=None;self.root_tainted=False
        self.first_exception_snapshot=False

    def closed(self, handle):
        key = str(handle)
        if self.root_image and self.root_image['handle']==key:self.root_tainted=True
        previous = self.handles.pop(key,None)
        if previous is not None or key == self.job:
            self.closed_history.append(dict(handle=key,status='closed',historyOnly=True,
                previousIdentity=list(previous) if previous is not None else None,
                role='job' if key==self.job else previous[2]))
        if key == self.job:
            self.job = None
        self.record['closedHandleHistory'] = list(self.closed_history)
        last = self.last_error()
        self.persist()
        self.restore_error(last)

    def scalars(self, name, args):
        """Only input scalars/roles; never dereference private env or output buffers."""
        def arg(i):return args[i] if i < len(args) else None
        def scalar(i):
            value=arg(i)
            return value if value is None or isinstance(value,(str,int,bool,float)) else getattr(value,'value',None)
        def handle(i):return str(arg(i)) if arg(i) is not None else None
        def pointer(i):
            value=arg(i)
            null=value is None or (isinstance(value,int) and value==0)
            return dict(present=not null,type=type(value).__name__ if value is not None else 'null',identityProven=False)
        def count(i):
            value=getattr(arg(i),'_obj',None)
            return getattr(value,'value',None)
        if name=='OpenProcess':
            return dict(desiredAccess=scalar(0),inheritHandle=scalar(1),processId=scalar(2))
        if name=='WaitForSingleObject':return dict(processHandle=handle(0),timeoutMilliseconds=scalar(1))
        if name=='QueryInformationJobObject':
            return dict(jobHandle=handle(0),informationClass=scalar(1),buffer=pointer(2),bufferBytes=scalar(3),returnLength=pointer(4))
        if name=='DuplicateHandle':
            return dict(sourceProcessHandle=handle(0),sourceHandle=handle(1),targetProcessHandle=handle(2),
                targetHandleOutput=pointer(3),desiredAccess=scalar(4),inheritHandle=scalar(5),options=scalar(6))
        if name=='CreateProcessW':
            command=scalar(1)
            return dict(application=scalar(0),commandLine=command,
                processSecurity=pointer(2),threadSecurity=pointer(3),inheritHandles=scalar(4),creationFlags=scalar(5),
                environment=dict(present=arg(6) is not None,contents='PRIVATE_NOT_RECORDED'),
                currentDirectory=scalar(7),startupInfo=pointer(8),processReceiptOutput=pointer(9))
        if name=='CreateJobObjectW':return dict(security=pointer(0),name=scalar(1))
        if name=='InitializeProcThreadAttributeList':
            return dict(attributeList=pointer(0),attributeCount=scalar(1),flags=scalar(2),sizeOutput=pointer(3),sizeInput=count(3))
        if name=='UpdateProcThreadAttribute':
            return dict(attributeList=pointer(0),flags=scalar(1),attribute=scalar(2),value=pointer(3),
                        valueBytes=scalar(4),previousValue=pointer(5),returnSize=pointer(6))
        if name=='DeleteProcThreadAttributeList':return dict(attributeList=pointer(0))
        if name=='GetCurrentProcess':return {}
        if name=='GetStdHandle':return dict(standardHandleId=scalar(0))
        if name=='QueryFullProcessImageNameW':
            return dict(processHandle=handle(0),flags=scalar(1),imageOutput=pointer(2),sizeInOut=pointer(3),capacityCharacters=count(3))
        if name=='GetProcessTimes':
            return dict(processHandle=handle(0),creationTimeOutput=pointer(1),exitTimeOutput=pointer(2),
                        kernelTimeOutput=pointer(3),userTimeOutput=pointer(4))
        if name=='GetProcessId':return dict(processHandle=handle(0))
        if name=='IsProcessInJob':return dict(processHandle=handle(0),jobHandle=handle(1),memberOutput=pointer(2))
        if name=='GetExitCodeProcess':return dict(processHandle=handle(0),exitCodeOutput=pointer(1))
        if name=='CloseHandle':return dict(handle=handle(0))
        return dict(parameterContract='unknown')

    def persist(self):
        try:
            if self.journal is None:self.save()
            else:
                calls=self.record.get('apiCalls',[])
                for event in calls[self.written_calls:]:
                    self.journal.append('api',event);self.written_calls+=1
                history=self.record.get('closedHandleHistory',[])
                if history!=self.written_history:
                    self.journal.append('closed',history);self.written_history=json.loads(json.dumps(history))
        except BaseException as exc:
            self.broken=True
            self.record['apiJournalStatus']='INCOMPLETE_STICKY_FAILURE'
            self.errors.append(dict(stage='api_context_save', error=repr(exc)))
            try:self.save()
            except BaseException as save_error:self.errors.append(dict(stage='first_error_snapshot',error=repr(save_error)))

    def finish(self):
        if self.journal is not None:
            self.persist()
            try:self.record['apiJournal']=self.journal.finish(self.record)
            except BaseException:
                self.broken=True;self.record['apiJournalStatus']='INCOMPLETE_STICKY_FAILURE';raise
        if self.broken:raise OSError('API journal incomplete; cannot qualify')
        self.record['apiJournalStatus']='COMPLETE_VERIFIED' if self.journal else 'LEGACY_TEST_CONTEXT'

    def capture_root(self,receipt,query):
        assert not self.broken and not self.root_tainted and self.root_image is None,'root capture unavailable'
        assert receipt is not None and receipt.process and receipt.pid,'partial PI cannot certify image'
        handle=receipt.process;pid=int(receipt.pid)
        self.handles[str(handle)]=(pid,None,'root')
        image=self.call('root.early_image',lambda:query(handle),targetPID=pid,handle=str(handle),handleRole='root')
        assert isinstance(image,str) and image and not self.broken and not self.root_tainted,'missing actual early image'
        events=self.record['apiCalls']
        direct=[(i,e) for i,e in enumerate(events) if e['operation']=='QueryFullProcessImageNameW'
                and e['status']=='outcome' and e.get('handle')==str(handle) and e.get('result') not in ('0','False','None')]
        assert len(direct)==1,'missing/ambiguous actual root query outcome'
        index,event=direct[0]
        self.root_image=dict(handle=str(handle),pid=pid,birth=None,image=image,eventIndex=index,
                             capturedMonotonicNs=event['completedMonotonicNs'],closedGenerationCount=len(self.closed_history))
        self.record['rootEarlyImage']=dict(self.root_image)

    def prior_root_image(self,handle,api,pid):
        fact=self.root_image
        assert fact and not self.root_tainted and not self.broken and fact['handle']==str(handle) and fact['pid']==pid,'no continuous early root fact'
        assert str(handle) in self.handles and not any(x['handle']==str(handle) for x in self.closed_history),'closed/reused root handle'
        actual=api.info(handle)
        assert actual[0]==pid and api.member(handle,api.artifact_job),'root identity/member differs'
        assert not self.broken and not self.root_tainted,'root proof became incomplete'
        assert fact['birth'] is None or fact['birth']==actual[1],'root birth generation conflict'
        fact['birth']=actual[1]
        self.call('root.image_reference',lambda:dict(evidence='PRIOR_DIRECT_SAME_GENERATION',
             eventIndex=fact['eventIndex'],capturedMonotonicNs=fact['capturedMonotonicNs']),
             targetPID=pid,knownBirth=actual[1],handle=str(handle),handleRole='root')
        self.record['rootEarlyImage']=dict(fact)
        assert not self.broken,'root reference journal incomplete'
        return fact['image']

    def rows(self, rows, index):
        self.raw_index = index
        self.targets = {p['ProcessId']: p.get('CreationDate') for p in rows if p.get('ProcessId')}

    def call(self, operation, action, **target):
        event = dict(operation=operation, phase=self.phase, rawSampleIndex=self.raw_index,
                     rootArgvSha256=self.argv, targetPID=None, knownBirth=None,
                     handle=None, handleRole='unknown', job=self.job, requestedAccess=None)
        event.update(self.frame)
        event.update(target)
        event['status'] = 'attempt'
        event['monotonicNs']=time.monotonic_ns();event['utc']=datetime.datetime.now(datetime.timezone.utc).isoformat()
        calls = self.record.setdefault('apiCalls', [])
        calls.append(event)
        prior = self.last_error()
        self.persist()  # failure remains sticky but cannot block native cleanup/wait
        self.restore_error(prior)
        if self.broken and operation=='CreateProcessW':raise OSError('journal failure prohibits new launch')
        invoked_ns=time.monotonic_ns()
        try:
            result = action()
            last = self.last_error()  # capture before serialization/diagnostics/another API
            completed_ns=time.monotonic_ns()
        except BaseException as exc:
            last = self.last_error()
            completed_ns=time.monotonic_ns()
            calls.append(dict(event, status='exception', lastError=last, error=repr(exc),
                              winerror=getattr(exc,'winerror',None), traceback=traceback.format_exc(),
                              invokedMonotonicNs=invoked_ns,completedMonotonicNs=completed_ns))
            self.persist()
            if not self.first_exception_snapshot:
                self.first_exception_snapshot=True
                try:self.save()
                except BaseException as save_error:self.errors.append(dict(stage='first_api_exception_snapshot',error=repr(save_error)))
            self.restore_error(last)
            raise
        calls.append(dict(event, status='outcome', lastError=last,
                          result=repr(result),invokedMonotonicNs=invoked_ns,completedMonotonicNs=completed_ns))
        self.persist()
        self.restore_error(last)
        return result

    def method(self, name, action, *args, **kwargs):
        before = self.frame
        frame = dict(targetPID=None,knownBirth=None,observedBirth=None,handle=None,
                     handleRole='unknown',requestedAccess=None,job=self.job)
        if name == 'opened':
            pid = args[0]
            frame.update(targetPID=pid, knownBirth=None, observedBirth=self.targets.get(pid),handle=None,
                         handleRole='candidate', requestedAccess=0x101000)
        elif name in ('info','image','member','signaled','code','close') and args:
            handle = args[0]
            pid, birth, role = self.handles.get(str(handle), (None,None,'unknown'))
            frame.update(targetPID=pid, knownBirth=birth, handle=str(handle),handleRole=role)
            if str(handle)==self.job:
                frame.update(targetPID=None,knownBirth=None,handleRole='job',job=str(handle))
            if name=='member':frame['job']=str(args[1])
        elif name in ('job','active'):
            frame.update(handleRole='job',handle=str(args[0]) if args else None,
                         job=str(args[0]) if args else None)
        elif name == 'launch':
            frame.update(handleRole='root-create',targetPID=None,knownBirth=None)
        self.frame = frame
        try:
            result = self.call('api.'+name, lambda: action(*args, **kwargs))
            if name == 'opened' and result:
                self.handles[str(result)] = (args[0],None,'member-candidate')
            elif name == 'info':
                self.handles[str(args[0])] = (result[0],result[1],frame.get('handleRole','unknown'))
            elif name == 'job':
                self.job = str(result)
            elif name == 'launch' and isinstance(result,tuple):
                self.handles[str(result[0])] = (result[1],None,'root')
            elif name == 'close':
                self.closed(args[0])
            return result
        except BaseException:
            if name=='image' and (self.root_image is None or self.root_image['handle']==str(args[0])):self.root_tainted=True
            raise
        finally:
            self.frame = before

    def native(self, name, action):
        def invoke(*args):
            target = dict(actualParameters=self.scalars(name,args))
            if name == 'OpenProcess':
                target.update(targetPID=args[2],knownBirth=None,observedBirth=self.targets.get(args[2]),
                              handle=None,handleRole='candidate',requestedAccess=args[0])
            elif name=='DuplicateHandle':
                target.update(targetPID=None,knownBirth=None,handle=str(args[1]),handleRole='duplicate-source',
                              requestedAccess=args[4])
            elif name in ('QueryInformationJobObject','CreateJobObjectW'):
                target.update(targetPID=None,knownBirth=None,observedBirth=None,handleRole='job',
                              handle=str(args[0]) if name=='QueryInformationJobObject' else None,
                              job=str(args[0]) if name=='QueryInformationJobObject' else None,requestedAccess=None)
            elif name in ('GetProcessTimes','GetProcessId','IsProcessInJob','WaitForSingleObject','GetExitCodeProcess','CloseHandle'):
                h=str(args[0]);pid,birth,role=self.handles.get(h,(None,None,'unknown'))
                target.update(handle=h,targetPID=pid,knownBirth=birth,observedBirth=None,handleRole=role,requestedAccess=None)
                if h==self.job:target.update(targetPID=None,knownBirth=None,handleRole='job',job=h)
                if name=='IsProcessInJob':target['job']=str(args[1])
            result = self.call(name, lambda: action(*args), **target)
            if name=='CloseHandle' and result:
                self.closed(args[0])
            if name == 'CreateProcessW':
                receipt = getattr(args[-1],'_obj',None)
                if receipt is not None and getattr(receipt,'process',None):
                    self.handles[str(receipt.process)] = (int(receipt.pid),None,'root')
                    self.call('native.launch_receipt', lambda:dict(processHandle=str(receipt.process),pid=int(receipt.pid)),
                              targetPID=int(receipt.pid), handle=str(receipt.process),handleRole='root')
                if result:
                    original_error=self.last_error()
                    try:
                        assert self.after_create is not None,'missing genuine root capture hook'
                        self.after_create(receipt)
                    finally:self.restore_error(original_error)
            return result
        return invoke


def instrument(api, context):
    """Wrap existing bound functions without changing kernel source or flags."""
    aliases = {'createJob':'CreateJobObjectW','query':'QueryInformationJobObject',
        'init':'InitializeProcThreadAttributeList','update':'UpdateProcThreadAttribute',
        'delete':'DeleteProcThreadAttributeList','create':'CreateProcessW','_close':'CloseHandle',
        'current':'GetCurrentProcess','stdInput':'GetStdHandle','duplicate':'DuplicateHandle',
        'openProcess':'OpenProcess','isMember':'IsProcessInJob','pidOf':'GetProcessId',
        'times':'GetProcessTimes','waitOne':'WaitForSingleObject','exit':'GetExitCodeProcess'}
    for attr, name in aliases.items():
        setattr(api, attr, context.native(name, getattr(api,attr)))
    for name in ('job','launch','opened','info','image','member','signaled','code','active','close'):
        original = getattr(api,name)
        setattr(api,name,lambda *args, _name=name, _fn=original, **kw:context.method(_name,_fn,*args,**kw))
    return api
