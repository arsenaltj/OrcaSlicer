"""Real Windows C ABI color bridge, material routing and enhanced repair regression.

Arguments: fresh Slicing build root, full Model DLL, NEW evidence output root.
No physical printer, network, synthetic native response or cached object is used.
"""
import ctypes as C,json,pathlib,sys,tempfile,hashlib,os,copy,re
base=pathlib.Path(sys.argv[1]).resolve(); model=pathlib.Path(sys.argv[2]).resolve()
dirs=[]
for p in base.rglob("*.dll"):
 try:dirs.append(os.add_dll_directory(str(p.parent)))
 except OSError:pass
dirs.append(os.add_dll_directory(str(model.parent)))
s=C.CDLL(str(base/"bin/orca_mobile_slicing_sdk_c.dll"));m=C.CDLL(str(model));CB=C.CFUNCTYPE(None,C.c_char_p,C.c_void_p)
@CB
def event(raw,user):pass
for lib,prefix in [(s,"orca_mobile_slicing"),(m,"orca_model")]:
 getattr(lib,prefix+"_create").argtypes=[C.c_char_p];getattr(lib,prefix+"_create").restype=C.c_void_p
 getattr(lib,prefix+"_execute").argtypes=[C.c_void_p,C.c_char_p,CB,C.c_void_p,C.POINTER(C.c_void_p)]
 getattr(lib,prefix+"_free_string").argtypes=[C.c_void_p]
 getattr(lib,prefix+"_destroy").argtypes=[C.c_void_p]
root=pathlib.Path(sys.argv[3]).resolve()
fixture=pathlib.Path(sys.argv[4]).resolve()
assert not root.exists(), 'Use a new evidence directory; previous runs remain immutable'
root.mkdir(parents=True)
sh=s.orca_mobile_slicing_create(str(root).encode());mh=m.orca_model_create(str(root).encode())
def run(lib,prefix,handle,request,name):
 raw=C.c_void_p();code=getattr(lib,prefix+"_execute")(handle,json.dumps(request).encode(),event,None,C.byref(raw))
 try:r=json.loads(C.string_at(raw))
 finally:getattr(lib,prefix+"_free_string")(raw)
 (root/(name+".request.json")).write_text(json.dumps(request,indent=2))
 (root/(name+".result.json")).write_text(json.dumps(r,indent=2))
 assert code==0 and r["ok"],r
 return r["result"]
def S(request,name):return run(s,"orca_mobile_slicing",sh,dict(schemaVersion=2,**request),name)
def M(request,name):return run(m,"orca_model",mh,dict(schemaVersion=1,**request),name)

# A real ordinary request produced from the public Model/manufacturing chain is
# the fixture. The review counterexample changes only self1,2 -> self1.
originalBytes=fixture.read_bytes();original=json.loads(originalBytes)
inputFile=pathlib.Path(original['inputPath']);sourceDigest=hashlib.sha256(inputFile.read_bytes()).hexdigest()
record=dict(fixture=str(fixture),fixtureSha256=hashlib.sha256(originalBytes).hexdigest(),inputPath=str(inputFile),inputSha256=sourceDigest,actualPid=os.getpid(),sdkSha256=hashlib.sha256((base/'bin/orca_mobile_slicing_sdk_c.dll').read_bytes()).hexdigest(),scope='Normal public C ABI execution tests, no fake result, original request/model unchanged',cases=[])

def execute_case(request,name,success,expected=None):
    request=copy.deepcopy(request);request['projectId']='ordinary-layout-'+name;request['outputDir']=str(root/name)
    request['schemaVersion']=2
    pointer=C.c_void_p();code=s.orca_mobile_slicing_execute(sh,json.dumps(request).encode(),event,None,C.byref(pointer))
    try:response=json.loads(C.string_at(pointer))
    finally:s.orca_mobile_slicing_free_string(pointer)
    (root/(name+'.request.json')).write_text(json.dumps(request,indent=2))
    (root/(name+'.result.json')).write_text(json.dumps(response,indent=2))
    outputs=pathlib.Path(request['outputDir']);executable=[str(p) for p in outputs.rglob('*') if p.is_file() and (p.suffix in ['.gcode','.orcapv'] or p.name.endswith(('.gcode.partial','.orcapv.partial')))] if outputs.exists() else []
    row=dict(name=name,code=code,expectedSuccess=success,actualOk=response.get('ok'),executableOutputs=executable)
    if success:
        assert code==0 and response['ok'],response
        result=response['result'];config=result['effectiveConfig'];assert result['nativeMaterialConfigurationVerified'] is False
        for key,value in expected.items():assert config[key]==value,(name,key,config.get(key),value)
        raw=pathlib.Path(result['gcodePath']).read_bytes();assert hashlib.sha256(raw).hexdigest()==result['gcodeSha256']
        text=raw.decode();tools=[int(x) for x in re.findall(r'^T(\d+)\s*$',text,re.M)];active=result['activeMaterialIds']
        if tools:assert tools[0]==active[0] and set(tools)==set(active)
        else:assert active==[0] and len(config['nozzle_diameter'].split(','))==1,('Only the native one-nozzle logical0 default may omit a T command',active)
        row.update(sourceCommit=result['sourceCommit'],activeMaterialIds=active,effectiveConfigSelection={k:config[k] for k in expected},gcodePath=result['gcodePath'],gcodeSha256=result['gcodeSha256'],rawFanCommands=[line for line in text.splitlines() if line.startswith(('M106','M107'))])
    else:
        assert code!=0 and not response['ok'],response
        assert not executable,(name,executable)
        assert 'MATERIAL_EXECUTION_' in json.dumps(response) or 'COLOR_CONFIG_DEVICE_VARIANT_' in json.dumps(response),response
        row['error']=response.get('error')
    assert hashlib.sha256(inputFile.read_bytes()).hexdigest()==sourceDigest
    assert fixture.read_bytes()==originalBytes
    record['cases'].append(row);(root/'summary.json').write_text(json.dumps(record,indent=2));print(json.dumps(row))

