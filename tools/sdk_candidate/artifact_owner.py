"""Artifact-only retained Job owner. Import never opens Windows APIs.

Uses the API-v8 KernelDomain unchanged; never enables Native build mode.
"""
from pathlib import Path
import os
import time
import hashlib
import json

PROTOCOL = 'sdk-artifact-kernel-v1'
SUPPORT = {
    'kernel_job.py': '492b7ac00096aaa0c05164010b9cc19e7a6a3273689d2ae61917b387ef93950e',
    'build_resources.py': 'db0b53eb89facac3f6551dfb0b2a8e141f13089ed00b2c040a76aac8ad637575',
    'qa_owner_safety.py': '7d79b97dd704949aa4b9869072b7408c9f9b3d9ac0c44536cdf3aa93cfe0fca6',
}
TRACE_CONTEXT = None


def memory_policy():
    return dict(schema='sdk-resource-policy-v1',profile='SDK_MEMORY_ONLY_V1',entrySamples=2,
                entryMinFreeKiB=8912896,runtimeMinExclusiveFreeKiB=3145728,
                foreignProcesses='OBSERVE_ONLY',ownership='STRICT_RETAINED_PID_BIRTH_JOB')


def memory_only(policy=None):
    if policy is None:return False # unchanged legacy strict contract
    assert type(policy) is dict and json.dumps(policy,sort_keys=True,allow_nan=False)==json.dumps(memory_policy(),sort_keys=True), 'unknown/ill-typed resource policy'
    return True


def resource_admission(rows,save,policy,strict_gate,sample,heavy):
    if not memory_only(policy):return strict_gate(rows,save)
    for _ in range(2):
        raw=sample();rows.append(raw)
        # Preserve complete nullable process rows. Classification is diagnostic only.
        observed=[];unknown=[]
        for index,row in enumerate(raw['processes']):
            try:
                if heavy(row):observed.append(row)
            except (KeyError,TypeError,AttributeError):unknown.append(index)
        raw['heavy']=observed;raw['unclassifiedProcessIndices']=unknown;raw['resourcePolicy']=memory_policy()
        save()
        assert type(raw['freeKiB']) is int and raw['freeKiB']>=8912896, 'memory-only entry requires8.5GiB'
        time.sleep(1)


def resource_available(raw,policy,heavy):
    if memory_only(policy):return type(raw['freeKiB']) is int and raw['freeKiB']>3145728
    return raw['freeKiB']>3*1024**2 and not any(heavy(row) for row in raw['processes'])


