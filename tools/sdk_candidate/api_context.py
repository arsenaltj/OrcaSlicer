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
        self.job = None
        self.frame = {}

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
        self.persist()  # failure remains sticky but cannot block native cleanup/wait
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
        frame = dict(before)
        if name == 'opened':
            pid = args[0]
            frame.update(targetPID=pid, knownBirth=self.targets.get(pid), handle=None,
                         handleRole='candidate', requestedAccess=0x101000)
        elif name in ('info','image','member','signaled','code','close') and args:
            handle = args[0]
            pid, birth, role = self.handles.get(str(handle), (None,None,'unknown'))
            frame.update(targetPID=pid, knownBirth=birth, handle=str(handle),handleRole=role)
        elif name in ('job','active'):
            frame.update(handleRole='job',handle=str(args[0]) if args else None)
        elif name == 'launch':
            frame.update(handleRole='root-create',targetPID=None,knownBirth=None)
        self.frame = frame
        try:
            result = self.call('api.'+name, lambda: action(*args, **kwargs))
            if name == 'opened' and result:
                self.handles[str(result)] = (args[0],self.targets.get(args[0]),'member-candidate')
            elif name == 'info':
                self.handles[str(args[0])] = (result[0],result[1],frame.get('handleRole','unknown'))
            elif name == 'job':
                self.job = str(result)
            elif name == 'launch' and isinstance(result,tuple):
                self.handles[str(result[0])] = (result[1],None,'root')
            return result
        finally:
            self.frame = before

    def native(self, name, action):
        def invoke(*args):
            target = {}
            if name == 'OpenProcess':
                target = dict(targetPID=args[2],knownBirth=self.targets.get(args[2]),requestedAccess=args[0])
            result = self.call(name, lambda: action(*args), **target)
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
