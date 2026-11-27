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
route=json.loads(sys.argv[4])
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
nativeCatalog=S(dict(operation='presets'),'native-presets')
honorPreset=next(item for item in nativeCatalog['devices'] if item['id']=='honor/forge-6-head-0.4')
def honor_start_receipt(result,prepared,tool):
    config=prepared['manufacturingConfig'];original=honorPreset['config']['machine_start_gcode']
    assert original.count('INITIAL_TOOL=[initial_tool]')==1
    mapped=original.replace('INITIAL_TOOL=[initial_tool]','INITIAL_TOOL={filament_map[initial_tool]-1}')
    assert config['printer_model']=='Honor Forge' and config['gcode_flavor']=='klipper'
    assert config['single_extruder_multi_material']=='0'
    assert config['machine_start_gcode']==mapped.replace('\n','\\n')
    adapter=prepared['toolRoutingAdapter'];assert adapter['id']=='honor-forge-physical-tools-v1'
    assert adapter['startTemplateSourceJsonSha256']==hashlib.sha256(json.dumps(original,ensure_ascii=False,separators=(',',':')).encode()).hexdigest()
    gcode=pathlib.Path(result['gcodePath']).read_text();assert hashlib.sha256(gcode.encode()).hexdigest()==result['gcodeSha256']
    starts=re.findall(r'^START_PRINT [^\n]+$',gcode,re.M);assert len(starts)==1
    pairs=re.findall(r'(\w+)=([^\s]+)',starts[0]);values=dict(pairs);assert len(values)==len(pairs)
    assert int(values['INITIAL_TOOL'])==tool
    assert float(values['EXTRUDER'])==float(config['nozzle_temperature_initial_layer'].split(',')[0])
    assert config['curr_bed_type']=='Cool Plate'
    assert float(values['BED'])==float(config['cool_plate_temp_initial_layer'].split(',')[0])
    assert float(values['CHAMBER'])==float(config['chamber_temperature'].split(',')[0])
    direct=re.findall(r'^M10[49][^\n]*$',gcode,re.M)
    assert all(int(target)==tool for line in direct for target in re.findall(r'\bT(\d+)\b',line))
    firstTool=re.search(r'^T'+str(tool)+r'\s*$',gcode,re.M);assert firstTool
    firstExtrusion=next(match for match in re.finditer(r'^G[123] [^\n]*\bE(-?[0-9.]+)[^\n]*$',gcode,re.M) if float(match.group(1))>0)
    assert firstTool.start()<firstExtrusion.start()
    # ARB-COLOR-PHYSICAL-01: this exact native Klipper template owns initial
    # heating. Empty direct commands are accepted only after the full identity,
    # macro arguments and physical initial-tool checks above. Firmware remains unverified.
    return dict(knownNativeTemplate=True,initialTool=tool,directTemperatureCommandCount=len(direct),firmwareMacroVerified=False)
source=root/"cube.obj"
source.write_text("v 20 20 0\nv 30 20 0\nv 30 30 0\nv 20 30 0\nv 20 20 10\nv 30 20 10\nv 30 30 10\nv 20 30 10\nf 1 3 2\nf 1 4 3\nf 5 6 7\nf 5 7 8\nf 1 2 6\nf 1 6 5\nf 2 3 7\nf 2 7 6\nf 3 4 8\nf 3 8 7\nf 4 1 5\nf 4 5 8\n")
baseRequest=dict(inputPath=str(source),projectId="color-roundtrip",projectRevision=1,
                 config={'ooze_prevention':'1','max_volumetric_extrusion_rate_slope':'1.8','max_volumetric_extrusion_rate_slope_segment_length':'0.5','outer_wall_speed':'10','inner_wall_speed':'10','sparse_infill_speed':'10','internal_solid_infill_speed':'10','top_surface_speed':'10','initial_layer_speed':'10','initial_layer_infill_speed':'10'},outputDir=str(root/"slicing"))
