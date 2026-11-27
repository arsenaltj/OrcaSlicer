"""Actual C ABI direct color transactions, persistence and manufacturing sidecar."""
import ctypes as C, json, pathlib, sys, tempfile
lib=C.CDLL(str(pathlib.Path(sys.argv[1]).resolve()))
CB=C.CFUNCTYPE(None,C.c_char_p,C.c_void_p)
lib.orca_model_create.argtypes=[C.c_char_p];lib.orca_model_create.restype=C.c_void_p
lib.orca_model_execute.argtypes=[C.c_void_p,C.c_char_p,CB,C.c_void_p,C.POINTER(C.c_void_p)]
lib.orca_model_destroy.argtypes=[C.c_void_p];lib.orca_model_free_string.argtypes=[C.c_void_p]
@CB
def event(raw,user): pass
with tempfile.TemporaryDirectory(prefix='color-sdk-') as folder:
    h=lib.orca_model_create(folder.encode());assert h
    def run(op, error=None, **kwargs):
        raw=C.c_void_p();rc=lib.orca_model_execute(h,json.dumps(dict(schemaVersion=1,operation=op,**kwargs)).encode(),event,None,C.byref(raw))
        try:answer=json.loads(C.string_at(raw))
        finally:lib.orca_model_free_string(raw)
        if error:
            assert rc!=0 and answer['error']['code']==error, answer
            return
        assert rc==0 and answer['ok'],answer
        return answer['result']
    created=run('create',vertices=[[0,0,0],[1,0,0],[0,1,0],[0,0,1]],triangles=[[0,2,1],[0,1,3],[0,3,2],[1,2,3]],faceColors=[[1,0,0,1],[0,0,1,1],[1,0,0,1],[0,0,1,1]])
    ident=created['documentId']
    assert len(run('getPrintPalette',expectedRevision=1,documentId=ident,requestedColorCount=2)['colors'])==2
    args=dict(documentId=ident,expectedRevision=1,requestedColorCount=2,materialFingerprint='a'*64,processFingerprint='b'*64,palette=[dict(slot=0,materialId=0,color=[1,0,0],materialType='PLA'),dict(slot=1,materialId=1,color=[0,0,1],materialType='PLA')])
    match=run('matchPrintColors',**args)
    assert match['plan']['quality']['metric']=='CIEDE2000' and match['plan']['quality']['max']<0.001
    assert match['scene']['sceneUsage']=='preview-only'
    run('acceptCandidate',expectedRevision=1,candidateId=match['candidateId'],error='COLOR_MAPPING_CONFIRMATION_REQUIRED')
    apply=dict(documentId=ident,expectedRevision=1,candidateId=match['candidateId'],confirmMaterialMapping=True,materialFingerprint='a'*64,processFingerprint='b'*64)
    run('applyPrintColors',**{**apply,'materialFingerprint':'c'*64},error='COLOR_PLAN_CHANGED')
    committed=run('applyPrintColors',**apply);assert committed['revision']==2
    saved=run('save',expectedRevision=2);assert pathlib.Path(saved['documentPath']).is_file()
    document=json.loads(pathlib.Path(saved['documentPath']).read_text());assert document['materialIds']==[0,1,0,1]
    run('undo',expectedRevision=2);run('redo',expectedRevision=3)
    exported=run('exportManufacturing',expectedRevision=4)
    assert exported['multiMaterial'] and not exported['materialRoutingVerified']
    side=json.loads(pathlib.Path(exported['artifacts'][1]['path']).read_text());assert side['materialIds']==[0,1,0,1]
    stale=run('matchPrintColors',**{**args,'expectedRevision':4})
    run('paint',expectedRevision=4,faces=[0],color=[0,1,0,1])
    run('applyPrintColors',**{**apply,'expectedRevision':5,'candidateId':stale['candidateId']},error='STALE_CANDIDATE')
    run('matchPrintColors',**{**args,'expectedRevision':5,'requestedColorCount':1,'protectedFaces':[0,1]},error='COLOR_BUDGET_CONFLICT')
    locked=run('matchPrintColors',**{**args,'expectedRevision':5,'materialLocks':[dict(faces=[0],materialId=1)]})
    assert locked['plan']['quality']['metric']=='CIEDE2000'
    run('applyPrintColors',**{**apply,'expectedRevision':5,'candidateId':locked['candidateId']},error='COLOR_ERROR_CONFIRMATION_REQUIRED')
    run('applyPrintColors',**{**apply,'expectedRevision':5,'candidateId':locked['candidateId'],'acceptColorError':True})
    saved=run('save',expectedRevision=6)
    run('load',documentId=ident)
    restored=run('exportManufacturing',expectedRevision=6)
    side=json.loads(pathlib.Path(restored['artifacts'][1]['path']).read_text());assert side['materialIds'][0]==1
    assert side['appearanceMetadata']['printColorPlan']['scope']=='shared-desktop-direct-print-color-v1'
    trial=run('matchPrintColors',**{**args,'expectedRevision':6,'trialOnly':True})
    run('applyPrintColors',**{**apply,'expectedRevision':6,'candidateId':trial['candidateId']},error='COLOR_TRIAL_ONLY')
    run('savePrintColorDraft',documentId=ident,expectedRevision=6,candidateId=trial['candidateId'])
    run('exportManufacturing',expectedRevision=7,error='COLOR_TRIAL_ONLY')
    run('manufacturingSnapshot',expectedRevision=7,error='COLOR_TRIAL_ONLY')
    lib.orca_model_destroy(h)
print(json.dumps(dict(actualCabi=True,transactions=True,saveLoad=True,undoRedo=True,staleRejected=True,manufacturingSidecar=True,routingVerified=False)))