good=copy.deepcopy(original);good['config']['filament_self_index']='1,2'
expected={'nozzle_temperature':'220,255','fan_min_speed':'20,80','filament_flow_ratio':'0.98,1','filament_diameter':'1.75,2.85','filament_self_index':'1,2','max_volumetric_extrusion_rate_slope':'1.8'}
execute_case(good,'legal-ordinary-logical1',True,expected)
for name,key,value in [('review-missing-self','filament_self_index','1'),('empty-self','filament_self_index',''),('duplicate-self','filament_self_index','1,1'),('out-of-range-self','filament_self_index','1,3'),('zero-self','filament_self_index','0,2'),('unknown-variant','filament_extruder_variant','"Unknown";"Direct Drive Standard"'),('mismatched-variant','filament_extruder_variant','"Direct Drive Standard";"Bowden Standard"'),('ambiguous-variant','filament_extruder_variant','"Direct Drive Standard";"Direct Drive Standard";"Direct Drive Standard"'),('invalid-diameter-layout','filament_diameter','1.75,2.85,3')]:
    bad=copy.deepcopy(good);bad['config'][key]=value
    if name=='ambiguous-variant':bad['config']['filament_self_index']='1,1,2'
    execute_case(bad,name,False)
# Several variants for each logical material retain each material's parameters.
multi=copy.deepcopy(good);multi['config'].update(filament_self_index='1,1,2,2',filament_extruder_variant='"Direct Drive Standard";"Direct Drive High Flow";"Direct Drive Standard";"Direct Drive High Flow"',nozzle_temperature='220,230,255,265',fan_min_speed='20,30,80,90',filament_flow_ratio='0.98,0.97,1,0.99')
execute_case(multi,'legal-multiple-variant-rows',True,expected)
perMaterial=copy.deepcopy(multi);perMaterial['config'].update(nozzle_temperature='220,255',fan_min_speed='20,80',filament_flow_ratio='0.98,1')
execute_case(perMaterial,'legal-per-material-to-variants',True,expected)
sharedVariant=copy.deepcopy(good);sharedVariant['config']['filament_extruder_variant']='"Direct Drive Standard"'
execute_case(sharedVariant,'legal-shared-variant-name',True,expected)
shared=copy.deepcopy(multi);shared['config']['nozzle_temperature']='225'
execute_case(shared,'legal-shared-temperature',True,dict(expected,nozzle_temperature='225,225'))
# One material may legitimately own several variants and repeat self1.
cube=root/'one.obj';cube.write_text('v 20 20 0\nv 30 20 0\nv 30 30 0\nv 20 30 0\nv 20 20 10\nv 30 20 10\nv 30 30 10\nv 20 30 10\nf 1 3 2\nf 1 4 3\nf 5 6 7\nf 5 7 8\nf 1 2 6\nf 1 6 5\nf 2 3 7\nf 2 7 6\nf 3 4 8\nf 3 8 7\nf 4 1 5\nf 4 5 8\n')
one=dict(operation='slice',inputPath=str(cube),projectRevision=1,executeEffectiveConfig=True,quality='standard',config={'filament_self_index':'1,1,1,1,1,1','filament_extruder_variant':'"Direct Drive Standard";"Direct Drive High Flow";"Direct Drive Extra High Flow";"Bowden Standard";"Bowden High Flow";"Bowden Extra High Flow"','nozzle_temperature':'220,230,240,250,260,270','nozzle_temperature_initial_layer':'220,230,240,250,260,270','fan_min_speed':'20,30,40,50,60,70','fan_max_speed':'40,50,60,70,80,90','filament_flow_ratio':'0.98,0.97,0.96,0.95,0.94,0.93'})
execute_case(one,'legal-single-material-multiple-variants',True,{'nozzle_temperature':'220','fan_min_speed':'20','filament_flow_ratio':'0.98','filament_self_index':'1'})
record['state']='passed';record['negativeCases']=sum(not x['expectedSuccess'] for x in record['cases']);record['positiveCases']=sum(x['expectedSuccess'] for x in record['cases']);(root/'summary.json').write_text(json.dumps(record,indent=2));print(json.dumps({'state':'passed','positiveCases':record['positiveCases'],'negativeCases':record['negativeCases']}))
s.orca_mobile_slicing_destroy(sh);m.orca_model_destroy(mh)
