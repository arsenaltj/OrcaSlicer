"""Actual C ABI static-scene/specular fixtures, not a renderer/repair mock."""
import base64, copy, ctypes as C, hashlib, json, math, pathlib, struct, sys, tempfile
lib=C.CDLL(str(pathlib.Path(sys.argv[1]).resolve()))
CB=C.CFUNCTYPE(None,C.c_char_p,C.c_void_p)
lib.orca_model_create.argtypes=[C.c_char_p];lib.orca_model_create.restype=C.c_void_p
lib.orca_model_execute.argtypes=[C.c_void_p,C.c_char_p,CB,C.c_void_p,C.POINTER(C.c_void_p)]
lib.orca_model_destroy.argtypes=[C.c_void_p];lib.orca_model_free_string.argtypes=[C.c_void_p]
P=[(0.,0.,0.),(1.,0.,0.),(0.,1.,0.),(0.,0.,1.)]
F=[(0,2,1),(0,1,3),(0,3,2),(1,2,3)]
UV=[(0.,0.),(1.,0.),(0.,1.),(1.,1.)];N=[1/math.sqrt(3)]*3
PNG=base64.b64decode('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+jit8AAAAASUVORK5CYII=')
def fixture():
    arrays=[b''.join(struct.pack('<3f',*p) for p in P),b''.join(struct.pack('<3I',*f) for f in F),b''.join(struct.pack('<2f',*p) for p in UV),struct.pack('<12f',*(N*4)),PNG]
    binary=b'';views=[]
    for a in arrays:
        binary+=b'\0'*(-len(binary)%4);views.append({'buffer':0,'byteOffset':len(binary),'byteLength':len(a)});binary+=a
    return {'asset':{'version':'2.0'},'buffers':[{'byteLength':len(binary)}],'bufferViews':views,
      'accessors':[{'bufferView':i,'componentType':5125 if i==1 else 5126,'count':12 if i==1 else 4,'type':'SCALAR' if i==1 else 'VEC2' if i==2 else 'VEC3'} for i in range(4)],
      'meshes':[{'primitives':[{'attributes':{'POSITION':0,'TEXCOORD_0':2,'NORMAL':3},'indices':1,'material':0}]}],
      'nodes':[{'mesh':0}],'scenes':[{'nodes':[0]}],'scene':0,
      'images':[{'bufferView':4,'mimeType':'image/png'}],'textures':[{'source':0}],
      'materials':[{'pbrMetallicRoughness':{'baseColorTexture':{'index':0},'metallicFactor':0},'doubleSided':True,'extensions':{'KHR_materials_specular':{'specularFactor':0}}}],
      'extensionsUsed':['KHR_materials_specular'],'extensionsRequired':['KHR_materials_specular']},binary
def encode(doc,binary):
    j=json.dumps(doc,separators=(',',':')).encode();j+=b' '*(-len(j)%4);b=binary+b'\0'*(-len(binary)%4)
    return struct.pack('<5I',0x46546c67,2,28+len(j)+len(b),len(j),0x4e4f534a)+j+struct.pack('<2I',len(b),0x004e4942)+b
def decode(data):
    n=struct.unpack_from('<I',data,12)[0];return json.loads(data[20:20+n]),data[28+n:]
def values(doc,binary,index):
    a=doc['accessors'][index];v=doc['bufferViews'][a['bufferView']];n={'VEC3':3,'VEC2':2,'SCALAR':1}[a['type']];fmt=('f' if a['componentType']==5126 else 'I')*n
    return [struct.unpack_from('<'+fmt,binary,v.get('byteOffset',0)+a.get('byteOffset',0)+i*v.get('byteStride',n*4)) for i in range(a['count'])]
