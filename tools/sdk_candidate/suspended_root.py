"""Opt-in fixed SDK root lifecycle. Import is inert; no Windows API is opened here."""
import os
import sys
import threading
from pathlib import Path
from api_context import Context

PROFILE = 'SDK_SUSPENDED_ROOT_V1'
FLAGS = 0x80404
PLAN_SCHEMA = 'artifact-owner-suspended-plan-v1'
GO_SCHEMA = 'artifact-owner-suspended-go-v1'
REFERENCE = 'PRE_RESUME_DIRECT_SAME_CONTINUOUS_GENERATION'


def hold_creator(record):
    """Do not unwind/close original handles. Root must decide explicit recovery.

    This intentionally has no automatic completion timeout, API retry, controller,
    process transfer or permission change. An external process kill cannot be made
    safe by Python. Future actual GO must name the live creator and response window.
    """
    try:
        sys.stderr.write('CLEANUP_BLOCKED: original SDK PI retained by creator; root decision required\n')
        sys.stderr.flush()
    except BaseException:
        pass
    event = threading.Event()
    while True:
        try:
            event.wait()  # no process API, no retry, cannot return normally
        except BaseException:
            continue  # Ctrl-C must not silently discard the only handles


class SuspendedContext(Context):
    def native(self, name, action):
        if name not in ('ResumeThread', 'TerminateProcess'):
            return super().native(name, action)
        def invoke(*args):
            h = str(args[0])
            pid, birth, role = self.handles.get(h, (None, None, 'unknown'))
            def actual():
                if name == 'ResumeThread':
                    # The attempt row has now been durably persisted. A failed
                    # attempt write must not execute the suspended tool.
                    assert not self.broken, 'Resume attempt journal incomplete'
                    life = self.lifecycle
                    life.continuous()
                    assert not self.root_tainted and not life.resume_attempted, 'Resume authority lost'
                    life.resume_attempted = True
                    life.status['resumeAttempted'] = True
                return action(*args)
            return self.call(name, actual, actualParameters=self.scalars(name, args),
                             handle=h, targetPID=pid, knownBirth=birth, handleRole=role, job=self.job)
        return invoke

    def scalars(self, name, args):
        if name == 'ResumeThread':
            return dict(threadHandle=str(args[0]))
        if name == 'TerminateProcess':
            return dict(processHandle=str(args[0]), exitCode=args[1])
        return super().scalars(name, args)

    def prior_root_image(self, handle, api, pid):
        fact = self.root_image
        assert fact and not self.broken and not self.root_tainted, 'tainted suspended root'
        assert fact['handle'] == str(handle) and fact['pid'] == pid, 'different original root'
        assert self.handles.get(str(handle), ())[:2] == (pid, fact['birth']), 'different root generation'
        assert not any(x['handle'] == str(handle) for x in self.closed_history), 'closed/reused root'
        assert api.suspended.resumed, 'root not successfully resumed'
        self.call('root.image_reference', lambda: dict(evidence=REFERENCE, proof=fact),
                  targetPID=pid, knownBirth=fact['birth'], handle=str(handle), handleRole='root')
        assert not self.broken, 'root reference journal failed'
        return fact['image']

    def method(self, name, action, *args, **kwargs):
        fact = self.root_image
        original = bool(fact and args and str(args[0]) == fact['handle'])
        try:
            result = super().method(name, action, *args, **kwargs)
            if name == 'launch' and self.root_image:
                # Legacy Context registers a returned launch tuple with birth=None.
                # Preserve the direct proof already obtained before Resume; this is
                # a reference to that same original PI, never a new identity query.
                fact = self.root_image
                assert result == (self.lifecycle.pi.process, fact['pid']), 'launch return conflicts with original PI'
                self.lifecycle.continuous(include_thread=False)
                self.handles[fact['handle']] = (fact['pid'], fact['birth'], 'root')
            if original:
                if ((name == 'info' and result[:2] != (fact['pid'], fact['birth'])) or
                    (name == 'member' and result is not True) or
                    (name == 'image' and result != fact['image'])):
                    self.root_tainted = True
                    raise AssertionError('later direct root contradiction')
            return result
        except BaseException:
            if original and name in ('info', 'member', 'image'):
                self.root_tainted = True
            raise


