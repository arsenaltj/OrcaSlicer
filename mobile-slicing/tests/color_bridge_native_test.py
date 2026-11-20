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
s=C.CDLL(str(base/"Release/orca_mobile_slicing_sdk_c.dll"));m=C.CDLL(str(model));CB=C.CFUNCTYPE(None,C.c_char_p,C.c_void_p)
@CB
def event(raw,user):pass
for lib,prefix in [(s,"orca_mobile_slicing"),(m,"orca_model")]:
 getattr(lib,prefix+"_create").argtypes=[C.c_char_p];getattr(lib,prefix+"_create").restype=C.c_void_p
 getattr(lib,prefix+"_execute").argtypes=[C.c_void_p,C.c_char_p,CB,C.c_void_p,C.POINTER(C.c_void_p)]
 getattr(lib,prefix+"_free_string").argtypes=[C.c_void_p]
 getattr(lib,prefix+"_destroy").argtypes=[C.c_void_p]
root=pathlib.Path(sys.argv[3]).resolve()
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
                 config={'ooze_prevention':'1'},outputDir=str(root/"slicing"))
initial=S(dict(**baseRequest,operation="import"),"import")["project"]
exported=S(dict(**baseRequest,operation="exportColorDocument"),"color-export")
mesh,appearance=exported["artifacts"]
doc=M(dict(operation="importConverted",inputPath=mesh["path"],appearancePath=appearance["path"],units="mm"),"model-import");docid=doc["documentId"];rev=doc["revision"]
M(dict(operation="paint",expectedRevision=rev,faces=list(range(6)),color=[1,0,0,1]),"paint-red");rev+=1
M(dict(operation="paint",expectedRevision=rev,faces=list(range(6,12)),color=[0,0,1,1]),"paint-blue");rev+=1
bindings=[dict(materialId=i,slot=i+4,tool=i+4,materialPresetId="wondermaker/pla-basic",color=["#FF0000","#0000FF"][i],purpose="model") for i in range(2)]
prepared=S(dict(**baseRequest,operation="prepareColorManufacturingConfig",devicePresetId="honor/forge-6-head-0.4",bindings=bindings),"prepare-config")
apex=prepared["manufacturingConfig"];process=prepared["processFingerprint"];material=prepared["materialFingerprint"]
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
print(json.dumps(dict(authorityExport=True,modelTwoMaterials=True,nativeProjectRebind=True,result=result)))
assert result['materialPlanValidationVersion']==2 and result['activeMaterialIds']==[0,1]
assert result['nativeMaterialConfigurationVerified'] is True
assert result['filamentConfirmationSource']=='operator' and result['actualFilamentLoadingVerified'] is False
assert result['requestedConfigSha256']==process and result['processFingerprint']==process
assert result['materialFingerprint']==material and len(result['activeMaterialRoutes'])==2
for route in result['activeMaterialRoutes']:
    assert route['physicalSlot']==route['toolId']==route['materialId']+4
    assert route['materialPresetVersion'] and route['family']=='PLA'

def fails(request,name,error):
    request=dict(schemaVersion=2,**request)
    raw=C.c_void_p();code=s.orca_mobile_slicing_execute(sh,json.dumps(request).encode(),event,None,C.byref(raw))
    try:r=json.loads(C.string_at(raw))
    finally:s.orca_mobile_slicing_free_string(raw)
    (root/(name+'.request.json')).write_text(json.dumps(request,indent=2))
    (root/(name+'.result.json')).write_text(json.dumps(r,indent=2))
    assert code!=0 and not r['ok'] and error in json.dumps(r),(name,r)

