"""Public ABI regression cases for independent review A-R01, A-R02 and A-R03."""
import ctypes
import hashlib
import json
import pathlib
import sys
import tempfile

dll_path=pathlib.Path(sys.argv[1]).resolve()
dll=ctypes.CDLL(str(dll_path))
dll.orca_model_create.argtypes=[ctypes.c_char_p]
dll.orca_model_create.restype=ctypes.c_void_p
dll.orca_model_execute.argtypes=[ctypes.c_void_p,ctypes.c_char_p,ctypes.c_void_p,ctypes.c_void_p,ctypes.POINTER(ctypes.c_void_p)]
dll.orca_model_free_string.argtypes=[ctypes.c_void_p]
dll.orca_model_destroy.argtypes=[ctypes.c_void_p]
evidence={'library':str(dll_path),'sha256':hashlib.sha256(dll_path.read_bytes()).hexdigest(),'cases':[]}

class Session:
    def __init__(self,root):
        self.handle=dll.orca_model_create(str(root).encode())
        assert self.handle
    def close(self):
        dll.orca_model_destroy(self.handle)
    def call(self,name,inline=True,**fields):
        request=dict(schemaVersion=1,operation=name,returnInlineScene=inline,**fields)
        raw=ctypes.c_void_p()
        code=dll.orca_model_execute(self.handle,json.dumps(request).encode(),None,None,ctypes.byref(raw))
        assert raw.value,'Missing ABI result'
        try: response=json.loads(ctypes.string_at(raw))
        finally: dll.orca_model_free_string(raw)
        assert (code==0)==response['ok'],response
        return response
    def create(self,size=1):
        return self.call('create',vertices=[[0,0,0],[size,0,0],[0,size,0]],triangles=[[0,1,2]])['result']
    def candidate(self,revision):
        return self.call('prepareCandidate',expectedRevision=revision,command={'operation':'transform','translation':[10,0,0]})['result']['candidateId']

with tempfile.TemporaryDirectory(prefix='orca-model-state-regression-') as temporary:
    root=pathlib.Path(temporary)
    s=Session(root)
    a=s.create()
    old=s.candidate(1)
    b=s.create(2)
    denied=s.call('acceptCandidate',expectedRevision=1,candidateId=old)
    assert not denied['ok'] and denied['error']['code']=='STALE_CANDIDATE'
    assert s.call('getScene')['result']['documentId']==b['documentId']
    same=s.candidate(1)
    s.create(2)
    assert not s.call('acceptCandidate',expectedRevision=1,candidateId=same)['ok']
    assert s.call('save',expectedRevision=1)['ok']
    candidate=s.candidate(1)
    assert s.call('load',documentId=b['documentId'])['ok']
    assert not s.call('acceptCandidate',expectedRevision=1,candidateId=candidate)['ok']
    evidence['cases'].append({'id':'A-R01','passed':True,'checks':['different document','same document reopened','load generation']})

    original=root/'original.obj'
    original.write_text('v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n')
    original_sha=hashlib.sha256(original.read_bytes()).hexdigest()
    candidate=s.candidate(1)
    assert s.call('import',inputPath=str(original),units='mm')['ok']
    assert not s.call('acceptCandidate',expectedRevision=1,candidateId=candidate)['ok']
    texture=root/'texture.png';texture.write_bytes(b'preserved-source-dependency')
    appearance=root/'appearance.json'
    appearance.write_text(json.dumps({'schemaVersion':1,'units':'mm','upAxis':'Z','geometrySha256':original_sha,
        'faceColors':[[1,0,0,1]],'materialIds':[0],
        'parts':[{'entityId':'p','firstVertex':0,'vertexCount':3,'firstTriangle':0,'triangleCount':1,'texture':{'path':'texture.png'}}]}))
    candidate=s.candidate(1)
    opened=s.call('importConverted',inputPath=str(original),appearancePath=str(appearance),units='mm')['result']
    assert not s.call('acceptCandidate',expectedRevision=1,candidateId=candidate)['ok']
    assert s.call('transform',expectedRevision=1,translation=[1,0,0])['ok']
    assert s.call('save',expectedRevision=2)['ok']
    s.close()
    s=Session(root)
    assert s.call('load',documentId=opened['documentId'])['ok']
    alias=root/'original-hardlink.obj';alias.hardlink_to(original)
    protected=[original,alias,appearance,texture,root/'documents'/f"{opened['documentId']}.json"]
    before={str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in protected}
    for path in protected:
        failed=s.call('exportManufacturing',expectedRevision=2,outputPath=str(path))
        assert not failed['ok'] and failed['error']['code']=='SOURCE_OVERWRITE',failed
    assert before=={str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in protected}
    assert s.call('save',expectedRevision=2)['ok'],'Authorized save remains usable'
    evidence['cases'].append({'id':'A-R02','passed':True,'checks':['source','hardlink alias','appearance','texture dependency','saved document','persistent load protection'],'hashes':before})

    candidate=s.candidate(2)
    current=s.call('getScene')['result']
    count=65537
    colors=[[(i%256)/255,((i//256)%256)/255,((i//65536)%256)/255,1] for i in range(count)]
    failed=s.call('create',inline=False,vertices=[[0,0,0],[2,0,0],[0,2,0]],triangles=[[0,1,2]]*count,faceColors=colors)
    assert not failed['ok'] and failed['error']['code']=='PREVIEW_PALETTE_LIMIT',failed
    retained=s.call('getScene')['result']
    assert retained['documentId']==current['documentId'] and retained['revision']==current['revision']
    assert s.call('acceptCandidate',expectedRevision=2,candidateId=candidate)['ok']
    assert s.call('undo',expectedRevision=3)['ok']
    s.close()
    blocked=root/'blocked';blocked.mkdir()
    (blocked/'scenes').write_bytes(b'block-scene-directory')
    s=Session(blocked)
    old=s.create()
    failed=s.call('transform',inline=False,expectedRevision=1,translation=[10,0,0])
    assert not failed['ok']
    retained=s.call('getScene')['result']
    assert retained['revision']==1 and retained['vertices']==old['vertices']
    evidence['cases'].append({'id':'A-R03','passed':True,'checks':['failed create retains document','failed create retains candidate','undo preserved','failed edit retains geometry/revision']})
    s.close()

print(json.dumps(evidence,indent=2))
if len(sys.argv)>2:
    pathlib.Path(sys.argv[2]).write_text(json.dumps(evidence,indent=2),encoding='utf-8')
