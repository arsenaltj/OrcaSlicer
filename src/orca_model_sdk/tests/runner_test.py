"""Real subprocess consumer tests for state, appearance, revision and export identity."""
import hashlib
import json
import pathlib
import subprocess
import sys
import tempfile

runner = pathlib.Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix='orca-model-sdk-') as directory:
    root = pathlib.Path(directory)
    def run(operations, success=True):
        request = root/'request.json'
        request.write_text(json.dumps({'operations': operations}), encoding='utf-8')
        result = subprocess.run([str(runner), '--workspace', str(root), '--request', str(request), '--output', str(root/'result.json')], capture_output=True, text=True)
        assert (result.returncode == 0) == success, result.stderr
        return json.loads((root/'result.json').read_text())
    def op(name, **kw):
        if name in ('import','inspect','importConverted'): kw.setdefault('units','mm')
        return dict(schemaVersion=1, operation=name,returnInlineScene=True, **kw)
    create = op('create', vertices=[[0,0,0],[10,0,0],[0,10,0]], triangles=[[0,1,2]])
    result = run([create,op('pick', origin=[1,1,2], direction=[0,0,-1]),op('paint',expectedRevision=1,faces=[0],color=[1,0,0,1]),op('save',expectedRevision=2),op('exportManufacturing',expectedRevision=2)])
    assert result['results'][1]['result']['faceIndex']==0
    export = result['results'][-1]['result']
    for artifact in export['artifacts']:
        file = pathlib.Path(artifact['path'])
        assert hashlib.sha256(file.read_bytes()).hexdigest()==artifact['sha256']
        assert len(file.read_bytes())==artifact['sizeBytes']
    loaded = run([op('load',documentId=export['documentId'])])
    assert loaded['results'][0]['result']['faceColors']==[[1,0,0,1]]
    stale = run([create,op('transform',expectedRevision=0,translation=[1,0,0])],False)
    assert stale['results'][-1]['error']['code']=='STALE_REVISION'
    escaped = run([create,op('exportManufacturing',expectedRevision=1,outputPath=str(root.parent/'escaped.obj'))],False)
    assert escaped['results'][-1]['error']['code']=='OUTPUT_PATH'
    candidate = run([create,op('prepareCandidate',expectedRevision=1,command={'operation':'transform','translation':[0,0,2]})])
    cid=candidate['results'][-1]['result']['candidateId']
    applied=run([create,op('prepareCandidate',expectedRevision=1,command={'operation':'transform','translation':[0,0,2]}),op('acceptCandidate',expectedRevision=1,candidateId=cid),op('undo',expectedRevision=2)])
    assert applied['results'][-1]['result']['vertices']==[[0,0,0],[10,0,0],[0,10,0]]
    replaced=run([create,op('prepareCandidate',expectedRevision=1,command={'operation':'transform','translation':[0,0,3]}),op('acceptCandidate',expectedRevision=1,candidateId=cid)],False)
    assert replaced['results'][-1]['error']['code']=='STALE_CANDIDATE'
    binding=run([create,op('manufacturingSnapshot',expectedRevision=1)])['results'][-1]['result']
    assert binding['nativeRequest']['projectId']==binding['documentId']
    assert binding['nativeRequest']['inputSha256']==binding['manufacturingGeometrySha256']
    assert hashlib.sha256(pathlib.Path(binding['snapshotPath']).read_bytes()).hexdigest()==binding['snapshotSha256']
    region = run([create,op('selectRegion',seedFace=0)])
    assert region['results'][-1]['result']['faces']==[0]
    descriptor = run([dict(create,returnInlineScene=False)])['results'][0]['result']
    assert 'vertices' not in descriptor
    scene_file=pathlib.Path(descriptor['scenePath'])
    assert scene_file.read_bytes()[:8]==b'ORCASC01'
    assert hashlib.sha256(scene_file.read_bytes()).hexdigest()==descriptor['sceneSha256']
    obj=root/'converted.obj'
    obj.write_text('v 0 0 0\nv 2 0 0\nv 0 2 0\nf 1 2 3\n',encoding='utf-8')
    appearance=root/'converted.appearance.json'
    appearance.write_text(json.dumps({'schemaVersion':1,'units':'mm','upAxis':'Z',
        'geometrySha256':hashlib.sha256(obj.read_bytes()).hexdigest(),
        'faceColors':[[0,0,1,1]],'materialIds':[0],
        'warnings':['TEXTURE_TO_FACE_COLOR_REQUIRES_MAPPING']}),encoding='utf-8')
    converted=run([op('importConverted',inputPath=str(obj),appearancePath=str(appearance))])['results'][0]['result']
    assert converted['faceColors']==[[0,0,1,1]]
    assert converted['appearanceMetadata']['warnings']==['TEXTURE_TO_FACE_COLOR_REQUIRES_MAPPING']
    raw=json.loads(appearance.read_text());raw['geometrySha256']='0'*64
    appearance.write_text(json.dumps(raw),encoding='utf-8')
    wrong=run([op('importConverted',inputPath=str(obj),appearancePath=str(appearance))],False)
    assert wrong['results'][0]['error']['code']=='GEOMETRY_HASH_MISMATCH'
    colored=root/'vertex-color.obj'
    colored.write_text('v 0 0 0 1 0 0\nv 1 0 0 0 1 0\nv 0 1 0 0 0 1\nf 1 2 3\n')
    rejected=run([op('import',inputPath=str(colored))],False)
    assert rejected['results'][0]['error']['code']=='CONVERSION_REQUIRED'
    ply=root/'colored.ply'
    ply.write_text('ply\nformat ascii 1.0\nelement vertex 3\nproperty float x\nproperty float y\nproperty float z\nproperty uchar red\nelement face 1\nproperty list uchar int vertex_indices\nend_header\n0 0 0 255\n1 0 0 0\n0 1 0 0\n3 0 1 2\n')
    rejected=run([op('import',inputPath=str(ply))],False)
    assert rejected['results'][0]['error']['code']=='CONVERSION_REQUIRED'
    obj.write_text('o a\nv 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\no b\nv 10 0 0\nv 11 0 0\nv 10 1 0\nf 4 5 6\n')
    appearance.write_text(json.dumps({'schemaVersion':1,'units':'mm','upAxis':'Z',
        'geometrySha256':hashlib.sha256(obj.read_bytes()).hexdigest(),'faceColors':[[1,0,0,1],[0,0,1,1]],'materialIds':[0,1],
        'parts':[{'entityId':'a','firstVertex':0,'vertexCount':3,'firstTriangle':0,'triangleCount':1},
                 {'entityId':'b','firstVertex':3,'vertexCount':3,'firstTriangle':1,'triangleCount':1}]}))
    shifted=run([op('importConverted',inputPath=str(obj),appearancePath=str(appearance)),op('transform',expectedRevision=1,entityId='b',translation=[0,0,5])])['results'][-1]['result']
    assert shifted['vertices'][0]==[0,0,0] and shifted['vertices'][3]==[10,0,5]
    print('runner: create/pick/paint/save/load/export hashes/stale/path traversal/candidate/undo/native region/SC01 descriptor passed')
