"""Real native C ABI safety/repair regressions; no provider or mock decoder."""
import ctypes
import hashlib
import json
import pathlib
import struct
import sys
import tempfile
import base64
import os,concurrent.futures
import subprocess

library = ctypes.CDLL(str(pathlib.Path(sys.argv[1]).resolve()))
library.orca_model_create.argtypes = [ctypes.c_char_p]
library.orca_model_create.restype = ctypes.c_void_p
callback_type = ctypes.CFUNCTYPE(None, ctypes.c_char_p, ctypes.c_void_p)
library.orca_model_execute.argtypes = [ctypes.c_void_p, ctypes.c_char_p, callback_type, ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p)]
library.orca_model_cancel.argtypes = [ctypes.c_void_p]
library.orca_model_destroy.argtypes = [ctypes.c_void_p]
library.orca_model_free_string.argtypes = [ctypes.c_void_p]
library.orca_model_capabilities.restype=ctypes.c_char_p
capabilities=json.loads(library.orca_model_capabilities())
assert capabilities['glbSafetyV1']['strictSelfIntersection'] is True
assert capabilities['glbSafetyV1']['maxGeneratedFaceLimit']==1000000
streamed_inspection=capabilities['glbSafetyV1'].get('inspectionWorkspacePolicy')=='streamed-inspect-live-containers-v1'
assert streamed_inspection==('glbRepairV2' in capabilities),capabilities

def encode(document, binary):
    document = json.loads(json.dumps(document))
    document['buffers'] = [{'byteLength': len(binary)}]
    text = json.dumps(document, separators=(',', ':')).encode()
    text += b' ' * (-len(text) % 4)
    binary += b'\0' * (-len(binary) % 4)
    return struct.pack('<IIIII', 0x46546c67, 2, 28+len(text)+len(binary), len(text), 0x4e4f534a)+text+struct.pack('<II', len(binary), 0x004e4942)+binary

def fixture(vertices, faces):
    positions = b''.join(struct.pack('<fff', *v) for v in vertices)
    indexes = b''.join(struct.pack('<III', *f) for f in faces)
    document = {'asset': {'version': '2.0'}, 'bufferViews': [{'buffer': 0, 'byteLength': len(positions)}, {'buffer': 0, 'byteOffset': len(positions), 'byteLength': len(indexes)}], 'accessors': [{'bufferView': 0, 'componentType': 5126, 'count': len(vertices), 'type': 'VEC3'}, {'bufferView': 1, 'componentType': 5125, 'count': len(faces)*3, 'type': 'SCALAR'}], 'meshes': [{'primitives': [{'attributes': {'POSITION': 0}, 'indices': 1, 'mode': 4}]}], 'nodes': [{'mesh': 0}], 'scenes': [{'nodes': [0]}], 'scene': 0}
    return encode(document, positions+indexes)