def saved_request(name):return json.loads((root/(name+'.request.json')).read_text())
def without_schema(request):return {k:v for k,v in request.items() if k!='schemaVersion'}
request=without_schema(saved_request('slice'));request.pop('bindings')
fails(request,'no-loading-bindings','ACTUAL_FILAMENT_LOADING_REQUIRED')
request=without_schema(saved_request('prepare-config'));request['bindings'][1]['tool']=4
fails(request,'duplicate-tool','COLOR_CONFIG_SLOT_TOOL_INVALID')
request=without_schema(saved_request('prepare-config'));request['bindings'][0]['materialId']=2
fails(request,'missing-material-id','COLOR_CONFIG_SLOT_TOOL_INVALID')
request=without_schema(saved_request('prepare-config'));request['bindings'][1]['materialId']=0
fails(request,'duplicate-material-id','COLOR_CONFIG_SLOT_TOOL_INVALID')
request=without_schema(saved_request('prepare-config'));request['bindings'][1]['slot']=4
fails(request,'duplicate-physical-slot','COLOR_CONFIG_SLOT_TOOL_INVALID')
request=without_schema(saved_request('prepare-config'));request['bindings'][0]['materialPresetVersion']='0'*64
fails(request,'material-version-mismatch','COLOR_CONFIG_MATERIAL_PRESET_VERSION_CHANGED')
request=without_schema(saved_request('prepare-config'));request['bindings'][0]['materialPresetId']='pla-black'
fails(request,'unknown-material-id','COLOR_CONFIG_MATERIAL_PRESET_UNKNOWN')
request=without_schema(saved_request('rebind'));request['projectRevision']=2
fails(request,'stale-project-binding','COLOR_REBIND_SOURCE_CHANGED')
request=without_schema(saved_request('rebind'));request['manufacturingConfig']['nozzle_diameter']='0.8,0.8,0.8,0.8,0.8,0.8'
fails(request,'config-tamper','COLOR_REBIND_MANUFACTURING_CONFIG_CHANGED')
for name,key,value in [
    ('wrong-start-template','machine_start_gcode','START_PRINT EXTRUDER=220 INITIAL_TOOL=4'),
    ('wrong-native-temperature','nozzle_temperature_initial_layer','200,220'),
    ('wrong-initial-tool-expression','machine_start_gcode',apex['machine_start_gcode'].replace('filament_map[initial_tool]-1','initial_tool'))]:
    request=without_schema(saved_request('rebind'));request['manufacturingConfig'][key]=value
    fails(request,name,'COLOR_REBIND_MANUFACTURING_CONFIG_CHANGED')
for name,error,mutate in [
    ('trial-rebind','COLOR_REBIND_PLAN_CHANGED',lambda a:a['appearanceMetadata']['printColorPlan'].update(trialOnly=True)),
    ('geometry-tamper','COLOR_REBIND_GEOMETRY_CHANGED',lambda a:a.update(geometrySha256='0'*64)),
    ('parts-tamper','COLOR_REBIND_PARTS_CHANGED',lambda a:a['appearanceMetadata']['parts'][0].update(entityId='other'))]:
    appearance=json.loads(pathlib.Path(side['path']).read_text());mutate(appearance)
    altered=root/(name+'.appearance.json');altered.write_text(json.dumps(appearance))
    request=without_schema(saved_request('rebind'));request['appearancePath']=str(altered)
    request['appearanceSha256']=hashlib.sha256(altered.read_bytes()).hexdigest()
    fails(request,name,error)

# The desktop enhanced repair entry is included whole and remains behind explicit
# topology/annotation-loss consent. It does not expose the separate full strict API.
repairBase=dict(inputPath=str(source),projectId='repair-positive',projectRevision=1,outputDir=str(root/'repair'))
fails(dict(**repairBase,operation='import',projectCommands=[dict(type='repairMesh')]),
      'repair-needs-consent','TOPOLOGY_CHANGE_CONFIRMATION_REQUIRED')
repaired=S(dict(**repairBase,operation='import',projectCommands=[dict(type='repairMesh',acceptTopologyChange=True)]),'repair-healthy')
assert repaired['project']['repairReports'][0]['sourcePreserved'] is True
hole=root/'hole.obj';hole.write_text('\n'.join(source.read_text().splitlines()[:-2])+'\n')
repaired=S(dict(inputPath=str(hole),projectId='repair-hole',projectRevision=1,outputDir=str(root/'repair-hole'),operation='import',
                projectCommands=[dict(type='repairMesh',acceptTopologyChange=True)]),'repair-hole')
assert repaired['project']['repairReports'][0]['filledHoles']>0
fails(dict(inputPath=project['path'],projectId='repair-painted',projectRevision=1,outputDir=str(root/'repair-painted'),operation='import',
           projectCommands=[dict(type='repairMesh',acceptTopologyChange=True)]),
      'repair-painted-needs-remap','REPAIR_ANNOTATION_REMAP_REQUIRED')

# APEX one active material still carries the same confirmed configuration receipt.
one=S(dict(operation='slice',inputPath=str(source),projectId='one-material',projectRevision=1,
           outputDir=str(root/'one-material'),config=apex,quality='standard',executeEffectiveConfig=True,
           devicePresetId='honor/forge-6-head-0.4',bindings=bindings,confirmMaterialMapping=True),'slice-one-material')
