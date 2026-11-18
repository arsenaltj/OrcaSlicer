"""Actual Model C ABI v2 publication and carrier regressions; no provider/mock."""
import ctypes as C,json,hashlib,struct,tempfile,pathlib,sys,base64,os,copy
lib=C.CDLL(str(pathlib.Path(sys.argv[1]).resolve()))
CB=C.CFUNCTYPE(None,C.c_char_p,C.c_void_p)
lib.orca_model_create.argtypes=[C.c_char_p];lib.orca_model_create.restype=C.c_void_p
lib.orca_model_execute.argtypes=[C.c_void_p,C.c_char_p,CB,C.c_void_p,C.POINTER(C.c_void_p)]
lib.orca_model_cancel.argtypes=[C.c_void_p];lib.orca_model_destroy.argtypes=[C.c_void_p];lib.orca_model_free_string.argtypes=[C.c_void_p]
lib.orca_model_capabilities.restype=C.c_char_p
cap=json.loads(lib.orca_model_capabilities());assert cap['glbRepairV2']['supportedCompletions']==['native_compatible']
assert cap['glbRepairV2']['preservesOriginal'] and cap['glbRepairV2']['maxGeneratedFaceLimit']==2000000

def encode(doc,binary):
 doc=copy.deepcopy(doc);doc['buffers']=[{'byteLength':len(binary)}]
 j=json.dumps(doc,separators=(',',':')).encode();j+=b' '*(-len(j)%4);binary+=b'\0'*(-len(binary)%4)
 return struct.pack('<IIIII',0x46546c67,2,28+len(j)+len(binary),len(j),0x4e4f534a)+j+struct.pack('<II',len(binary),0x004e4942)+binary

def decode(data):
 n=struct.unpack_from('<I',data,12)[0];return json.loads(data[20:20+n]),data[28+n:]

def fixture(textured=False):
 v=[(-0.,0.,0.),(1.,0.,0.),(0.,1.,0.),(0.,0.,1.)];f=[(0,2,1),(0,1,3),(0,3,2),(1,2,3)]
 b=b''.join(struct.pack('<fff',*x) for x in v)+b''.join(struct.pack('<III',*x) for x in f)
 doc={'asset':{'version':'2.0'},'bufferViews':[{'buffer':0,'byteLength':48},{'buffer':0,'byteOffset':48,'byteLength':48}],
 'accessors':[{'bufferView':0,'componentType':5126,'count':4,'type':'VEC3','min':[min(p[k] for p in v) for k in range(3)],'max':[max(p[k] for p in v) for k in range(3)]},{'bufferView':1,'componentType':5125,'count':12,'type':'SCALAR'}],
 'meshes':[{'primitives':[{'attributes':{'POSITION':0},'indices':1,'mode':4}]}],'nodes':[{'mesh':0}],'scenes':[{'nodes':[0]}],'scene':0}
 if textured:
  uv=struct.pack('<8f',0,0,1,0,0,1,1,1);png=base64.b64decode('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+jit8AAAAASUVORK5CYII=')
  doc['bufferViews'] += [{'buffer':0,'byteOffset':len(b),'byteLength':len(uv)},{'buffer':0,'byteOffset':len(b)+len(uv),'byteLength':len(png)}]
  doc['accessors'].append({'bufferView':2,'componentType':5126,'count':4,'type':'VEC2'})
  doc['meshes'][0]['primitives'][0]['attributes']['TEXCOORD_0']=2;doc['meshes'][0]['primitives'][0]['material']=0
  doc['images']=[{'bufferView':3,'mimeType':'image/png'}];doc['textures']=[{'source':0,'sampler':0}];doc['samplers']=[{'wrapS':10497,'wrapT':33071}]
  doc['materials']=[{'pbrMetallicRoughness':{'baseColorTexture':{'index':0},'metallicRoughnessTexture':{'index':0},'metallicFactor':.7,'roughnessFactor':.3},'normalTexture':{'index':0,'scale':.8},'alphaMode':'BLEND','doubleSided':True}]
  b+=uv+png
 return doc,b

