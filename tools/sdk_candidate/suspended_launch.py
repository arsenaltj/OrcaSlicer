"""Local copy of pinned kernel launch: only flags, proof and thread custody differ.
Never selected by legacy. Globals come from the verified inert kernel module.
"""
def launch(self,job,argv,cwd,env,out,err):
    import msvcrt
    duplicates=[]; attrs=None; initialized=False
    # Domain publishes this receipt and its RootProcess BEFORE entering CreateProcess.
    pi=getattr(self,'launchReceipt',None)
    if pi is None: pi=PROCESSINFO(); self.launchReceipt=pi
    try:
        for f in [out,err]:
            h=H(); self.ok(self.duplicate(self.current(),msvcrt.get_osfhandle(f.fileno()),self.current(),C.byref(h),0,True,2)); duplicates.append(h.value)
        stdin=self.stdInput(0xFFFFFFF6)
        if stdin not in [None,0,(1<<64)-1]:
            h=H(); self.ok(self.duplicate(self.current(),stdin,self.current(),C.byref(h),0,True,2)); duplicates.append(h.value)
        handles=(H*len(duplicates))(*duplicates); jobs=(H*1)(job); size=SZ()
        self.init(None,2,0,C.byref(size))
        assert size.value>0 and C.get_last_error()==122, 'attribute sizing failed'
        attrs=C.create_string_buffer(size.value)
        self.ok(self.init(attrs,2,0,C.byref(size))); initialized=True
        self.ok(self.update(attrs,0,0x2000D,jobs,C.sizeof(jobs),None,None)) # JOB_LIST, before execution
        self.ok(self.update(attrs,0,0x20002,handles,C.sizeof(handles),None,None)) # only stdout/stderr inherit
        si=STARTUPINFOEX(); si.si.cb=C.sizeof(si); si.si.flags=0x100
        si.si.stdin=duplicates[2] if len(duplicates)==3 else None
        si.si.stdout=duplicates[0]; si.si.stderr=duplicates[1]; si.attributes=C.cast(attrs,H)
        cmd=C.create_unicode_buffer(subprocess.list2cmdline(argv))
        block=C.create_unicode_buffer('\0'.join(k+'='+v for k,v in sorted(env.items(),key=lambda kv:kv[0].upper()))+'\0\0')
        # Explicit SDK_SUSPENDED_ROOT_V1 adds CREATE_SUSPENDED.
        self.ok(self.create(argv[0],cmd,None,None,True,0x80404,block,str(cwd),C.byref(si),C.byref(pi)))
        return pi.process,int(pi.pid)
    except BaseException as exc:
        self.suspended.failure(exc)
        # Native success with a complete original PI is the only abort authority.
        # Unknown Create/partial PI cannot be resumed, aborted or discarded.
        if (pi.process or pi.thread) and not self.suspended.safe_to_release:
            self.suspended.block('launch outcome/cleanup unknown', exc)
        raise
    finally:
        # The shared native output is authoritative even if interruption occurs
        # after the kernel filled it but before Python receives a success value.
        def clean(label,action):
            try: action()
            except BaseException as e:
                self.launchCleanupErrors.append(dict(stage=label,error=repr(e),processHandlePresent=bool(pi.process)))
                if label == 'thread_close':
                    self.suspended.block('original thread CloseHandle failed or unknown', e)
        if pi.thread and self.suspended.safe_to_release and not self.suspended.blocked:
            clean('thread_close',lambda:self.close(pi.thread))
        if initialized: clean('attribute_delete',lambda:self.delete(attrs))
        for h in duplicates: clean('duplicate_close',lambda h=h:self.close(h))