initial=S(dict(**baseRequest,operation="import"),"import")["project"]
exported=S(dict(**baseRequest,operation="exportColorDocument"),"color-export")
mesh,appearance=exported["artifacts"]
doc=M(dict(operation="importConverted",inputPath=mesh["path"],appearancePath=appearance["path"],units="mm"),"model-import");docid=doc["documentId"];rev=doc["revision"]
M(dict(operation="paint",expectedRevision=rev,faces=list(range(6)),color=[1,0,0,1]),"paint-red");rev+=1
M(dict(operation="paint",expectedRevision=rev,faces=list(range(6,12)),color=[0,0,1,1]),"paint-blue");rev+=1
bindings=[dict(materialId=i,slot=route[i],tool=route[i],materialPresetId=["generic/pla","generic/petg"][i],color=["#FF0000","#0000FF"][i],purpose="model") for i in range(2)]
prepared=S(dict(**baseRequest,operation="prepareColorManufacturingConfig",devicePresetId="honor/forge-6-head-0.4",bindings=bindings),"prepare-config")
apex=prepared["manufacturingConfig"];assert float(apex["max_volumetric_extrusion_rate_slope"])==1.8;assert float(apex["max_volumetric_extrusion_rate_slope_segment_length"])==0.5;process=prepared["processFingerprint"];material=prepared["materialFingerprint"]
planArgs=dict(documentId=docid,expectedRevision=rev,requestedColorCount=2,materialFingerprint=material,processFingerprint=process,palette=prepared["palette"])
candidate=M(dict(operation="matchPrintColors",**planArgs),"match")
M(dict(operation="applyPrintColors",documentId=docid,expectedRevision=rev,candidateId=candidate["candidateId"],materialFingerprint=material,processFingerprint=process,confirmMaterialMapping=True,acceptColorError=True),"apply");rev+=1
manufacturing=M(dict(operation="exportManufacturing",expectedRevision=rev),"manufacturing")
side=manufacturing["artifacts"][1]
derivative=S(dict(**baseRequest,operation="applyColorDocument",appearancePath=side["path"],appearanceSha256=side["sha256"],materialFingerprint=material,processFingerprint=process,manufacturingConfig=apex,devicePresetId="honor/forge-6-head-0.4",bindings=bindings,confirmMaterialMapping=True),"rebind")
project=derivative["artifacts"][0]
readback=S(dict(operation="import",inputPath=project["path"],projectId="readback",projectRevision=1,outputDir=str(root/"readback")),"readback")
readbackPrepared=S(dict(operation="prepareColorManufacturingConfig",inputPath=project["path"],projectId="readback",projectRevision=1,outputDir=str(root/"readback"),devicePresetId="honor/forge-6-head-0.4",bindings=bindings),"readback-prepare")
result=S(dict(operation="slice",inputPath=project["path"],projectId="readback",projectRevision=1,outputDir=str(root/"gcode"),quality="standard",executeEffectiveConfig=True,multiMaterialConfirmed=True,devicePresetId="honor/forge-6-head-0.4",bindings=bindings,confirmMaterialMapping=True),"slice")


expected={'nozzle_temperature':'220,255','nozzle_temperature_initial_layer':'220,255','fan_min_speed':'100,20','filament_flow_ratio':'0.98,1','slow_down_layer_time':'4,8','filament_self_index':'1,2'}
for key,value in expected.items():
    assert prepared['manufacturingConfig'][key]==value,(key,prepared['manufacturingConfig'][key])
    assert result['effectiveConfig'][key]==value,(key,result['effectiveConfig'][key])