class Lifecycle:
    def __init__(self, api, context, job, expected, cancel, hold=hold_creator):
        self.api, self.ctx, self.job = api, context, job
        context.lifecycle = self
        self.expected, self.cancel, self.hold = expected, cancel, hold
        self.pi = None
        self.original = None
        self.resume_attempted = self.resumed = self.terminate_attempted = False
        self.safe_to_release = self.blocked = False
        self.first_failure = None
        context.record['suspendedRoot'] = self.status = dict(
            profile=PROFILE, effectiveCreationFlags=FLAGS, resumeAttempted=False,
            terminateAttempted=False, qualification='PENDING', cleanup='PENDING',
            custodian='CURRENT_CREATING_PROCESS')

    def failure(self, exc):
        if self.first_failure is None:
            self.first_failure = repr(exc)
            self.status.update(firstFailure=self.first_failure, qualification='FAILED')

    def block(self, reason, exc=None):
        self.blocked = True
        self.failure(exc or RuntimeError(reason))
        self.status.update(cleanup='CLEANUP_BLOCKED', reason=reason,
                           originalPI=self.original, resumeAttempted=self.resume_attempted,
                           terminateAttempted=self.terminate_attempted,
                           next='KEEP_CREATOR_ALIVE_AND_ORIGINAL_HANDLES; ROOT_EXPLICIT_DECISION')
        try:
            self.ctx.call('root.cleanup_blocked', lambda: dict(self.status))
            self.ctx.save()
        except BaseException:
            pass
        self.hold(self.status)
        # An incorrectly returning injected custodian is never normal completion.
        hold_creator(self.status)

    def continuous(self, include_thread=True):
        assert self.pi is not None and self.original == dict(
            process=self.pi.process, thread=self.pi.thread, pid=int(self.pi.pid), tid=int(self.pi.tid)), 'PI changed'
        protected = (str(self.pi.process), str(self.job)) + ((str(self.pi.thread),) if include_thread else ())
        assert not any(x['handle'] in protected
                       for x in self.ctx.closed_history), 'original handle closed/reused'

    def created(self, pi):
        self.pi = pi
        self.original = dict(process=pi.process, thread=pi.thread, pid=int(pi.pid), tid=int(pi.tid)) if pi else None
        if not pi or not all(self.original.values()):
            self.block('successful Create returned partial PI')
        self.status['originalPI'] = dict(self.original)
        self.ctx.handles[str(pi.thread)] = (int(pi.pid), None, 'root-primary-thread')
        try:
            self.continuous()
            assert not self.ctx.broken and not self.ctx.root_tainted, 'Create evidence incomplete'
            start = len(self.ctx.record.get('apiCalls', []))
            pid, birth, _ = self.api.info(pi.process)
            assert pid == int(pi.pid) and type(birth) is int, 'original PID/birth unproved'
            assert self.api.member(pi.process, self.job) is True, 'original member out not true'
            image = self.api.image(pi.process)
            assert os.path.normcase(image) == self.expected, 'original image differs'
            events = self.ctx.record['apiCalls'][start:]
            direct = {}
            for name in ('GetProcessTimes', 'GetProcessId', 'IsProcessInJob', 'QueryFullProcessImageNameW'):
                matches = [e for e in events if e['operation'] == name and e['status'] == 'outcome'
                           and e.get('actualParameters', {}).get('processHandle') == str(pi.process)
                           and e.get('result') not in ('0', 'False', 'None')]
                assert len(matches) == 1, 'missing direct original PI proof: ' + name
                direct[name] = matches[0]
            assert direct['QueryFullProcessImageNameW']['actualParameters']['flags'] == 0, 'image flags differ'
            self.continuous()
            assert not self.ctx.broken and not self.ctx.root_tainted, 'proof journal incomplete'
            fact = dict(handle=str(pi.process), pid=pid, birth=birth, image=image,
                        job=str(self.job), originalPI=dict(self.original), evidence=REFERENCE,
                        directEvents=direct, closedGenerationCount=len(self.ctx.closed_history))
            self.ctx.root_image = fact
            self.ctx.record['rootPreResumeProof'] = fact
            self.ctx.call('root.pre_resume_proof', lambda: fact, targetPID=pid, knownBirth=birth,
                          handle=str(pi.process), handleRole='root')
            self.status['qualification'] = 'PROVED_PENDING_RESUME_AND_CLEANUP'
            self.ctx.save()
            self.continuous()
            assert not self.ctx.broken and not self.cancel(), 'pre-resume save/cancellation failure'
            assert not self.ctx.root_tainted, 'pre-resume proof became tainted'
        except BaseException as exc:
            self.failure(exc)
            self.abort()
            raise
        # Mark before invocation: even an exception/unknown result cannot authorize abort.
        try:
            count = self.api.resume(pi.thread)
            assert self.resume_attempted, 'Resume native invocation not evidenced'
            self.status['previousSuspendCount'] = count
            assert count == 1 and type(count) is int, 'ResumeThread previous count must be exactly one'
            self.resumed = True
            self.ctx.save()
            assert not self.ctx.broken, 'resume journal incomplete'
            self.safe_to_release = True
        except BaseException as exc:
            if not self.resume_attempted:
                self.failure(exc)
                self.abort()
                raise
            self.block('Resume attempted; no abort or second Resume permitted', exc)

    def abort(self):
        try:
            self.continuous()
            assert not self.resume_attempted and not self.terminate_attempted, 'never-resumed abort only once'
            self.terminate_attempted = True
            self.status['terminateAttempted'] = True
            assert self.api.terminate(self.pi.process, 74), 'TerminateProcess failed'
            assert self.api.signaled(self.pi.process, 1000), 'original PI not signaled'
            assert self.api.code(self.pi.process) == 74, 'abort exit differs'
            assert self.api.active(self.job) == 0 and self.api.active(self.job) == 0, 'Job double zero missing'
            self.status['cleanup'] = 'ABORT_SIGNALED_EXIT74_JOB_DOUBLE_ZERO_PENDING_CLOSE'
            self.safe_to_release = True
        except BaseException as exc:
            self.block('never-resumed precise abort did not close', exc)


