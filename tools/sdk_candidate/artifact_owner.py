"""Artifact-only retained Job owner. Import never opens Windows APIs.

Uses the API-v8 KernelDomain unchanged; never enables Native build mode.
"""
from pathlib import Path
import os
import time

PROTOCOL = 'sdk-artifact-kernel-v1'
SUPPORT = {
    'kernel_job.py': '492b7ac00096aaa0c05164010b9cc19e7a6a3273689d2ae61917b387ef93950e',
    'build_resources.py': 'db0b53eb89facac3f6551dfb0b2a8e141f13089ed00b2c040a76aac8ad637575',
    'qa_owner_safety.py': '7d79b97dd704949aa4b9869072b7408c9f9b3d9ac0c44536cdf3aa93cfe0fca6',
}
TRACE_CONTEXT = None


def admit_cancel(path):
    assert not Path(path).exists(), 'preexisting cancellation'


def runtime():
    from kernel_job import KernelDomain, Win64API, H, U32
    from build_resources import sample, heavy, identity, now
    from qa_owner_safety import attempt, remember_error, wait_naturally
    import ctypes as C

    class ImageAPI(Win64API):
        def __init__(self):
            super().__init__()
            from api_context import instrument
            assert TRACE_CONTEXT is not None, 'missing artifact API context'
            instrument(self, TRACE_CONTEXT)
        def image(self, handle):
            fn = self.k.QueryFullProcessImageNameW
            fn.argtypes = [H, U32, C.c_wchar_p, C.POINTER(U32)]
            fn.restype = C.c_int
            size = U32(32768)
            buf = C.create_unicode_buffer(size.value)
            value = TRACE_CONTEXT.call('QueryFullProcessImageNameW',lambda:fn(handle, 0, buf, C.byref(size)))
            self.ok(value)
            return buf.value

    class ArtifactDomain(KernelDomain):
        def __init__(self):
            super().__init__(ImageAPI())
        def launch(self, argv, *args):
            self.expected = os.path.normcase(str(Path(argv[0]).resolve()))
            return super().launch(argv, *args)
        def prove_root(self):
            super().prove_root()
            assert os.path.normcase(self.api.image(self.root.h)) == self.expected, 'retained root image differs'
        def allowed(self, row, owned, images):
            if identity(row) not in owned:
                return False
            # Every observed allowed process must have an actual retained handle
            # with Job membership and matching kernel image, not just CIM name.
            matches = [k for k in self.handles if k[0] == row['ProcessId']]
            if len(matches) != 1:
                return False
            handle = self.handles[matches[0]]
            image = os.path.normcase(self.api.image(handle))
            # Only one interpreter root is authorized. An arbitrary nested
            # ctypes SDK script is not exempt just because its image is Python.
            interpreter_child = (matches[0][0] != self.root.pid
                                 and Path(image).name.lower() in ('python.exe','pythonw.exe','node.exe'))
            return (not interpreter_child and self.api.member(handle, self.job) and image in images
                    and Path(image).name.lower() == row['Name'].lower())
    return ArtifactDomain, sample, heavy, identity, now, attempt, remember_error, wait_naturally