assert result['nativeMaterialConfigurationVerified'] is True
assert result['activeMaterialIds']==[0,1] and result['physicalToolGcodeReparsed'] is True
reverse=S(dict(**baseRequest,operation='prepareColorManufacturingConfig',devicePresetId='honor/forge-6-head-0.4',bindings=list(reversed(bindings))),'reversed-bindings')
assert reverse['manufacturingConfig']==prepared['manufacturingConfig']
assert reverse['processFingerprint']==prepared['processFingerprint']
duplicate=copy.deepcopy(bindings);duplicate[1]['materialPresetId']=duplicate[0]['materialPresetId']
dup=S(dict(**baseRequest,operation='prepareColorManufacturingConfig',devicePresetId='honor/forge-6-head-0.4',bindings=duplicate),'duplicate-preset-colors')
assert dup['manufacturingConfig']['filament_self_index']=='1,2'
assert dup['manufacturingConfig']['nozzle_temperature']=='220,220'
assert dup['manufacturingConfig']['filament_colour']=='#FF0000;#0000FF'
# Rebinding must reject client-authored machine/material variant claims. The
# catalog's Honor machine is DD Standard; a non-first legal variant is tested
# with the internal trusted machine fixture, never a production test preset.
negative=0
for key,value in [('nozzle_volume_type','High Flow'),('filament_extruder_variant','"Unknown variant";"Direct Drive Standard"'),('filament_self_index','1,1')]:
    request=json.loads((root/'rebind.request.json').read_text());request['manufacturingConfig'][key]=value
    ptr=C.c_void_p();code=s.orca_mobile_slicing_execute(sh,json.dumps(request).encode(),event,None,C.byref(ptr))
    try:response=json.loads(C.string_at(ptr))
    finally:s.orca_mobile_slicing_free_string(ptr)
    (root/('tamper-'+key+'.request.json')).write_text(json.dumps(request,indent=2))
    (root/('tamper-'+key+'.result.json')).write_text(json.dumps(response,indent=2))
    assert code!=0 and not response['ok'] and 'COLOR_REBIND_MANUFACTURING_CONFIG_CHANGED' in json.dumps(response)
    negative+=1
g=pathlib.Path(result['gcodePath']);raw=g.read_bytes();assert hashlib.sha256(raw).hexdigest()==result['gcodeSha256'];text=raw.decode()
tools=[int(v) for v in re.findall(r'^T(\d+)\s*$',text,re.M)];assert set(tools)==set(route)
fans=[line for line in text.splitlines() if line.startswith(('M106','M107'))]
mainFans=[int(v) for v in re.findall(r'^M106(?: P0)? S([0-9]+)',text,re.M)]
assert 255 in mainFans and any(0<x<255 for x in mainFans),set(mainFans)
feed=[line.split(';',1)[0].strip() for line in text.splitlines() if re.match(r'^G[123] ',line) and re.search(r'\bF[0-9]',line)]
# The start macro owns initial heating; later explicit tool-qualified commands
# must carry the independently known native PLA/PETG temperatures.
assert re.search(r'^START_PRINT .*EXTRUDER=220 .*INITIAL_TOOL='+str(route[0])+r'\b',text,re.M)
thermal={route[0]:{220},route[1]:set()};current=route[0]
for line in text.splitlines():
    change=re.match(r'^T(\d+)\s*$',line)
    if change:current=int(change.group(1))
    if re.match(r'^M10[49]\s',line):
        temp=re.search(r'\bS([0-9.]+)',line);tool=re.search(r'\bT(\d+)',line)
        if temp and float(temp.group(1))>0:thermal.setdefault(int(tool.group(1)) if tool else current,set()).add(float(temp.group(1)))
assert 255 in thermal[route[1]],thermal
summary=dict(state='passed',actualPid=os.getpid(),sourceCommit=result['sourceCommit'],physicalRoute=route,effectiveConfigSelection={k:result['effectiveConfig'][k] for k in expected},gcodePath=str(g),gcodeSha256=result['gcodeSha256'],thermalByPhysicalTool={str(k):sorted(v) for k,v in thermal.items()},rawFanCommands=fans,mainFanValues=sorted(set(mainFans)),rawFeedCommandCount=len(feed),toolSequence=tools,negativeCases=negative,fullModelManufacturingSlice=True,scope='Development complete normal C ABI chain. Non-first machine variant is internal unit coverage, Honor diameter is native 1.75; no firmware/physical print claim.')
(root/'summary.json').write_text(json.dumps(summary,indent=2));(root/'derived-feed-analysis.json').write_text(json.dumps(feed));print(json.dumps(summary))
s.orca_mobile_slicing_destroy(sh);m.orca_model_destroy(mh)