vertices = [(0,0,0), (1,0,0), (0,1,0), (0,0,1)]
faces = [(0,2,1), (0,1,3), (0,3,2), (1,2,3)]
cases = 0
with tempfile.TemporaryDirectory(prefix='orca-glb-safety-tests-') as directory:
    root = pathlib.Path(directory)
    def run(name, data, repair=False, override=None, cancel_at=None):
        global cases
        source = root/(name+'.glb'); source.write_bytes(data)
        destination = root/name; destination.mkdir(exist_ok=True)
        request = {'schemaVersion': 1, 'operation': 'repairGlbArtifact' if repair else 'inspectGlbArtifact', 'identity': {'accountId': 'account', 'jobId': 'job', 'assetId': 'asset', 'assetVersion': 1, 'sourceSha256': hashlib.sha256(data).hexdigest(), 'operationId': name}, 'sourcePath': str(source), 'outputDirectory': str(destination)}
        if override: override(request)
        before={str(p):p.read_bytes() for p in destination.iterdir()}
        context = library.orca_model_create(str(root).encode())
        assert context
        events = []
        @callback_type
        def event(raw, _):
            value = json.loads(raw)
            if value.get('type') == 'glbSafetyProgress':
                events.append(value)
                if value['phase'] == cancel_at: library.orca_model_cancel(context)
        output = ctypes.c_void_p()
        code = library.orca_model_execute(context, json.dumps(request).encode(), event, None, ctypes.byref(output))
        response = json.loads(ctypes.string_at(output)); library.orca_model_free_string(output); library.orca_model_destroy(context)
        assert source.read_bytes() == data, name
        assert [e['sequence'] for e in events] == list(range(1,len(events)+1))
        assert all(e['identity'] == request['identity'] for e in events)
        if response.get('result', {}).get('candidate') is None: assert {str(p):p.read_bytes() for p in destination.iterdir()}==before, name
        cases += 1
        return code, response

    code, result = run('valid_closed', fixture(vertices, faces))
    assert code == 0 and result['result']['completion'] == 'verified', result
    for name, changed in [('open_boundary', faces[:-1]), ('duplicate', faces+[faces[0]]), ('wrong_orientation', [tuple(reversed(faces[0]))]+faces[1:]), ('degenerate', faces+[(0,0,1)])]:
        code, result = run(name, fixture(vertices, changed))
        assert code == 0 and result['result']['completion'] == 'needs_attention', result
        assert result['result']['checks']['selfIntersection']['performed'] is False
    for name, data in [('bad_index',fixture(vertices,[(0,1,99)])), ('truncated', fixture(vertices,faces)[:-2]), ('nonfinite',fixture([(float('nan'),0,0)]+vertices[1:],faces))]:
        code, result = run(name,data)
        assert code != 0 and not result['ok'], result
    code, result = run('bad_hash', fixture(vertices,faces), override=lambda r:r['identity'].update(sourceSha256='0'*64))
    assert code != 0
    code, result = run('outside_root', fixture(vertices,faces), override=lambda r:r.update(sourcePath=str(root.parent/'outside.glb')))
    assert code != 0
    code, result = run('cancel_before_topology', fixture(vertices,faces), cancel_at='topology')
    for phase in ['residualSoup','residualSurface','residualVertexLinks','residualIntersections','residualComplete']:
        canceled_code,canceled_result=run('cancel_'+phase,fixture(vertices,faces),cancel_at=phase)
        assert canceled_code!=0 and canceled_result['error']['code']=='CANCELED',canceled_result
    assert code == 4 and result['error']['code'] == 'CANCELED', result
    # Two closed tetrahedra overlap without sharing vertices: actual CGAL
    # intersection test must run and reject, rather than treating closed as safe.
    shifted = [(x+.2,y+.2,z+.2) for x,y,z in vertices]
    code,result = run('self_intersecting',fixture(vertices+shifted,faces+[tuple(v+4 for v in f) for f in faces]))
    assert code == 0 and result['result']['checks']['selfIntersection']['performed']
    assert result['result']['checks']['selfIntersection']['status'] == 'failed', result
    # A tiny isolated coincident sheet lies on the tetrahedron's existing face.
    # Dropping only that sheet preserves all original triangles/appearance and
    # passes the strict geometric deviation/area limits, then real CGAL recheck.
    sheet=[(.2,.2,0),(.2001,.2,0),(.2,.2001,0)]
    code,result = run('safe_sheet_repair',fixture(vertices+sheet,faces+[(4,5,6),(6,5,4)]),True)
    answer=result.get('result',{})
    assert code == 0 and answer['completion']=='verified' and answer['repairApplied'], result
    candidate=pathlib.Path(answer['candidate']['path'])
    assert hashlib.sha256(candidate.read_bytes()).hexdigest()==answer['candidate']['sha256']
    code,result = run('repaired_candidate_recheck',candidate.read_bytes())
    assert code == 0 and result['result']['completion']=='verified',result
    code,result=run('closed_verified_copy',fixture(vertices,faces),True)
    answer=result['result']
    assert answer['completion']=='verified' and not answer['repairApplied'] and answer['candidate']
    assert answer['candidate']['sha256']==answer['source']['sha256']
    code,result=run('cancel_publish',fixture(vertices+sheet,faces+[(4,5,6),(6,5,4)]),True,cancel_at='publishing')
    assert code==4 and result['error']['code']=='CANCELED'
    raised=[(x,y,-.0001) for x,y,z in sheet]
    code,result=run('raised_untextured_not_equivalent',fixture(vertices+raised,faces+[(4,5,6),(6,5,4)]),True)
    assert code==0 and result['result']['completion']=='needs_attention' and not result['result']['appearanceVerified']
    # A textured fixture exercises the actual original-BIN/UV/material proofs.
    plain=fixture(vertices+sheet,faces+[(4,5,6),(6,5,4)])
    json_size=struct.unpack_from('<I',plain,12)[0]
    document=json.loads(plain[20:20+json_size]);binary=plain[28+json_size:]
    png=base64.b64decode('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+a6XcAAAAASUVORK5CYII=')
    image_offset=len(binary);binary+=png;binary+=b'\0'*(-len(binary)%4)
    document['bufferViews'].append({'buffer':0,'byteOffset':image_offset,'byteLength':len(png)})
    document['images']=[{'bufferView':2,'mimeType':'image/png'}]
    uv_offset=len(binary);binary+=b''.join(struct.pack('<ff',0.25,0.75) for _ in vertices+sheet)
    document['bufferViews'].append({'buffer':0,'byteOffset':uv_offset,'byteLength':len(binary)-uv_offset})
    document['accessors'].append({'bufferView':3,'componentType':5126,'count':len(vertices+sheet),'type':'VEC2'})
    document['textures']=[{'source':0}]
    document['materials']=[{'pbrMetallicRoughness':{'baseColorTexture':{'index':0}}}]
    primitive=document['meshes'][0]['primitives'][0];primitive['attributes']['TEXCOORD_0']=2;primitive['material']=0
    textured=encode(document,binary)
    code,result=run('textured_deletion_unproven',textured,True)
    assert code==0 and result['result']['completion']=='needs_attention' and result['result']['candidate'] is None and not result['result']['appearanceVerified'],result
    positive=fixture(vertices+sheet,faces+[(4,5,6),(6,5,4)])
    code,result=run('appearance_equivalent_cover',positive,True)
    answer=result['result'];assert code==0 and answer['completion']=='verified' and answer['repairApplied'],result
    derived=pathlib.Path(answer['candidate']['path']).read_bytes();n=struct.unpack_from('<I',derived,12)[0];derived_doc=json.loads(derived[20:20+n]);derived_bin=derived[28+n:]
    positive_n=struct.unpack_from('<I',positive,12)[0];positive_doc=json.loads(positive[20:20+positive_n]);positive_bin=positive[28+positive_n:]
    assert derived_bin[:len(positive_bin)]==positive_bin
    assert derived_doc.get('materials',[])==positive_doc.get('materials',[]) and derived_doc.get('images',[])==positive_doc.get('images',[])
    assert derived_doc['accessors'][:len(positive_doc['accessors'])]==positive_doc['accessors']
    receipt=pathlib.Path(answer['receipt']['path']);receipt_bytes=receipt.read_bytes()
    assert hashlib.sha256(receipt_bytes).hexdigest()==answer['receipt']['sha256']
    assert json.loads(receipt_bytes)['identity']==answer['identity']
    old_candidate=pathlib.Path(answer['candidate']['path']);old_mtime=old_candidate.stat().st_mtime_ns
    code,replay=run('appearance_equivalent_cover',positive,True)
    assert code==0 and replay['result']==answer and old_candidate.stat().st_mtime_ns==old_mtime
    # Simulate termination after candidate publication but before receipt: a
    # fresh context must reprove original/appearance, retain bytes and fill receipt.
    receipt.unlink()
    code,replay=run('appearance_equivalent_cover',positive,True)
    assert code==0 and replay['result']==answer and receipt.read_bytes()==receipt_bytes
    assert old_candidate.stat().st_mtime_ns==old_mtime
    corrupt=bytearray(old_candidate.read_bytes());corrupt[-1]^=1;old_candidate.write_bytes(corrupt)
    code,replay=run('appearance_equivalent_cover',positive,True)
    assert code!=0 and old_candidate.read_bytes()==corrupt
    broken=json.loads(json.dumps(document));broken['images'][0]['uri']='https://outside.invalid/texture.png'
    code,result=run('external_texture',encode(broken,binary));assert code!=0
    broken=json.loads(json.dumps(document));broken['meshes'][0]['primitives'][0]['attributes'].pop('TEXCOORD_0')
    code,result=run('missing_uv',encode(broken,binary));assert code!=0
    # Incompatible bounded inputs are never accepted merely because the
    # position/index arrays would otherwise be readable.
    broken=json.loads(json.dumps(document));broken['extensionsRequired']=['KHR_draco_mesh_compression']
    code,result=run('required_compression',encode(broken,binary));assert code!=0
    def output_trap(kind):
        def prepare(r):
            key=hashlib.sha256(json.dumps(r['identity'],sort_keys=True,separators=(',',':')).encode()).hexdigest()
            suffix={'candidate_alias':'-verified.glb','candidate_symlink':'-verified.glb','candidate_tmp':'-verified.glb.tmp','receipt_tmp':'.glb-safety.json.tmp','receipt_alias':'.glb-safety.json'}[kind]
            target=pathlib.Path(r['outputDirectory'])/(key+suffix)
            if kind=='candidate_symlink':
                try: target.symlink_to(r['sourcePath'])
                except OSError: os.link(r['sourcePath'],target)
            else:os.link(r['sourcePath'],target)
        return prepare
    for kind in ['candidate_alias','candidate_symlink','receipt_alias']:
        code,result=run(kind,fixture(vertices,faces),True,override=output_trap(kind));assert code!=0,result
    for kind in ['candidate_tmp','receipt_tmp']:
        code,result=run(kind,fixture(vertices,faces),True,override=output_trap(kind));assert code==0 and result['result']['completion']=='verified',result
    # Same immutable request, two independent contexts: exclusive publication
    # either creates or reuses identical files without truncation or overwrite.
    data=fixture(vertices,faces);source=root/'concurrent.glb';source.write_bytes(data);destination=root/'concurrent';destination.mkdir()
    request={'schemaVersion':1,'operation':'repairGlbArtifact','identity':{'accountId':'a','jobId':'j','assetId':'x','assetVersion':1,'sourceSha256':hashlib.sha256(data).hexdigest(),'operationId':'concurrent'},'sourcePath':str(source),'outputDirectory':str(destination)}
    def concurrent_call(_):
        context=library.orca_model_create(str(root).encode());out=ctypes.c_void_p()
        @callback_type
        def event(raw,_):pass
        code=library.orca_model_execute(context,json.dumps(request).encode(),event,None,ctypes.byref(out));value=json.loads(ctypes.string_at(out));library.orca_model_free_string(out);library.orca_model_destroy(context);return code,value
    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as executor:values=list(executor.map(concurrent_call,range(2)))
    assert all(code==0 and value['result']['completion']=='verified' for code,value in values),values
    assert values[0][1]==values[1][1] and source.read_bytes()==data
    cases+=2
    if os.name=='nt':
        shallow_root=root
        root=root/'generation-runs'/('b'*64)/'22222222-2222-4222-8222-222222222222'/('deep-private-'+('x'*80))
        root.mkdir(parents=True)
        assert len(str(root))>260
        code,result=run('windows_long_verified',fixture(vertices,faces),True)
        assert code==0 and result['result']['completion']=='verified',result
        assert len(result['result']['candidate']['path'])>260
        assert len(result['result']['receipt']['path'])>260
        for kind in ['candidate_alias','receipt_alias']:
            code,result=run('long_'+kind,fixture(vertices,faces),True,override=output_trap(kind));assert code!=0
        for kind in ['candidate_tmp','receipt_tmp']:
            code,result=run('long_'+kind,fixture(vertices,faces),True,override=output_trap(kind));assert code==0,result
        code,result=run('long_outside',fixture(vertices,faces),override=lambda r:r.update(sourcePath=str(shallow_root/'valid_closed.glb')))
        assert code!=0
        code,result=run('long_cancel_publish',fixture(vertices,faces),True,cancel_at='publishing')
        assert code==4 and result['error']['code']=='CANCELED'
        source=root/'long-concurrent.glb';source.write_bytes(data);destination=root/'concurrent';destination.mkdir()
        request.update(sourcePath=str(source),outputDirectory=str(destination));request['identity']['operationId']='long-concurrent'
        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as executor:values=list(executor.map(concurrent_call,range(2)))
        assert all(code==0 and value['result']['completion']=='verified' for code,value in values),values
        assert values[0][1]==values[1][1] and source.read_bytes()==data
        assert not list(destination.glob('*.tmp-*'))
        cases+=2
        # The independent runner does not inherit Python's Windows manifest.
        # Exercise the same 64/36-character scope as the mobile host transport,
        # including candidate/receipt reads and a second cold process replay.
        real_root=shallow_root/'generation-runs'/('b'*64)/'22222222-2222-4222-8222-222222222222'
        real_root.mkdir(parents=True,exist_ok=True)
        real_source=real_root/'original.glb';real_source.write_bytes(data)
        real_output=real_root/'verified';real_output.mkdir(exist_ok=True)
        request['sourcePath']=str(real_source);request['outputDirectory']=str(real_output)
        request_file=shallow_root/'long-runner-request.json';request_file.write_text(json.dumps({'operations':[request]}))
        runner=pathlib.Path(sys.argv[1]).with_name('orca_model_runner.exe')
        responses=[]
        for i in range(2):
            result_file=real_root/('result-'+str(i)+'.json')
            process=subprocess.run([str(runner),'--workspace',str(real_root),'--request',str(request_file),'--output',str(result_file)],capture_output=True,text=True)
            assert process.returncode==0,(process.stdout,process.stderr,result_file.read_text() if result_file.exists() else None)
            responses.append(json.loads(result_file.read_text()))
        assert responses[0]==responses[1]
        result=responses[0]['results'][0]['result']
        assert result['completion']=='verified' and len(result['candidate']['path'])>260
        assert hashlib.sha256(pathlib.Path(result['receipt']['path']).read_bytes()).hexdigest()==result['receipt']['sha256']
        assert real_source.read_bytes()==data
        cases+=2
        root=shallow_root
        # The DLL inherits its caller's manifest. Python's long-path-aware
        # process cannot establish that the packaged standalone runner works.
        # Run each root/source/output shape in fresh native processes, including
        # cold idempotence and aliases, with short control-file names.
        runner=pathlib.Path(sys.argv[1]).resolve().parent/'orca_model_runner.exe'
        def deep(path):
            while len(str(path)) < 280: path=path/('private-'+('z'*60))
            path.mkdir(parents=True,exist_ok=True)
            return path
        for shape in ['source','output','root']:
            workspace=shallow_root/('runner-'+shape)
            workspace.mkdir()
            if shape=='root': workspace=deep(workspace)
            source_folder=deep(workspace/'input') if shape=='source' else workspace
            source=source_folder/'s.glb';data=fixture(vertices,faces);source.write_bytes(data)
            output_folder=deep(workspace/'out') if shape!='root' else workspace/'v'
            output_folder.mkdir(exist_ok=True)
            for trap in ['none','candidate_alias','receipt_alias','candidate_tmp','receipt_tmp','outside','namespace_source','device_source']:
                destination=output_folder/trap;destination.mkdir()
                request={'schemaVersion':1,'operation':'repairGlbArtifact','identity':{'accountId':'a','jobId':'j','assetId':'x','assetVersion':1,'sourceSha256':hashlib.sha256(data).hexdigest(),'operationId':'runner-'+shape+'-'+trap},'sourcePath':str(source),'outputDirectory':str(destination)}
                if trap=='outside': request['sourcePath']=str(shallow_root/'valid_closed.glb')
                elif trap=='namespace_source': request['sourcePath']='\\\\?\\'+str(source)
                elif trap=='device_source': request['sourcePath']='\\\\.\\'+str(source)
                elif trap!='none': output_trap(trap)(request)
                request_path=shallow_root/('runner-'+shape+'-'+trap+'.json')
                request_path.write_text(json.dumps({'operations':[request]}))
                answers=[]
                for attempt in range(2 if trap=='none' else 1):
                    result_path=workspace/'r.json'
                    control=request_path
                    if attempt==1:
                        control=output_folder/'request.json';control.write_bytes(request_path.read_bytes())
                        assert len(str(control))>260
                    process=subprocess.run([str(runner),'--workspace',str(workspace),'--request',str(control),'--output',str(result_path)],capture_output=True,text=True)
                    assert process.returncode in [0,1],(shape,trap,process.returncode,process.stderr)
                    response=json.loads(result_path.read_text())
                    if trap in ['candidate_alias','receipt_alias','outside','namespace_source','device_source']:
                        assert process.returncode==1 and not response['ok'],(shape,trap,response)
                    else:
                        assert process.returncode==0 and response['ok'],(shape,trap,response)
                        value=response['results'][0]['result'];assert value['completion']=='verified'
                        for item in [value['source'],value['candidate'],value['receipt']]:
                            contents=pathlib.Path(item['path']).read_bytes()
                            assert len(contents)==item['bytes'] and hashlib.sha256(contents).hexdigest()==item['sha256']
                        assert len(value['candidate']['path'])>260 and len(value['receipt']['path'])>260
                        answers.append(response)
                    assert source.read_bytes()==data
                    cases+=1
                if trap=='none': assert answers[0]==answers[1]
                assert not list(destination.glob('*.tmp-*'))
    # Repair retains its conservative removal/publication admission. Inspect
    # now streams the same 2M-face container, but must report its real duplicate
    # topology defect rather than claiming safe or skipping the checks.
    large=fixture(vertices,faces);n=struct.unpack_from('<I',large,12)[0]
    doc=json.loads(large[20:20+n]);payload=large[28+n:][:48]+struct.pack('<III',0,2,1)*2000000
    doc['accessors'][1]['count']=6000000;doc['bufferViews'][1]['byteLength']=24000000
    code,result=run('workspace_budget',encode(doc,payload),repair=True);assert code!=0 and result['error']['code']=='RESOURCE_LIMIT',result
    code,result=run('streamed_duplicate_2m',encode(doc,payload))
    if streamed_inspection:
        assert code==0,result
        proof=result['result'];assert proof['completion']=='needs_attention' and not proof['geometryVerified'],proof
        assert proof['checks']['topology']['status']=='failed' and proof['checks']['selfIntersection']['performed'] is False,proof
        assert proof['workspaceAdmission']['admissionBytes']<=805306368,proof
    else:
        # CGAL-only builds keep the original conservative workspace admission.
        assert code!=0 and result['error']['code']=='RESOURCE_LIMIT',result
print(json.dumps({'status':'passed','nativeCases':cases}))