cases=0
with tempfile.TemporaryDirectory(prefix='orca-v2-cabi-') as directory:
 root=pathlib.Path(directory)
 def run(name,doc=None,binary=None,edit=None,cancel=None,setup=None,expect=None,replay=False):
  global cases
  if doc is None:doc,binary=fixture()
  data=encode(doc,binary);source=root/(name+'.glb');source.write_bytes(data);dest=root/name;dest.mkdir()
  identity={'accountId':'private-tests','jobId':'job','assetId':'asset','assetVersion':1,'sourceSha256':hashlib.sha256(data).hexdigest(),'operationId':name}
  request={'schemaVersion':1,'operation':'repairGlbArtifact','policyVersion':'glb-repair-v2','identity':identity,'sourcePath':str(source),'outputDirectory':str(dest)}
  if edit:edit(request)
  if setup:setup(dest,source,identity)
  before={str(p):p.read_bytes() for p in dest.rglob('*') if p.is_file()};events=[];handle=lib.orca_model_create(str(root).encode());assert handle
  @CB
  def event(raw,_):
   value=json.loads(raw)
   if value.get('type')=='glbSafetyProgress':
    events.append(value);assert value['identity']==identity
    if value['phase']==cancel:lib.orca_model_cancel(handle)
  def execute():
   output=C.c_void_p();code=lib.orca_model_execute(handle,json.dumps(request).encode(),event,None,C.byref(output));response=json.loads(C.string_at(output));lib.orca_model_free_string(output);return code,response
  try:
   code,response=execute();assert source.read_bytes()==data
   if expect:
    assert code!=0 and response['error']['code']==expect,(name,response)
    assert before=={str(p):p.read_bytes() for p in dest.rglob('*') if p.is_file()},name
   else:
    assert code==0,(name,response);result=response['result'];assert result['completion']=='native_compatible'
    assert result['nativeProof']['outputGeometryId']==result['nativeProof']['readbackGeometryId']
    assert result['checks']['geometry']=={'performed':False,'status':'not_run','count':None,'details':'Full bidirectional geometry deviation and area policy not evaluated by enhanced-only entry'}
    assert result['manufacturingSafe'] is False
    candidate=pathlib.Path(result['candidate']['path']);receipt=pathlib.Path(result['receipt']['path'])
    assert hashlib.sha256(candidate.read_bytes()).hexdigest()==result['candidate']['sha256']
    assert hashlib.sha256(receipt.read_bytes()).hexdigest()==result['receipt']['sha256']
    rd,rb=decode(candidate.read_bytes());assert rd['nodes'][0]['matrix']==[.001,0,0,0,0,0,-.001,0,0,.001,0,0,0,0,0,1]
    position=rd['accessors'][rd['meshes'][0]['primitives'][0]['attributes']['POSITION']]
    view=rd['bufferViews'][position['bufferView']];offset=view.get('byteOffset',0)+position.get('byteOffset',0)
    stride=view.get('byteStride',12)
    values=[struct.unpack_from('<fff',rb,offset+i*stride) for i in range(position['count'])]
    assert position.get('min')==[min(p[k] for p in values) for k in range(3)],('missing or wrong serialized POSITION min',position)
    assert position.get('max')==[max(p[k] for p in values) for k in range(3)],('missing or wrong serialized POSITION max',position)
    for field in ['materials','textures','samplers']:assert rd.get(field)==doc.get(field)
    if result['nativeProof']['appearanceCarrierLayout']=='original-bin-prefix':
     assert rd.get('images')==doc.get('images') and rb[:len(binary)]==binary
    else:
     assert result['nativeProof']['appearanceCarrierLayout']=='active-encoded-images'
     assert len(rd.get('images',[]))==len(doc.get('images',[]))
     for source_image,target_image in zip(doc.get('images',[]),rd.get('images',[])):
      before=doc['bufferViews'][source_image['bufferView']];after=rd['bufferViews'][target_image['bufferView']]
      assert binary[before.get('byteOffset',0):before.get('byteOffset',0)+before['byteLength']]==rb[after.get('byteOffset',0):after.get('byteOffset',0)+after['byteLength']]
      restored=copy.deepcopy(target_image);restored['bufferView']=source_image['bufferView'];assert restored==source_image
     assert len(candidate.read_bytes())<64*1024*1024
     assert len(rb)<len(binary)
    if replay:
     saved=receipt.read_bytes();events.clear();code2,again=execute();assert code2==0 and receipt.read_bytes()==saved,(name,again)
   assert [e['sequence'] for e in events]==list(range(1,len(events)+1))
  finally:lib.orca_model_destroy(handle)
  cases+=1;print('PASS',name,flush=True)
 run('unit_negative_zero',replay=True)
 d,b=fixture();points=[(-.25,-.5,-1.25),(1.5,-.5,-1.25),(-.25,2.75,-1.25),(-.25,-.5,3.5)]
 b=b''.join(struct.pack('<fff',*v) for v in points)+b[48:]
 d['accessors'][0]['min']=[min(v[k] for v in points) for k in range(3)]
 d['accessors'][0]['max']=[max(v[k] for v in points) for k in range(3)]
 run('negative_nonuniform_position_bounds',d,b)
 d,b=fixture(True);run('pbr_alpha_wrap_normal_channels',d,b)
 d,b=fixture(True);run('compact_inactive_bin_keep_all_pbr_images',d,b+b'\0'*(70*1024*1024-len(b)))
 for phase in ['reading','repairing','verifying','appearanceMapping','publishing','candidatePublished','receiptPublished','residualSoup','residualSurface','residualVertexLinks','residualIntersections']:
  run('cancel_'+phase,cancel=phase,expect='CANCELED')
 run('wrong_source_hash',edit=lambda r:r['identity'].update(sourceSha256='0'*64),expect='INVALID_GLB')
 run('unknown_policy',edit=lambda r:r.update(policyVersion='unknown'),expect='INVALID_GLB')
 d,b=fixture();bad=bytearray(b);struct.pack_into('<f',bad,0,float('nan'));run('nonfinite',d,bytes(bad),expect='INVALID_GLB')
 d,b=fixture();positions=b[:48]+b'\0'*(2500000*12-48);faces=b[48:]*500000
 d['bufferViews'][0]['byteLength']=len(positions);d['bufferViews'][1]['byteOffset']=len(positions);d['bufferViews'][1]['byteLength']=len(faces)
 d['accessors'][0]['count']=2500000;d['accessors'][1]['count']=6000000
 run('resource_preflight_real_dense_layout',d,positions+faces,expect='RESOURCE_LIMIT')
 del positions,faces
 d,b=fixture();faces=b[48:]*500000+b[48:60]
 d['accessors'][1]['count']=6000003;d['bufferViews'][1]['byteLength']=len(faces)
 run('face_limit_plus_one_real_layout',d,b[:48]+faces,expect='INVALID_GLB');del faces
 d,b=fixture();positions=b[:48]+b'\0'*((2500001)*12-48)
 d['accessors'][0]['count']=2500001;d['bufferViews'][0]['byteLength']=len(positions);d['bufferViews'][1]['byteOffset']=len(positions)
 run('vertex_limit_plus_one_real_layout',d,positions+b[48:],expect='INVALID_GLB');del positions
 d,b=fixture();run('input_file_limit_real_bytes',d,b+b'\0'*(100*1024*1024-len(b)),expect='FILE_LIMIT')
 d,b=fixture(True);d['images'][0]['uri']='https://invalid.example/x.png';run('external_image',d,b,expect='INVALID_GLB')
 d,b=fixture();d['meshes'][0]['primitives']*=2;run('multi_primitive_explicit_rejection',d,b,expect='INVALID_GLB')
 def alias(dest,source,identity):
  target=dest/'verified-enhanced';target.mkdir();stem=hashlib.sha256((json.dumps(identity,sort_keys=True,separators=(',',':'))+'glb-repair-v2').encode()).hexdigest();os.link(source,target/(stem+'.glb'))
 run('source_hardlink_alias',setup=alias,expect='OUTPUT_CONFLICT')
 def bad_receipt(dest,source,identity):
  target=dest/'verified-enhanced';target.mkdir();stem=hashlib.sha256((json.dumps(identity,sort_keys=True,separators=(',',':'))+'glb-repair-v2').encode()).hexdigest();(target/(stem+'.json')).write_text('{}')
 run('receipt_conflict_cleanup',setup=bad_receipt,expect='OUTPUT_CONFLICT')
print(json.dumps({'cases':cases,'status':'passed','actualNativeCABI':True}))