cases=0
with tempfile.TemporaryDirectory(prefix='orca-scene-profile-') as directory:
    root=pathlib.Path(directory)
    def run(name,edit=None,expect=None,world=lambda p:p,normal=N,reflected=False):
        global cases
        doc,binary=fixture()
        if edit:edit(doc)
        data=encode(doc,binary);source=root/(name+'.glb');source.write_bytes(data);dest=root/name;dest.mkdir()
        identity={'accountId':'private-tests','jobId':'job','assetId':'asset','assetVersion':1,'sourceSha256':hashlib.sha256(data).hexdigest(),'operationId':name}
        request={'schemaVersion':1,'operation':'repairGlbArtifact','policyVersion':'glb-repair-v2','identity':identity,'sourcePath':str(source),'outputDirectory':str(dest)}
        handle=lib.orca_model_create(str(root).encode());assert handle;events=[]
        @CB
        def event(raw,_):events.append(json.loads(raw))
        try:
            out=C.c_void_p();code=lib.orca_model_execute(handle,json.dumps(request).encode(),event,None,C.byref(out));answer=json.loads(C.string_at(out));lib.orca_model_free_string(out)
        finally:lib.orca_model_destroy(handle)
        assert source.read_bytes()==data
        if expect:
            assert code!=0 and answer['error']['code']==expect,(name,answer)
            assert not [p for p in dest.rglob('*') if p.is_file()],name
        else:
            assert code==0,(name,answer)
            result=answer['result'];proof=result['nativeProof'];assert result['completion']=='native_compatible' and not result['manufacturingSafe']
            assert proof['outputGeometryId']==proof['readbackGeometryId']
            candidate=pathlib.Path(result['candidate']['path']).read_bytes();receipt=pathlib.Path(result['receipt']['path']).read_bytes()
            assert hashlib.sha256(candidate).hexdigest()==result['candidate']['sha256']
            assert hashlib.sha256(receipt).hexdigest()==result['receipt']['sha256']
            rd,rb=decode(candidate)
            for field in ['materials','extensionsUsed','extensionsRequired','textures']:assert rd.get(field)==doc.get(field),(name,field)
            view=rd['bufferViews'][rd['images'][0]['bufferView']];assert rb[view.get('byteOffset',0):view.get('byteOffset',0)+view['byteLength']]==PNG
            assert rd['nodes']==[{'mesh':0,'matrix':[.001,0,0,0,0,0,-.001,0,0,.001,0,0,0,0,0,1]}]
            attrs=rd['meshes'][0]['primitives'][0]['attributes'];rp=values(rd,rb,attrs['POSITION']);uv=values(rd,rb,attrs['TEXCOORD_0']);rn=values(rd,rb,attrs['NORMAL']);wanted=[world(p) for p in P]
            for p,u,n in zip(rp,uv,rn):
                p=(p[0]/1000,p[2]/1000,-p[1]/1000);n=(n[0],n[2],-n[1])
                distances=[sum((p[k]-q[k])**2 for k in range(3)) for q in wanted];i=min(range(4),key=distances.__getitem__)
                assert distances[i]<1e-10,(name,p,wanted)
                assert max(abs(u[k]-UV[i][k]) for k in range(2))<1e-6,(name,'corner UV',u,i)
                assert abs(abs(sum(n[k]*normal[k] for k in range(3)))-1)<1e-5,(name,'normal',n,normal)
            actual=json.loads(receipt);assert actual['appearanceEvidence']==result['appearanceEvidence']
            assert actual['appearanceEvidence']['sourceFaces']==4
            assert actual['appearanceEvidence']['sourceReflectionWindingCorrected']==reflected
            assert actual['appearanceEvidence']['materialDescriptorSha256']==hashlib.sha256(json.dumps(doc['materials'],sort_keys=True,separators=(',',':')).encode()).hexdigest()
            sequence=[e['sequence'] for e in events if e.get('type')=='glbSafetyProgress'];assert sequence==list(range(1,len(sequence)+1))
        cases+=1;print('PASS',name,flush=True)
    run('factor_zero_identity')
    run('factor_one',lambda d:d['materials'][0]['extensions']['KHR_materials_specular'].update(specularFactor=1))
    q=math.sqrt(.5)
    run('rotation90',lambda d:d['nodes'][0].update(rotation=[q,0,0,q]),world=lambda p:(p[0],-p[2],p[1]),normal=(N[0],-N[1],N[2]))
    norm=math.sqrt(1/4+1/9+1/16)
    run('translated_nonuniform_scale',lambda d:d['nodes'][0].update(translation=[3,4,5],scale=[2,3,4]),world=lambda p:(3+2*p[0],4+3*p[1],5+4*p[2]),normal=(.5/norm,1/3/norm,.25/norm))
    run('reflection',lambda d:d['nodes'][0].update(scale=[-1,1,1]),world=lambda p:(-p[0],p[1],p[2]),normal=(-N[0],N[1],N[2]),reflected=True)
    run('hierarchy',lambda d:d.update(nodes=[{'translation':[3,4,5],'children':[1]},{'mesh':0,'rotation':[q,0,0,q]}]),world=lambda p:(3+p[0],4-p[2],5+p[1]),normal=(N[0],-N[1],N[2]))
    run('affine_matrix',lambda d:d['nodes'][0].update(matrix=[2,0,0,0,0,3,0,0,0,0,4,0,3,4,5,1]),world=lambda p:(3+2*p[0],4+3*p[1],5+4*p[2]),normal=(.5/norm,1/3/norm,.25/norm))
    for name,edit in [
        ('unknown_extension',lambda d:d.update(extensionsUsed=['KHR_unknown'])),
        ('missing_used',lambda d:d.pop('extensionsUsed')),
        ('specular_texture',lambda d:d['materials'][0]['extensions']['KHR_materials_specular'].update(specularTexture={'index':0})),
        ('specular_bool',lambda d:d['materials'][0]['extensions']['KHR_materials_specular'].update(specularFactor=True)),
        ('specular_range',lambda d:d['materials'][0]['extensions']['KHR_materials_specular'].update(specularFactor=1.1)),
        ('unknown_material_extension',lambda d:d['materials'][0]['extensions'].update(KHR_unknown={})),
        ('matrix_trs',lambda d:d['nodes'][0].update(matrix=[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1],translation=[1,0,0])),
        ('singular',lambda d:d['nodes'][0].update(scale=[1,0,1])),
        ('quaternion_norm',lambda d:d['nodes'][0].update(rotation=[1,0,0,1])),
        ('nonfinite_transform',lambda d:d['nodes'][0].update(translation=[1e300,0,0])),
        ('non_affine',lambda d:d['nodes'][0].update(matrix=[1,0,0,1,0,1,0,0,0,0,1,0,0,0,0,1])),
        ('cycle',lambda d:d['nodes'][0].update(children=[0])),
        ('shared_parent',lambda d:d.update(nodes=[{'children':[2]},{'children':[2]},{'mesh':0}],scenes=[{'nodes':[0,1]}])),
        ('orphan',lambda d:d['nodes'].append({})),
        ('second_mesh_instance',lambda d:d.update(nodes=[{'mesh':0,'children':[1]},{'mesh':0}])),
        ('active_scene',lambda d:d.update(scene=1))
    ]:run(name,edit,expect='INVALID_GLB')
print(json.dumps({'actualCabiCases':cases,'hostOnly':True,'rendererPixelsTested':False}))