assert one['materialPlanValidationVersion']==1 and one['activeMaterialIds']==[0]
assert one['nativeMaterialConfigurationVerified'] is True and one['filamentConfirmationSource']=='operator'
assert one['actualFilamentLoadingVerified'] is False and len(one['activeMaterialRoutes'])==1
assert one['requestedConfigSha256']==process and one['materialFingerprint']==material
oneReceipt=honor_start_receipt(one,prepared,4)
# Also exercise a genuinely single loaded material, without invented unused spools.
singleBindings=[bindings[0]]
singlePrepared=S(dict(**baseRequest,operation='prepareColorManufacturingConfig',devicePresetId='honor/forge-6-head-0.4',
                      bindings=singleBindings),'single-binding-prepare')
assert len(singlePrepared['palette'])==len(singlePrepared['bindings'])==1
single=S(dict(operation='slice',inputPath=str(source),projectId='single-binding',projectRevision=1,
              outputDir=str(root/'single-binding'),config=singlePrepared['manufacturingConfig'],quality='standard',executeEffectiveConfig=True,
              devicePresetId='honor/forge-6-head-0.4',bindings=singleBindings,confirmMaterialMapping=True),'slice-single-binding')
assert single['materialPlanValidationVersion']==1 and single['activeMaterialIds']==[0]
assert single['requestedConfigSha256']==singlePrepared['processFingerprint']
assert single['materialFingerprint']==singlePrepared['materialFingerprint']
assert single['nativeMaterialConfigurationVerified'] is True and single['filamentConfirmationSource']=='operator'
assert single['actualFilamentLoadingVerified'] is False and single['physicalToolGcodeReparsed'] is True
assert single['activeMaterialRoutes'][0]['physicalSlot']==single['activeMaterialRoutes'][0]['toolId']==4
singleReceipt=honor_start_receipt(single,singlePrepared,4)
(root/'single-thermal-receipts.json').write_text(json.dumps(dict(oneActiveAmongTwoLoaded=oneReceipt,oneActuallyLoaded=singleReceipt),indent=2))

# A dedicated support spool is not part of the model color palette. Binding it
# configures routing while retaining the user's explicit source support switch.
support=root/'overhang.obj'
def box(x0,y0,z0,x1,y1,z1,offset):
    vertices=[(x0,y0,z0),(x1,y0,z0),(x1,y1,z0),(x0,y1,z0),
              (x0,y0,z1),(x1,y0,z1),(x1,y1,z1),(x0,y1,z1)]
    faces=[(1,3,2),(1,4,3),(5,6,7),(5,7,8),(1,2,6),(1,6,5),
           (2,3,7),(2,7,6),(3,4,8),(3,8,7),(4,1,5),(4,5,8)]
    return ''.join('v %s %s %s\n'%v for v in vertices)+''.join(
        'f %d %d %d\n'%tuple(v+offset for v in face) for face in faces)
support.write_text(box(24,24,0,26,26,10,0)+box(20,20,10,30,30,12,8))
supportBase=dict(inputPath=str(support),projectId='support-project',projectRevision=1,
                 config={'enable_support':'1','support_type':'normal(auto)','support_on_build_plate_only':'0'},
                 outputDir=str(root/'support-project'))
supportBindings=[dict(materialId=i,slot=i,tool=i,materialPresetId='wondermaker/pla-basic',
                     color=['#FF0000','#0000FF','#FFFFFF'][i],purpose='support' if i==2 else 'model') for i in range(3)]
supportPrepared=S(dict(**supportBase,operation='prepareColorManufacturingConfig',
                       devicePresetId='honor/forge-6-head-0.4',bindings=supportBindings),'support-prepare')
assert [entry['materialId'] for entry in supportPrepared['palette']]==[0,1]
assert supportPrepared['manufacturingConfig']['enable_support']=='1'
assert supportPrepared['manufacturingConfig']['support_filament']=='3'
assert supportPrepared['manufacturingConfig']['support_interface_filament']=='3'
supportExport=S(dict(**supportBase,operation='exportColorDocument'),'support-color-export')
supportMesh,supportAppearance=supportExport['artifacts']
supportDoc=M(dict(operation='importConverted',inputPath=supportMesh['path'],appearancePath=supportAppearance['path'],units='mm'),'support-model-import')
supportId=supportDoc['documentId'];supportRev=supportDoc['revision']
M(dict(operation='paint',expectedRevision=supportRev,faces=list(range(12)),color=[1,0,0,1]),'support-model-red');supportRev+=1
M(dict(operation='paint',expectedRevision=supportRev,faces=list(range(12,24)),color=[0,0,1,1]),'support-model-blue');supportRev+=1
supportPlan=M(dict(operation='matchPrintColors',documentId=supportId,expectedRevision=supportRev,requestedColorCount=2,
                   palette=supportPrepared['palette'],materialFingerprint=supportPrepared['materialFingerprint'],
                   processFingerprint=supportPrepared['processFingerprint']),'support-match')