def observe_owned(domain,rows):
    """No foreign enumeration/open by name. Only certified generations/lineage."""
    from kernel_job import key, rank
    selected=[];owned=set()
    for _ in range(len(rows)+1):
        certified=set(domain.certified);pids={k[0] for k in certified};selected=[];current=[];facts={}
        def retained(generation):
            if generation not in facts:
                assert generation in domain.handles, 'unretained own generation conflict'
                facts[generation]=domain.api.info(domain.handles[generation])
                assert facts[generation][:2]==generation, 'retained own identity conflict'
            return facts[generation]
        # Resolve the current parent generations before looking at any child.
        # In particular, foreign children must not inherit a historical PID alone.
        for row in rows:
            if type(row) is not dict:continue
            pid=row.get('ProcessId')
            if type(pid) is int and pid in pids:
                generation=key(row) # unknown own generation remains a strict error
                if generation not in certified:
                    for old in (k for k in certified if k[0]==pid):
                        actual=retained(old)
                        assert actual[2] is not None and generation[1]>actual[2], 'live own PID/birth conflict'
                    continue # proven later foreign PID reuse; never open/wait it
                current.append(row)
        current_pids={row['ProcessId'] for row in current}
        for row in rows:
            if type(row) is not dict:continue
            pid=row.get('ProcessId');parent=row.get('ParentProcessId')
            if type(pid) is int and pid in pids:
                if any(row is item for item in current):selected.append(row)
                continue
            if type(parent) is not int or parent not in pids:continue
            parents=[k for k in certified if k[0]==parent]
            live_parent=parent in current_pids or any(k in domain.handles and retained(k)[2] is None for k in parents)
            if not live_parent:
                # An exited historical parent cannot authorize strict child parsing.
                # Only a valid birth inside its retained lifetime can restore an edge.
                try:birth=rank(row) if type(row.get('CreationDate')) is str and row['CreationDate'] else None
                except (ValueError,TypeError,OverflowError,OSError):birth=None
                if birth is None:continue
                birth=round(birth*1000)
                if not any(k in domain.handles and k[1]<=birth<=retained(k)[2] for k in parents):continue
            assert type(pid) is int and pid>0, 'unknown own descendant PID'
            # The core still validates the child's strict key and bounded ancestry.
            # Foreign current generations are deliberately absent from this projection.
            if domain.parent(row,current):selected.append(row)
        owned=domain.observe(selected,lambda row:False)
        assert all(key(row) in domain.certified for row in selected), 'own descendant outside certified Job'
        if domain.certified==certified:break
    return selected,owned


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
            TRACE_CONTEXT.after_create=lambda receipt:TRACE_CONTEXT.capture_root(receipt,self.image)
        def image(self, handle):
            fn = self.k.QueryFullProcessImageNameW
            fn.argtypes = [H, U32, C.c_wchar_p, C.POINTER(U32)]
            fn.restype = C.c_int
            size = U32(32768)
            buf = C.create_unicode_buffer(size.value)
            value = TRACE_CONTEXT.native('QueryFullProcessImageNameW',fn)(handle, 0, buf, C.byref(size))
            self.ok(value)
            return buf.value

    class ArtifactDomain(KernelDomain):
        def __init__(self):
            super().__init__(ImageAPI())
            self.api.artifact_job=self.job
        def launch(self, argv, *args):
            self.expected = os.path.normcase(str(Path(argv[0]).resolve()))
            return super().launch(argv, *args)
        def prove_root(self):
            super().prove_root()
            actual=TRACE_CONTEXT.prior_root_image(self.root.h,self.api,self.root.pid)
            assert os.path.normcase(actual) == self.expected, 'retained root image differs'
        def allowed(self, row, owned, images):
            if identity(row) not in owned:
                return False
            # Every observed allowed process must have an actual retained handle
            # with Job membership and matching kernel image, not just CIM name.
            matches = [k for k in self.handles if k[0] == row['ProcessId']]
            if len(matches) != 1:
                return False
            handle = self.handles[matches[0]]
            actual=(TRACE_CONTEXT.prior_root_image(handle,self.api,self.root.pid)
                    if matches[0][0]==self.root.pid else self.api.image(handle))
            image = os.path.normcase(actual)
            # Only one interpreter root is authorized. An arbitrary nested
            # ctypes SDK script is not exempt just because its image is Python.
            interpreter_child = (matches[0][0] != self.root.pid
                                 and Path(image).name.lower() in ('python.exe','pythonw.exe','node.exe'))
            return (not interpreter_child and self.api.member(handle, self.job) and image in images
                    and Path(image).name.lower() == row['Name'].lower())
    return ArtifactDomain, sample, heavy, identity, now, attempt, remember_error, wait_naturally


