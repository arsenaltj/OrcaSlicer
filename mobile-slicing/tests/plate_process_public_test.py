"""Actual public import/reader/scanner/source-profile/validate/slice regression.
Run: python plate_process_public_test.py DLL NEW_OUTPUT
No mock response; every result and original source hash is saved.
"""
import argparse,copy,ctypes as C,hashlib,json,os,re,zipfile
from pathlib import Path
import xml.etree.ElementTree as ET
from three_mf_component_world_test import make_source,IDENTITY
from skirt_angle_cases import exercise as exercise_skirt_angles

def main():
    ap=argparse.ArgumentParser();ap.add_argument('library',type=Path);ap.add_argument('output',type=Path);a=ap.parse_args()
    root=a.output.resolve();root.mkdir(parents=True,exist_ok=False)
    dll_dir=os.add_dll_directory(str(a.library.resolve().parent))
    lib=C.CDLL(str(a.library.resolve()));CB=C.CFUNCTYPE(None,C.c_char_p,C.c_void_p)
    lib.orca_mobile_slicing_create.argtypes=[C.c_char_p];lib.orca_mobile_slicing_create.restype=C.c_void_p
    lib.orca_mobile_slicing_execute.argtypes=[C.c_void_p,C.c_char_p,CB,C.c_void_p,C.POINTER(C.c_void_p)]
    lib.orca_mobile_slicing_free_string.argtypes=[C.c_void_p];lib.orca_mobile_slicing_destroy.argtypes=[C.c_void_p]
    h=lib.orca_mobile_slicing_create(str(root).encode());rows=[]
    def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
    def run(name,path,operation='import',**extra):
        req=dict(schemaVersion=2,operation=operation,inputPath=str(path),projectId=name,projectRevision=1,maxThreads=1,outputDir=str(root/name),quality='standard',**extra)
        raw=C.c_void_p();before=sha(path);events=[]
        @CB
        def cb(data,_):events.append(json.loads(data))
        code=lib.orca_mobile_slicing_execute(h,json.dumps(req).encode(),cb,None,C.byref(raw))
        try:out=json.loads(C.string_at(raw))
        finally:lib.orca_mobile_slicing_free_string(raw)
        assert before==sha(path)
        row=dict(name=name,request=req,status=code,response=out,events=events,sourceSha256=before);rows.append(row)
        (root/'results.json').write_text(json.dumps(dict(librarySha256=sha(a.library),runs=rows),indent=2))
        return code,out.get('result',out)
    def success(name,path,operation='import',**extra):
        code,out=run(name,path,operation,**extra);assert code==0,(name,out);return out
    initial=root/'initial.3mf';make_source(initial,[IDENTITY,[1,0,0,0,1,0,0,0,1,40,0,0]],flat=True)
    basis=success('basis',initial,'exportProject',confirmDerivedProject=True,config={'before_layer_change_gcode':'G92 E0','filament_density':[1.24],'filament_diameter':[1.75]})
    basis=Path(basis['artifacts'][0]['path'])
    legal={'layer_height':'0.16','wall_loops':'4','sparse_infill_density':'23%','sparse_infill_pattern':'gyroid','outer_wall_line_width':'110%','outer_wall_speed':'50','enable_support':'0','support_type':'normal(auto)','support_threshold_angle':'38','support_on_build_plate_only':'1','wall_generator':'arachne'}
    def project(name,values,second=None,duplicates=()):
        path=root/(name+'.3mf')
        with zipfile.ZipFile(basis) as src,zipfile.ZipFile(path,'x') as dst:
            for info in src.infolist():
                data=src.read(info.filename)
                if info.filename.lower()=='metadata/model_settings.config':
                    doc=ET.fromstring(data);plates=doc.findall('plate');assert len(plates)==1
                    plate=plates[0]
                    for item in list(plate):
                        if item.tag=='metadata' and item.get('key','').startswith('orca_plate_process:'):plate.remove(item)
                    for key,value in list(values.items())+list(duplicates):ET.SubElement(plate,'metadata',key='orca_plate_process:'+key,value=value)
                    if second is not None:
                        other=copy.deepcopy(plate)
                        for item in list(other):
                            if item.tag=='metadata' and item.get('key')=='plater_id':item.set('value','2')
                            if item.tag=='metadata' and item.get('key','').startswith('orca_plate_process:'):other.remove(item)
                        for key,value in second.items():ET.SubElement(other,'metadata',key='orca_plate_process:'+key,value=value)
                        # Each build instance belongs to exactly one source plate.
                        first_members=plate.findall('model_instance');other_members=other.findall('model_instance')
                        assert len(first_members)==len(other_members)==2
                        plate.remove(first_members[1]);other.remove(other_members[0])
                        doc.append(other)
                    data=ET.tostring(doc,encoding='utf-8')
                dst.writestr(info,data)
        return path
    try:
        exercise_skirt_angles(run, success, basis, root)
        valid=project('all11',legal)
        imported=success('all11-import',valid)
        native=imported['project'];assert not native['blockingFeatures'],native['blockingFeatures']
        source=json.loads(Path(native['sourceProfilePath']).read_text())['config']
        declared=native['threeMfInspection']['plates'][0]['validatedNativeProcess']
        assert len(declared)==11
        with zipfile.ZipFile(basis) as original:
            global_config=json.loads(original.read('Metadata/project_settings.config'))
        assert float(global_config['layer_height'])!=float(source['layer_height'])
        assert source['layer_height']==declared['layer_height']
        for key,value in declared.items():assert source[key]==value,(key,source[key],value)
        changed=success('explicit-override',valid,config={'layer_height':'0.20','wall_loops':'6'})['project']
        original=json.loads(Path(changed['sourceProfilePath']).read_text())['config'];assert original['layer_height']=='0.16' and original['wall_loops']=='4'
        assert changed['config']['layer_height']=='0.2' and changed['config']['wall_loops']=='6'
        exported=success('all11-export',valid,'exportProject',confirmDerivedProject=True)
        again=success('all11-reimport',Path(exported['artifacts'][0]['path']))['project']
        assert not again['blockingFeatures']
        for key,value in declared.items():assert again['config'][key]==value,(key,again['config'][key],value)
        for operation in ('validate','slice'):
            out=success('all11-'+operation,valid,operation,executeEffectiveConfig=True)
            if operation=='slice':
                for key,value in declared.items():assert out['effectiveConfig'][key]==value,(key,out['effectiveConfig'][key],value)
                assert Path(out['gcodePath']).is_file() and Path(out['previewPath']).is_file()
                assert re.search(r'^G[123] .*E',Path(out['gcodePath']).read_text(),re.M)
        other=dict(legal,layer_height='0.24',wall_loops='5');multi=project('two-plates',legal,other)
        unselected=success('unselected',multi);assert unselected['project']['blockingFeatures']
        for pid,expected in [('1',legal),('2',other)]:
            out=success('selected-'+pid,multi,selectedPlateId=pid)['project']
            cfg=json.loads(Path(out['sourceProfilePath']).read_text())['config']
            assert cfg['layer_height']==expected['layer_height'] and cfg['wall_loops']==expected['wall_loops']
        bad=[('layer_height','0'),('layer_height','9'),('layer_height','NaN'),('layer_height','0.2garbage'),('wall_loops','1.5'),('wall_loops','1001'),('sparse_infill_density','101%'),('sparse_infill_pattern','unknown'),('outer_wall_line_width','10.1'),('outer_wall_line_width','9'),('outer_wall_speed','50,nan'),('outer_wall_speed','0'),('enable_support','true'),('support_type','none'),('support_threshold_angle','91'),('support_on_build_plate_only','false'),('wall_generator','unknown'),('future','1'),('layer_height','1'*1025)]
        for i,(key,value) in enumerate(bad):
            values=dict(legal);values[key]=value;path=project('invalid-'+str(i),values)
            code,out=run('invalid-'+str(i)+'-override',path,'slice',executeEffectiveConfig=True,config={'layer_height':'0.2','wall_loops':'3','outer_wall_line_width':'0.4'})
            assert code!=0,(key,value,out)
            assert not list((root/('invalid-'+str(i)+'-override')).rglob('*.gcode'))
            # Invalid source on an unselected plate is still a blocker.
            path=project('bad-other-'+str(i),legal,values)
            code,out=run('bad-other-'+str(i),path,'slice',selectedPlateId='1',executeEffectiveConfig=True)
            assert code!=0,(key,value,out)
        conflict=project('conflicting',legal,duplicates=[('wall_loops','9')]);assert run('conflicting',conflict,'slice',executeEffectiveConfig=True)[0]!=0
        for key in ('orca_workbench_slicing_v1','orca_workbench_curve_v1:1','orca_workbench_curve_v2:1'):
            path=project('unknown-'+key.replace(':','_'),legal)
            # Keep actual native archive and add exact unsupported metadata.
            changed=root/(path.stem+'-session.3mf')
            with zipfile.ZipFile(path) as src,zipfile.ZipFile(changed,'x') as dst:
                for info in src.infolist():
                    data=src.read(info.filename)
                    if info.filename.lower()=='metadata/model_settings.config':
                        doc=ET.fromstring(data);ET.SubElement(doc.find('plate'),'metadata',key=key,value='{}');data=ET.tostring(doc)
                    dst.writestr(info,data)
            assert run('blocked-'+path.stem,changed,'slice',executeEffectiveConfig=True)[0]!=0
        def reserved_context(name,key,value,container):
            destination=root/(name+'.3mf');sidecar_seen=False
            stats=ET.Element('config');stat_plate=ET.SubElement(stats,'plate')
            ET.SubElement(stat_plate,'metadata',key='index',value='1');ET.SubElement(stat_plate,'metadata',key='prediction',value='0');ET.SubElement(stat_plate,'metadata',key='weight',value='0')
            if container=='slice-info':ET.SubElement(stat_plate,'metadata',key=key,value=value)
            with zipfile.ZipFile(valid) as src,zipfile.ZipFile(destination,'x') as dst:
                for info in src.infolist():
                    data=src.read(info.filename)
                    if info.filename.lower()=='metadata/model_settings.config' and not container.startswith('ordinary-stats'):
                        doc=ET.fromstring(data);plate=doc.find('plate')
                        for item in list(plate):
                            if item.tag=='metadata' and item.get('key')==key:plate.remove(item)
                        target={'member':plate.find('model_instance'),'object':doc.find('object'),'part':doc.find('.//part'),'root':doc}.get(container)
                        if container in ('member','object','part','root'):
                            assert target is not None,container
                            ET.SubElement(target,'metadata',key=key,value=value)
                        data=ET.tostring(doc)
                    if info.filename.lower()=='metadata/slice_info.config':
                        data=ET.tostring(stats);sidecar_seen=True
                        if container=='ordinary-stats-incomplete':data=data.removesuffix(b'</config>')
                    dst.writestr(info,data)
                if not sidecar_seen:
                    stats_data=ET.tostring(stats)
                    if container=='ordinary-stats-incomplete':stats_data=stats_data.removesuffix(b'</config>')
                    dst.writestr('Metadata/slice_info.config',stats_data)
            return destination
        for container in ('slice-info','member','object','part','root'):
            for key,value in [('orca_plate_process:outer_wall_line_width','9'),('orca_workbench_slicing_v1','{}'),('orca_workbench_curve_v2:1','{}')]:
                name='wrong-context-'+container+'-'+key.replace(':','_')
                path=reserved_context(name,key,value,container)
                code,out=run(name,path,'slice',executeEffectiveConfig=True,config={'outer_wall_line_width':'0.4'})
                assert code!=0,(name,out)
                assert not list((root/name).rglob('*.gcode'))
        statistics=reserved_context('ordinary-statistics','','','ordinary-stats')
        normal=success('ordinary-statistics-slice',statistics,'slice',executeEffectiveConfig=True)
        assert Path(normal['gcodePath']).is_file()
        incomplete=reserved_context('ordinary-statistics-incomplete','','','ordinary-stats-incomplete')
        ordinary_import=success('ordinary-statistics-incomplete-import',incomplete)['project']
        # Immutable old/new same-input controls prove the pre-existing Native
        # warning contract: import succeeds but manufacturing remains blocked.
        blockers=ordinary_import['blockingFeatures']
        assert len(blockers)==1 and blockers[0]['id']=='native_import_warning'
        assert blockers[0]['status']=='blocked' and blockers[0]['blocksAllOutputModes'] is True
        assert 'Error (no element found) while parsing xml file' in blockers[0]['message']
        assert all(item['id']!='xml_parse_failure' for item in ordinary_import['threeMfInspection']['features'])
        # A type-valid source without a declared nozzle does not prove an
        # invalid physical source profile. Import remains available; execution
        # will require a legal effective profile, as before.
        missing=root/'missing-source-nozzle.3mf'
        with zipfile.ZipFile(valid) as src,zipfile.ZipFile(missing,'x') as dst:
            for info in src.infolist():
                data=src.read(info.filename)
                if info.filename.lower()=='metadata/project_settings.config':
                    cfg=json.loads(data);cfg.pop('nozzle_diameter',None);data=json.dumps(cfg).encode()
                if info.filename.lower()=='metadata/model_settings.config':
                    doc=ET.fromstring(data)
                    for item in doc.find('plate').findall('metadata'):
                        if item.get('key')=='orca_plate_process:outer_wall_line_width':item.set('value','3')
                    data=ET.tostring(doc)
                dst.writestr(info,data)
        preview=success('missing-source-nozzle-import',missing)['project']
        # The original explicit mapping has no source nozzle slot. Both old
        # and new Native preserve this mapping blocker without a new physical
        # missing-profile failure; keep this original negative control.
        mapping_blockers=preview['blockingFeatures']
        assert len(mapping_blockers)==1 and mapping_blockers[0]['id']=='invalid_plate_mapping:filament_maps'
        assert mapping_blockers[0]['status']=='blocked' and mapping_blockers[0]['blocksAllOutputModes'] is True
        assert 'PLATE_PROCESS_SOURCE_NATIVE_INVALID' not in json.dumps(preview)
        # Independent positive preview control: empty only the explicit map
        # metadata to Native defaults, keeping absent nozzle and carried width3.
        # This import preview is not proof of an executable slicing profile.
        no_map=root/'missing-source-nozzle-empty-default-mappings.3mf'
        with zipfile.ZipFile(missing) as src,zipfile.ZipFile(no_map,'x') as dst:
            for info in src.infolist():
                data=src.read(info.filename)
                if info.filename.lower()=='metadata/model_settings.config':
                    doc=ET.fromstring(data)
                    for item in doc.findall('plate/metadata'):
                        if item.get('key') in ('filament_maps','filament_volume_maps'):item.set('value','')
                    data=ET.tostring(doc)
                dst.writestr(info,data)
        with zipfile.ZipFile(no_map) as src:
            assert 'nozzle_diameter' not in json.loads(src.read('Metadata/project_settings.config'))
            doc=ET.fromstring(src.read('Metadata/model_settings.config'))
            assert next(item.get('value') for item in doc.findall('plate/metadata') if item.get('key')=='orca_plate_process:outer_wall_line_width')=='3'
        preview_positive=success('missing-source-nozzle-empty-map-import',no_map)['project']
        assert not preview_positive['blockingFeatures']
        recovery=success('recovery-after-invalid',valid,'slice',executeEffectiveConfig=True)
        assert Path(recovery['gcodePath']).is_file() and recovery['effectiveConfig']['layer_height']=='0.16'
        print(json.dumps(dict(state='passed',cases=len(rows),scope='actual public native nonGLB source semantics; no independent QA claim')))
    finally:lib.orca_mobile_slicing_destroy(h)
if __name__=='__main__':main()