def runtime(legacy_runtime, context, cancel):
    """Keep original monitor, child classifier and budgets. Replace only root lifecycle."""
    import ctypes as C
    import types
    import kernel_job as kernel
    import suspended_launch
    Base, *rest = legacy_runtime()
    # Bind the unchanged ctypes layouts/imports, not another API implementation.
    for name in ('C', 'H', 'SZ', 'PROCESSINFO', 'STARTUPINFOEX', 'subprocess'):
        setattr(suspended_launch, name, getattr(kernel, name))

    class Domain(Base):
        def __init__(self):
            super().__init__()
            api = self.api
            for attr, name, args, result in (
                ('resume', 'ResumeThread', [kernel.H], kernel.U32),
                ('terminate', 'TerminateProcess', [kernel.H, kernel.U32], C.c_int)):
                fn = getattr(api.k, name); fn.argtypes = args; fn.restype = result
                setattr(api, attr, context.native(name, fn))
            local = types.MethodType(suspended_launch.launch, api)
            api.launch = lambda *a, **kw: context.method('launch', local, *a, **kw)

        def launch(self, argv, *args):
            self.expected = os.path.normcase(str(Path(argv[0]).resolve()))
            life = self.api.suspended = Lifecycle(self.api, context, self.job, self.expected, cancel)
            context.after_create = life.created
            return kernel.KernelDomain.launch(self, argv, *args)

        def prove_root(self):
            fact = context.root_image
            assert fact and self.api.suspended.resumed, 'pre-resume proof missing'
            actual = context.prior_root_image(self.root.h, self.api, self.root.pid)
            assert os.path.normcase(actual) == self.expected, 'root image differs'
            key = (fact['pid'], fact['birth'])
            self.handles[key] = self.root.h
            self.certified.add(key)
            self.events.append(dict(kind=REFERENCE, pid=key[0], birthMs=key[1]))
            assert not self.api.launchCleanupErrors, 'atomic launch cleanup error'

    return (Domain, *rest)