def run_owned(argv, log_path, record, save, cancel_file, checkpoint, timeout=7200,
              mode='artifact', *, permitted_commands, permitted_images):
    global TRACE_CONTEXT
    assert mode == 'artifact', 'artifact protocol only'
    assert argv in permitted_commands, 'command outside exact artifact contract'
    admit_cancel(cancel_file)
    images = {os.path.normcase(str(Path(x).resolve())) for x in permitted_images}
    assert os.path.normcase(str(Path(argv[0]).resolve())) in images, 'unbound root image'
    Domain, sample, heavy, identity, now, attempt, remember, wait = runtime()
    errors = record.setdefault('monitorErrors', [])
    import ctypes
    from api_context import Context
    TRACE_CONTEXT = Context(record, save, errors, argv, lambda:getattr(ctypes,'get_last_error',lambda:None)(),
                            lambda value:getattr(ctypes,'set_last_error',lambda v:None)(value) if value is not None else None)
    domain = child = None
    code = None
    failure = None
    started = time.monotonic()
    record.update(protocol=PROTOCOL, argv=argv, rawSamples=[], samples=[], state='started')
    def persist():
        attempt(errors, 'record_save', save)
    def monitor():
        nonlocal failure
        try:
            raw = sample()
            record['rawSamples'].append(raw)
            persist()  # preserve raw nullable metadata before any classification
            TRACE_CONTEXT.rows(raw['processes'],len(record['rawSamples'])-1)
            TRACE_CONTEXT.phase = 'observe'
            names = {Path(x).name.lower() for x in images}
            owned = domain.observe(raw['processes'], lambda p: heavy(p) or p['Name'].lower() in names)
            bad = [p for p in raw['processes'] if
                   (heavy(p) or identity(p) in owned) and not domain.allowed(p, owned, images)]
            row = dict(freeKiB=raw['freeKiB'], unexpected=bad,
                       cancel=Path(cancel_file).exists(), timeout=time.monotonic()-started >= timeout)
            record['samples'].append(row)
            if row['freeKiB'] <= 3*1024**2 or bad or row['cancel'] or row['timeout']:
                failure = failure or 'artifact_resource_or_cancel'
        except BaseException as exc:
            remember(errors, 'monitor', exc)
            failure = failure or 'monitor_error'
        ready = False
        try:
            TRACE_CONTEXT.phase = 'drain'
            ready = domain.settled()
        except BaseException as exc:
            remember(errors, 'kernel_drain', exc)
            failure = failure or 'kernel_drain_error'
        persist()
        return ready
    with Path(log_path).open('xb') as log:
        try:
            admit_cancel(cancel_file)
            domain = Domain()
            TRACE_CONTEXT.phase = 'launch'
            child = domain.launch(argv, os.getcwd(), dict(os.environ), log, log)
            TRACE_CONTEXT.call('domain.launch_receipt',lambda:dict(pid=child.pid,handle=str(child.h) if hasattr(child,'h') else None),
                               targetPID=child.pid,handle=str(child.h) if hasattr(child,'h') else None,handleRole='root')
            domain.prove_root()
            record['pid'] = child.pid
            while child.poll() is None:
                monitor()
                time.sleep(1)
        except BaseException as exc:
            remember(errors, 'launch_or_owner', exc)
            failure = failure or 'launch_or_owner'
        finally:
            if domain is not None:
                if child is None:
                    while True:
                        try:
                            TRACE_CONTEXT.phase = 'recover'
                            child = TRACE_CONTEXT.call('domain.recover_root',domain.recover_root)
                            if child is not None:
                                TRACE_CONTEXT.call('domain.recovered_receipt',lambda:dict(pid=child.pid),targetPID=child.pid,
                                    handle=str(child.h) if hasattr(child,'h') else None,handleRole='root')
                            break
                        except BaseException as exc:
                            remember(errors, 'recover_root', exc)
                if child is not None:
                    code = wait(child, monitor, errors, pause=1)
                    while not monitor():
                        attempt(errors, 'drain_pause', lambda: time.sleep(1))
                    record['kernelEvidence'] = domain.snapshot()
                    record['ownClosure'] = 'retained_handles_signaled_and_job_double_zero'
                    persist()
                    TRACE_CONTEXT.phase = 'close'
                    attempt(errors, 'close_owned_handles', domain.close)
                else:
                    attempt(errors, 'unused_job_close', lambda: domain.api.close(domain.job))
    record.update(exitCode=code, cancelReason=failure, state='failed' if failure or errors or code != 0 else 'returned')
    persist()
    if errors:
        record.update(state='failed', cancelReason=failure or 'monitor_error')
        persist()
    TRACE_CONTEXT = None
    assert code == 0 and failure is None and not errors, 'artifact first failure; no next command'


def commands(plan):
    """Exact commands adapter/static-proof can run. No user-supplied extra argv."""
    from adapter import argv_for, STAGES
    stage = Path(plan['staging'])
    repo = Path(plan['sdkRepository'])
    result = [argv_for(plan), [plan['node'], str(repo/'mobile-slicing/build_android_arm64.cjs'), '--verify-only'],
              [plan['python'], plan['postprocessProof']['path'], '--binding', plan['bindingPath']]]
    for kind in ('model', 'slicing'):
        build=stage/(kind+'-windows')
        sdk=build/'Release'/('orca_model_sdk_c.dll' if kind=='model' else 'orca_mobile_slicing_sdk_c.dll')
        result.extend([[plan['readobj'], '--coff-exports', str(build/'raw-tbb/tbb12.dll')],
                       [plan['readobj'], '--coff-exports', str(sdk)], [plan['readobj'], '--coff-imports', str(sdk)]])
    return result
