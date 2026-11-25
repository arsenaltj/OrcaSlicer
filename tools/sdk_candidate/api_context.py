"""Artifact-local native-call evidence. Never opens or changes Windows APIs."""
import hashlib
import json
import traceback


class Context:
    def __init__(self, record, save, errors, argv, last_error, restore_error=lambda value:None):
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

    def closed(self, handle):
        key = str(handle)
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
            self.save()
        except BaseException as exc:
            self.errors.append(dict(stage='api_context_save', error=repr(exc)))

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
        calls = self.record.setdefault('apiCalls', [])
        calls.append(event)
        prior = self.last_error()
        self.persist()  # failure remains sticky but cannot block native cleanup/wait
        self.restore_error(prior)
        try:
            result = action()
            last = self.last_error()  # capture before serialization/diagnostics/another API
        except BaseException as exc:
            last = self.last_error()
            calls.append(dict(event, status='exception', lastError=last, error=repr(exc),
                              winerror=getattr(exc,'winerror',None), traceback=traceback.format_exc()))
            self.persist()
            self.restore_error(last)
            raise
        calls.append(dict(event, status='outcome', lastError=last,
                          result=repr(result)))
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
