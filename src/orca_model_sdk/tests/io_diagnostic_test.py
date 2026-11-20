"""Test-only compiled faults execute the real CABI/helpers, never production controls."""
import ctypes as C,errno,json,pathlib,sys,tempfile,hashlib
scope={};exec((pathlib.Path(__file__).with_name('glb_scene_profile_test.py')).read_text().split('cases=0')[0],scope)
lib=scope['lib'];CB=scope['CB'];lib.orca_model_test_io_fault.argtypes=[C.c_int,C.c_int]
lib.orca_model_test_errno_kind.argtypes=[C.c_int];lib.orca_model_test_errno_kind.restype=C.c_char_p
production=C.CDLL(str(pathlib.Path(sys.argv[2]).resolve()))
assert sys.argv[3] in ('ON','OFF'), 'CMake must declare the expected RepairCore profile'
repair_core=sys.argv[3]=='ON'
for candidate in (lib,production):
    candidate.orca_model_capabilities.restype=C.c_char_p
    assert ('glbRepairV2' in json.loads(candidate.orca_model_capabilities()))==repair_core
for name in ['orca_model_test_io_fault','orca_model_test_errno_kind']:
    try:getattr(production,name);raise AssertionError('Production exported test control')
    except AttributeError:pass
mapping={0:'unknown',errno.EACCES:'access_denied',errno.EPERM:'access_denied',errno.ENOSPC:'no_space',errno.EROFS:'read_only',errno.ENOENT:'not_found',errno.EMFILE:'too_many_files',errno.ENFILE:'too_many_files',errno.ENOSYS:'unsupported',errno.EXDEV:'cross_device',errno.EIO:'io_failure',errno.EINTR:'interrupted',123456:'unknown'}
# Python Windows exposes Winsock EDQUOT, not a CRT errno constant. It is
# deliberately unknown to the errno classifier; Win32 quota uses its own code.
if hasattr(errno,'EDQUOT'):mapping[errno.EDQUOT]='unknown' if sys.platform=='win32' else 'quota_exceeded'
for code,kind in mapping.items():assert lib.orca_model_test_errno_kind(code).decode()==kind,(code,kind)
with tempfile.TemporaryDirectory(prefix='safe-io-cabi-')as directory:
    root=pathlib.Path(directory);doc,binary=scope['fixture']();data=scope['encode'](doc,binary);source=root/'SECRET-source.glb';source.write_bytes(data);count=0
    def run(index,code,kind,boundary,unsupported=False):
        global count
        dest=root/str(count);dest.mkdir();count+=1
        identity={'accountId':'private','jobId':'job','assetId':'asset','assetVersion':1,'sourceSha256':hashlib.sha256(data).hexdigest(),'operationId':str(count)}
        request={'schemaVersion':1,'operation':'repairGlbArtifact','policyVersion':'glb-repair-v2','identity':identity,'sourcePath':str(source),'outputDirectory':str(dest)}
        context=lib.orca_model_create(str(root).encode());assert context
        lib.orca_model_test_io_fault(index,code)
        @CB
        def event(*args):pass
        out=C.c_void_p()
        try:rc=lib.orca_model_execute(context,json.dumps(request).encode(),event,None,C.byref(out));answer=json.loads(C.string_at(out));lib.orca_model_free_string(out)
        finally:lib.orca_model_destroy(context)
        assert source.read_bytes()==data
        if unsupported:
            assert rc!=0 and answer['error']['code']=='UNSUPPORTED',answer
            assert 'ioDiagnostic' not in answer['error'],answer
            assert not list(dest.iterdir()), list(dest.iterdir())
            return
        assert rc!=0 and answer['error']['code']=='IO_ERROR',answer
        diagnostic=answer['error']['ioDiagnostic'];assert diagnostic=={'schemaVersion':1,'boundary':boundary,'kind':kind},answer
        assert set(answer['error'])=={'code','message','ioDiagnostic'}
        assert 'SECRET'not in json.dumps(answer) and str(root)not in json.dumps(answer)
        assert not list(dest.rglob('*.glb'))and not list(dest.rglob('*.tmp-*'))
    run(0,errno.EACCES,'access_denied','file_open')
    run(1,0,'unknown','file_read')
    run(1,errno.EIO,'io_failure','file_read')
    if not repair_core:
        run(-1,0,None,None,unsupported=True)
    else:
        run(3,-2,'unknown','temp_write') # successful short write must ignore stale OS code
    # Actual Windows operations are faulted with immediate captured Win32 codes.
    for boundary,index in ([('temp_create',2),('temp_write',3),('temp_flush',4),('output_publish',5)] if repair_core else []):
        for code,kind in [(5,'access_denied'),(112,'no_space'),(1295,'quota_exceeded'),(19,'read_only'),(2,'not_found'),(4,'too_many_files'),(1117,'io_failure'),(1816,'unknown'),(50,'unsupported'),(17,'cross_device'),(995,'interrupted'),(0,'unknown')]:run(index,code,kind,boundary)
    assert count==(52 if repair_core else 4), count
print(json.dumps({'actualProductionHelperCabiCases':count,'repairCoreExpected':repair_core,'errnoClassificationCases':len(mapping),'productionControlSymbolsAbsent':True,'windowsFaultInjection':True,'posixSyscallsExecuted':False}))