M(dict(operation='applyPrintColors',documentId=supportId,expectedRevision=supportRev,candidateId=supportPlan['candidateId'],
       materialFingerprint=supportPrepared['materialFingerprint'],processFingerprint=supportPrepared['processFingerprint'],
       confirmMaterialMapping=True,acceptColorError=True),'support-apply');supportRev+=1
supportManufacturing=M(dict(operation='exportManufacturing',expectedRevision=supportRev),'support-manufacturing')
supportSide=supportManufacturing['artifacts'][1]
supportDerived=S(dict(**supportBase,operation='applyColorDocument',appearancePath=supportSide['path'],appearanceSha256=supportSide['sha256'],
                       manufacturingConfig=supportPrepared['manufacturingConfig'],materialFingerprint=supportPrepared['materialFingerprint'],
                       processFingerprint=supportPrepared['processFingerprint'],devicePresetId='honor/forge-6-head-0.4',
                       bindings=supportBindings,confirmMaterialMapping=True),'support-rebind')['artifacts'][0]
supportRequest=dict(inputPath=supportDerived['path'],projectId='support-slice',projectRevision=1,
                    outputDir=str(root/'support-gcode'),
                    operation='slice',quality='standard',executeEffectiveConfig=True,
                    devicePresetId='honor/forge-6-head-0.4',bindings=supportBindings,confirmMaterialMapping=True,multiMaterialConfirmed=True)
supportSlice=S(supportRequest,'slice-support')
assert supportSlice['activeMaterialIds']==[0,1,2]
supportRoute=next(route for route in supportSlice['activeMaterialRoutes'] if route['materialId']==2)
assert supportRoute['purpose']=='support' and supportRoute['toolId']==2
assert supportSlice['filamentVolumePerSlotMm3']['2']>0
sourceChanged=dict(baseRequest,operation='prepareColorManufacturingConfig',
                   devicePresetId='honor/forge-6-head-0.4',bindings=bindings,
                   config={'layer_height':'0.12','nozzle_diameter':'0.8,0.8,0.8,0.8,0.8,0.8',
                           'machine_start_gcode':'CLIENT_MACHINE_MACRO_MUST_NOT_OVERRIDE'})
qualityPrepared=S(sourceChanged,'quality-source-machine-protected')
assert qualityPrepared['manufacturingConfig']['layer_height']=='0.12'
assert qualityPrepared['manufacturingConfig']['nozzle_diameter']=='0.4,0.4,0.4,0.4,0.4,0.4'
assert 'CLIENT_MACHINE_MACRO' not in qualityPrepared['manufacturingConfig']['machine_start_gcode']
assert qualityPrepared['processFingerprint']!=process
rawGcode=pathlib.Path(result['gcodePath']).read_text()
tools={int(x) for x in re.findall(r'^T(\d+)\s*(?:;[^\n]*)?$',rawGcode,re.M)}
assert tools=={4,5}, ('Raw physical tool commands must match actual sparse bindings',tools)
assert re.search(r'\bINITIAL_TOOL=[45]\b',rawGcode)
temperatures={int(x) for x in re.findall(r'^M10[49][^\n]*\bT(\d+)\b',rawGcode,re.M)}
assert temperatures and temperatures<={4,5}, ('Physical temperature tools must match routing',temperatures)
assert result['physicalToolGcodeReparsed'] is True
generic=S(dict(inputPath=str(source),projectId='generic-unchanged',projectRevision=1,
               outputDir=str(root/'generic'),operation='slice',quality='standard'),'slice-generic-no-adapter')
assert generic['physicalToolGcodeReparsed'] is False and generic['activeMaterialIds']==[0]
summary=dict(state='passed',nativeTwoMaterialGcode=True,nativeSingleMaterialReceipt=True,
             nativeSupportGcode=True,nativeSingleLoadedMaterial=True,negativeCases=17,enhancedRepairHealthyAndHole=True,physicalPrinterVerified=False)
(root/'summary.json').write_text(json.dumps(summary,indent=2));print(json.dumps(summary))
s.orca_mobile_slicing_destroy(sh);m.orca_model_destroy(mh)