def run_owned(argv, log_path, record, save, cancel_file, checkpoint, timeout=7200,
              mode='artifact', *, permitted_commands, permitted_images, env=None, cwd=None, lifecycle_profile=None,
              package_plan=None, android_plan=None, resource_policy=None):
    global TRACE_CONTEXT
    assert mode == 'artifact', 'artifact protocol only'
    memory=memory_only(resource_policy)
    assert argv in permitted_commands, 'command outside exact artifact contract'
    admit_cancel(cancel_file)
    launch_env=dict(os.environ) if env is None else dict(env)
    launch_cwd=os.getcwd() if cwd is None else str(cwd)
    assert all(isinstance(k,str) and isinstance(v,str) for k,v in launch_env.items()), 'explicit string environment required'
    images = {os.path.normcase(str(Path(x).resolve())) for x in permitted_images}
    assert os.path.normcase(str(Path(argv[0]).resolve())) in images, 'unbound root image'
    import ctypes
    from api_context import Context
    if lifecycle_profile is not None:
        from suspended_root import PROFILE, SuspendedContext
        assert lifecycle_profile == PROFILE, 'unknown lifecycle profile'
        assert package_plan is None or android_plan is None, 'conflicting artifact scopes'
        if android_plan is not None:
            from package_android import owner_contract, commands as android_commands
            life = owner_contract(android_plan)
            assert life.get('resourcePolicy')==resource_policy, 'Android owner resource policy differs'
            assert permitted_commands == android_commands(android_plan), 'Android command table differs'
            assert launch_cwd == life['cwd'] and type(timeout) is int and timeout == life['commandTimeoutSeconds'], 'Android budget/cwd differs'
            assert permitted_images == [android_plan['strip'], android_plan['readelf']], 'Android images differ'
        elif package_plan is None:
            from owner_preflight import commands as fixed_commands, REPO
            assert permitted_commands == fixed_commands() and argv in fixed_commands(), 'fixed suspended SDK fixtures only'
            assert timeout == 60 and launch_cwd == str(REPO), 'fixed suspended budget/cwd only'
        else:
            import adapter
            from contracts import normal_lifecycle
            life = normal_lifecycle(package_plan, adapter)
            assert permitted_commands == commands(package_plan), 'normal package command table differs'
            assert launch_cwd == life['cwd'] and timeout == life['commandTimeoutSeconds'], 'normal package budget/cwd differs'
            assert permitted_images == [package_plan[k] for k in ('python','node','readobj','readelf','strip','git')], 'normal package images differ'
        Context = SuspendedContext
    else:
        assert package_plan is None and android_plan is None, 'normal package requires explicit suspended profile'
    Domain, sample, heavy, identity, now, attempt, remember, wait = runtime()
    errors = record.setdefault('monitorErrors', [])
    TRACE_CONTEXT = Context(record, save, errors, argv, lambda:getattr(ctypes,'get_last_error',lambda:None)(),
                            lambda value:getattr(ctypes,'set_last_error',lambda v:None)(value) if value is not None else None,
                            journal_path=Path(log_path).with_suffix('.api.jsonl'))
    if lifecycle_profile is not None:
        from suspended_root import runtime as suspended_runtime
        Domain, sample, heavy, identity, now, attempt, remember, wait = suspended_runtime(
            runtime, TRACE_CONTEXT, lambda:Path(cancel_file).exists())
    domain = child = None
    code = None
    failure = None
    started = time.monotonic()
    record.update(protocol=PROTOCOL, argv=argv, rawSamples=[], samples=[], state='started')
    record.update(launchEnvironmentSha256=hashlib.sha256(json.dumps(launch_env,sort_keys=True).encode()).hexdigest(),launchCwd=launch_cwd)
    if memory:record['resourcePolicy']=memory_policy()
    def persist():
        attempt(errors, 'record_save', save)
    def monitor():
        nonlocal failure
        try:
            raw = sample()
            record['rawSamples'].append(raw)
            persist()  # preserve raw nullable metadata before any classification
            TRACE_CONTEXT.phase = 'observe'
            names = {Path(x).name.lower() for x in images}
            if memory:
                TRACE_CONTEXT.rows([],len(record['rawSamples'])-1)
                selected,owned=observe_owned(domain,raw['processes'])
                record.setdefault('ownProjections',[]).append(dict(rawSampleIndex=len(record['rawSamples'])-1,
                    rawProcessIndices=[i for i,row in enumerate(raw['processes']) if any(row is item for item in selected)]))
                TRACE_CONTEXT.rows(selected,len(record['rawSamples'])-1)
                bad=[p for p in selected if identity(p) in owned and not domain.allowed(p,owned,images)]
                live=sum(not domain.api.signaled(h) for h in set(domain.handles.values()))
                active=domain.api.active(domain.job)
                assert type(active) is int and active>=0 and active<=live, 'unaccounted own Job process'
            else:
                TRACE_CONTEXT.rows(raw['processes'],len(record['rawSamples'])-1)
                owned = domain.observe(raw['processes'], lambda p: heavy(p) or p['Name'].lower() in names)
                bad = [p for p in raw['processes'] if
                       (heavy(p) or identity(p) in owned) and not domain.allowed(p, owned, images)]
            row = dict(freeKiB=raw['freeKiB'], unexpected=bad,
                       cancel=Path(cancel_file).exists(), timeout=time.monotonic()-started >= timeout)
            record['samples'].append(row)
            low_memory=(type(row['freeKiB']) is not int or row['freeKiB']<=3145728) if memory else row['freeKiB']<=3*1024**2
            if low_memory or bad or row['cancel'] or row['timeout']:
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
            child = domain.launch(argv, launch_cwd, launch_env, log, log)
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
            persist()  # first full error snapshot; never bypass natural finally
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
    attempt(errors,'journal_finalize',TRACE_CONTEXT.finish)
    persist()
    if errors:
        record.update(state='failed', cancelReason=failure or 'monitor_error')
        persist()
    TRACE_CONTEXT = None
    if lifecycle_profile is not None:
        record['suspendedRoot']['cleanup'] = record.get('ownClosure', 'INCOMPLETE')
        record['suspendedRoot']['qualification'] = ('QUALIFIED' if code == 0 and failure is None and not errors
                                                   else 'FAILED')
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
