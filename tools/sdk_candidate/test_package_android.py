"""Synthetic payloads and read-only metadata; no SDK, native tools or real raw reads."""
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import types
import unittest
from unittest.mock import patch
import adapter as a
import package_android as p
import android_elf as elf
import artifact_owner as owner
import zipfile


def elf_fixture(path):
    names=b'\0.text\0.dynsym\0.dynstr\0.dynamic\0.debug_info\0.shstrtab\0'
    data=bytearray(16384)
    data[:64]=struct.pack('<16sHHIQQQIHHHHHH',b'\x7fELF\x02\x01'+bytes(10),3,183,1,0,64,8192,0,64,56,1,64,7,6)
    data[64:120]=struct.pack('<IIQQQQQQ',1,5,0,0,0,4096,4096,16384)
    sections=[(0,0,0,0,0,0,0,0,0,0)]
    for idx,name in enumerate(('.text','.dynsym','.dynstr','.dynamic')):
        offset=1024+idx*32;data[offset:offset+16]=bytes([idx+1])*16
        sections.append((names.index(name.encode()),1,2,offset,offset,16,0,0,8,0))
    sections.append((names.index(b'.debug_info'),1,0,0,7000,16,0,0,1,0))
    data[7500:7500+len(names)]=names
    sections.append((names.index(b'.shstrtab'),3,0,0,7500,len(names),0,0,1,0))
    for idx,row in enumerate(sections):data[8192+idx*64:8192+(idx+1)*64]=struct.pack('<IIQQQQIIQQ',*row)
    data[2048:2088]=a.SOURCE.encode();path.write_bytes(data)
    return path


def producer(root,stage):
    d=root/stage;d.mkdir();raw=d/p.NAMES[stage];raw.write_bytes(b'fake-qualified-raw')
    raw_source=dict(source=a.SOURCE,buildPath=str(raw),raw=a.identity(raw),cleanBuild=True,
                    copiedPredecessorObjects=0,priorObjectsReused=False)
    records=[dict(name=n,state='returned',exitCode=0,monitorErrors=0,runtimeViolations=0) for n in
             ('configure','actual-ninja-commands','build','actual-ninja-deps')]
    execution=d/'execution.json';execution.write_bytes(b'not JSON; streamed original execution')
    summary=dict(source=a.SOURCE,records=records,commands=records,
                 originalRecordSha256=a.identity(execution)['sha256'],originalRecordBytes=execution.stat().st_size)
    if stage=='model-android':
        terminal=dict(source=a.SOURCE,actualExitCode=0,sourceBindings=1,sdkExecuted=False,
                      owner29816Absent=True,sourceClean=True,**{k:v for k,v in summary.items() if k!='source'})
    else:
        terminal=dict(source=a.SOURCE,actualExitCode=0,sourceBindings=1,sdkExecuted=False,
                      state='SCOPED_SOURCE_RECOVERY_PASSED',ownerProcessAbsent=True)
    refs=[]
    for name,value in [('actual-terminal.json',terminal),('raw-source.json',raw_source),
                       ('actual-compiler-inputs.json',{'read':[a.identity(raw)]}),
                       ('actual-source-command-flags.json',{'source':a.SOURCE}),('new-object-inventory.json',{'objects':[]})]:
        path=d/name;path.write_text(json.dumps(value));refs.append(a.identity(path))
    if stage=='model-android':summary_ref=next(r for r in refs if Path(r['path']).name=='actual-terminal.json')
    else:
        path=d/'resource-summary.json';path.write_text(json.dumps(summary));summary_ref=a.identity(path);refs.append(summary_ref)
    refs.append(a.identity(execution));seal=d/'seal.json';seal.write_text(json.dumps(dict(files=refs)))
    item=dict(terminal=terminal,rawSource=raw_source,seal=a.identity(seal),fullIndex=dict(files=refs),originalRecords={})
    expected=dict(seal=item['seal']['sha256'],rawSha=raw_source['raw']['sha256'],rawBytes=raw_source['raw']['bytes'])
    return item,summary_ref,expected


class AndroidPackage(unittest.TestCase):
    def test_stream_runtime_identity_matches_original_model_algorithm(self):
        normal=Path('D:/Codex/worktrees/OrcaSlicer/task-20261124-v102-desktop-sdk-sync/src/orca_model_sdk/tools/strip_android_model.py')
        spec=importlib.util.spec_from_file_location('frozen_model_strip_test',normal)
        module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
        with tempfile.TemporaryDirectory() as d:
            path=elf_fixture(Path(d)/'tiny.so')
            self.assertEqual(elf.runtime_identity(path),module.runtime_identity(path))
            self.assertTrue(elf.source_marker(path,a.SOURCE));before=elf.runtime_identity(path)[0]
            with path.open('r+b') as f:f.seek(1024);f.write(b'changed')
            self.assertNotEqual(elf.runtime_identity(path)[0],before)

    def test_invalid_arch_alignment_truncation_alloc_mapping(self):
        with tempfile.TemporaryDirectory() as d:
            for at,value in [(18,struct.pack('<H',62)),(112,struct.pack('<Q',4096)),(8192+64+24,struct.pack('<Q',12000))]:
                path=elf_fixture(Path(d)/'bad.so')
                with path.open('r+b') as f:f.seek(at);f.write(value)
                with self.assertRaises(ValueError):elf.runtime_identity(path)
            path.write_bytes(b'ELF')
            with self.assertRaises(ValueError):elf.runtime_identity(path)

    def test_disabled_typed_lifecycle_and_exact_two_stages(self):
        plan=dict(toolSource='a'*40,sdkRepository='D:/sdk',toolFiles=[],output='D:/new',strip='strip',readelf='readelf',
                  runtimeInputs={n:dict(stage=s,source=dict(path='D:/'+n,bytes=1,sha256='a'*64)) for s,n in p.NAMES.items()})
        plan['runtimeInputs']['libtbb.so']=dict(stage='model-android',source=dict(path='D:/libtbb.so',bytes=1,sha256='a'*64))
        plan['lifecycle']=p.life(plan)
        self.assertFalse(plan['lifecycle']['enabled'])
        with self.assertRaises(ValueError):p.validate_life(plan)
        plan['lifecycle']=p.life(plan,True);p.validate_life(plan,dict(lifecycle=copy.deepcopy(plan['lifecycle'])))
        for changes in (dict(enabled=1),dict(effectiveCreationFlags=0x80400),dict(commandTimeoutSeconds=True),dict(custodian='foreign')):
            bad=copy.deepcopy(plan);bad['lifecycle'].update(changes)
            with self.assertRaises(ValueError):p.validate_life(bad)
        with self.assertRaisesRegex(ValueError,'exact two'):
            p.validate(dict(schema=p.PLAN,source=a.SOURCE,tree=a.TREE,producers={s:None for s in a.STAGES},resourceSummaries={}))

    def test_original_resources_and_streaming_execution(self):
        with tempfile.TemporaryDirectory() as d:
            items={};summaries={};expected={}
            for stage in p.STAGES:items[stage],summaries[stage],expected[stage]=producer(Path(d),stage)
            plan=dict(producers=items,resourceSummaries=summaries)
            with patch.dict(p.ORIGINS,expected):
                for stage in p.STAGES:
                    p.original_graph(plan,stage,verify_all=True)
                    bad=copy.deepcopy(plan);bad['producers'][stage]['rawSource']['raw']['sha256']='0'*64
                    with self.assertRaises(ValueError):p.original_graph(bad,stage)
                Path(items['model-android']['seal']['path']).write_text('{}')
                with self.assertRaises(ValueError):p.original_graph(plan,'model-android')

    def test_substituted_summary_and_changed_resource_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);item,summary,expected=producer(root,'slicing-android')
            plan=dict(producers={'slicing-android':item},resourceSummaries={'slicing-android':summary})
            with patch.dict(p.ORIGINS,{'slicing-android':expected}):
                outside=root/'execution.json';outside.write_text('unrelated')
                item['fullIndex']['files'].append(a.identity(outside))
                seal=Path(item['seal']['path']);seal.write_text(json.dumps(item['fullIndex']))
                item['seal']=a.identity(seal);p.ORIGINS['slicing-android']['seal']=item['seal']['sha256']
                p.original_graph(plan,'slicing-android',verify_all=True)
                bad=copy.deepcopy(plan);bad['resourceSummaries']['slicing-android']=dict(summary,path=str(root/'resource-summary.json'))
                with self.assertRaises(ValueError):p.original_graph(bad,'slicing-android')
                path=Path(summary['path']);value=json.loads(path.read_text());value['commands'][0]['exitCode']=72
                path.write_text(json.dumps(value))
                with self.assertRaises(ValueError):p.original_graph(plan,'slicing-android')

    def test_full_dynamic_public_and_needed_tables(self):
        symbols="Symbol table '.dynsym' contains 10 entries:\n"+'\n'.join('1: 001 1 FUNC GLOBAL DEFAULT 1 orca_model_'+n for n in a.PUBLIC['model'])
        needed='Dynamic section contains 1 entries:\n (NEEDED) Shared library: [libtbb.so]'
        def command(args,label):return needed if args[0]=='-d' else symbols
        self.assertEqual(p.dynamic_tables(command,'raw','x','model')['needed'],['libtbb.so'])
        with self.assertRaises(ValueError):p.dynamic_tables(lambda *x:'truncated','raw','x','model')
        with self.assertRaises(ValueError):
            p.dynamic_tables(lambda args,label:needed if args[0]=='-d' else symbols+'\n1: 001 1 FUNC GLOBAL DEFAULT 1 orca_bad','raw','x','model')

    def test_manifest_pending_phone_no_capability_fabrication(self):
        with tempfile.TemporaryDirectory() as d:
            out=Path(d)/('sdk-android-'+a.SOURCE[:12]+'-r1');out.mkdir();(out/'payload').write_bytes(b'tiny')
            plan=dict(output=str(out),toolSource='a'*40,producers={},resourceSummaries={},companions={})
            m=p.manifest(plan,[],dict(path='provenance/android-static-proof.json',sha256='a'*64,bytes=1))
            self.assertEqual(m['schemaVersion'],p.SCHEMA);self.assertEqual(m['platformScope'],['android-arm64-v8a'])
            self.assertEqual(m['runtimeCapabilities'],dict(status='pending-phone',evidence=None))
            self.assertIsNone(m['functionalQualification']);self.assertNotIn('capabilities',m);self.assertNotIn('windows',m)
            self.assertEqual(m['files'],[dict(path='payload',sizeBytes=4,sha256=hashlib.sha256(b'tiny').hexdigest())])

    def test_android_owner_guard_rejects_before_runtime(self):
        with tempfile.TemporaryDirectory() as d,patch.object(owner,'runtime',side_effect=AssertionError('runtime forbidden')) as runtime:
            args=(['tool'],Path(d)/'log',{},lambda:None,Path(d)/'cancel',lambda:None)
            with self.assertRaises(AssertionError):
                owner.run_owned(*args,permitted_commands=[['tool']],permitted_images=['tool'],android_plan={})
            runtime.assert_not_called()
            with patch.object(p,'owner_contract',side_effect=ValueError('invalid Android lifecycle')):
                with self.assertRaises(ValueError):
                    owner.run_owned(*args,permitted_commands=[['tool']],permitted_images=['tool'],android_plan={},lifecycle_profile=p.PROFILE)
            runtime.assert_not_called()

    def test_disabled_go_never_runs(self):
        for go in ({},dict(authorized=False),dict(authorized=True,artifactOnlyAuthorized=True,allowSDKLoad=True)):
            with patch('subprocess.Popen',side_effect=AssertionError('no process')):
                with self.assertRaises(ValueError):p.execute({},go,'bad')

    def test_fake_strip_pipeline_proves_needed_runtime_closure_and_preserved_model(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);rows={};raws=root/'raw';raws.mkdir()
            for stage,name in p.NAMES.items():
                path=elf_fixture(raws/name);rows[name]=dict(stage=stage,source=a.identity(path))
            path=elf_fixture(raws/'libtbb.so');rows[path.name]=dict(stage='model-android',source=a.identity(path))
            plan=dict(output=str(root/'output'),debug=str(root/'debug'),runtimeInputs=rows,strip='strip',readelf='readelf')
            calls=[]
            def command(argv,label):
                calls.append(argv)
                if argv[0]=='strip':
                    raw=Path(argv[2]);out=Path(argv[4]);out.write_bytes(raw.read_bytes())
                    if argv[1]=='--strip-debug':
                        # Remove only the nonallocated .debug_* section name; loader bytes stay equal.
                        with out.open('r+b') as f:f.seek(8192+5*64);f.write(struct.pack('<I',0))
                    return ''
                if argv[1]=='-d':
                    return 'Dynamic section contains entries:\n'+(' (NEEDED) Shared library: [libtbb.so]' if 'libtbb.so' not in argv[2] else '')
                name=Path(argv[-1]).name
                kind='model' if name==p.NAMES['model-android'] else ('slicing' if name==p.NAMES['slicing-android'] else None)
                prefix='orca_model_' if kind=='model' else 'orca_mobile_slicing_'
                return "Symbol table '.dynsym' contains entries:\n"+('' if kind is None else '\n'.join('1: 001 1 FUNC GLOBAL DEFAULT 1 '+prefix+n for n in a.PUBLIC[kind]))
            proof=p.process_libraries(plan,command,lambda:None)
            self.assertEqual(proof['libraries']['libtbb.so']['operation'],'copy-byte-exact')
            self.assertEqual(a.identity(root/'debug/liborca_model_sdk_c.unstripped.so')['sha256'],rows[p.NAMES['model-android']]['source']['sha256'])
            self.assertTrue(any(x[1]=='--strip-debug' for x in calls));self.assertTrue(any(x[1]=='--strip-unneeded' for x in calls))
            self.assertFalse(any(x[0]=='strip' and Path(x[2]).name=='libtbb.so' for x in calls))

    def test_exact_go_refuses_missing_closure_wrong_operation_and_stale_clock(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);file=root/'input';file.write_bytes(b'tiny');ref=a.identity(file)
            plan=dict(bindingPath=str(root/'plan.json'),runtimeInputs={},qualification={},evidence=str(root/'evidence'))
            Path(plan['bindingPath']).write_text(json.dumps(plan));sha=a.identity(plan['bindingPath'])['sha256']
            go=dict(schema='android-artifact-go-v1',authorized=True,rootConversation=p.ROOT,
                    artifactOnlyAuthorized=True,allowSDKLoad=False,operation='package-android',
                    planSha256=sha,planPath=plan['bindingPath'],closure=[ref],cancel=str(root/'evidence.cancel'))
            with patch.object(p,'fresh',lambda g:None),patch.object(p,'validate_life',lambda *x:None), \
                 patch.object(p,'validate',return_value=[ref]),patch.object(p,'tool_refs',return_value=[]), \
                 patch.object(p,'source_check',lambda x:None):
                p.exact_go(plan,go,sha)
                for changes in (dict(closure=[]),dict(closure=[ref,ref]),dict(operation='observe-inputs')):
                    with self.assertRaises(ValueError):p.exact_go(plan,dict(go,**changes),sha)
            expired=dict(issuedUtc='2026-01-01T00:00:00Z',expiresUtc='2026-01-01T00:06:00Z')
            with self.assertRaises(ValueError):p.fresh(expired)

    def test_observer_has_only_two_stage_cache_binding_requests(self):
        with self.assertRaisesRegex(ValueError,'exact Android observation'):
            p.observe_inputs(dict(observationRequests={},sourceAppendices={}),Path('never'),lambda:None)

    def test_runtime_names_stage_and_roots_fail_closed(self):
        with self.assertRaises(ValueError):p.runtime_inputs(dict(runtimeInputs={}))
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);plan=dict(output=str(root/'out'),debug=str(root/'out/debug'),evidence=str(root/'evidence'),
                                  sdkRepository=str(root/'sdk'),producers={})
            with self.assertRaisesRegex(ValueError,'nested'):p.check_roots(plan)

    def test_android_two_producer_observation_uses_actual_approved_basis_and_first_observed(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);items={};summaries={};expected={};appendices={};requests={}
            headers=root/'headers';headers.mkdir();(headers/'tiny.h').write_bytes(b'qualified header')
            runtime=root/'libtbb.so';runtime.write_bytes(b'approved runtime')
            rule=root/'approved_tbb.json'
            rule.write_text(json.dumps(dict(runtime=dict(library=runtime.name,sha256=a.identity(runtime)['sha256'],bytes=runtime.stat().st_size),
                                           headers={'tiny.h':a.identity(headers/'tiny.h')['sha256']},upstream={})))
            original=Path(p.__file__).resolve().parents[2]/'src/orca_model_sdk/repair_core/verify_approved_tbb.py'
            verifier=root/'verify_approved_tbb.py';verifier.write_bytes(original.read_bytes())
            spec=importlib.util.spec_from_file_location('tiny_android_approved',verifier);module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
            hp=root/'header-proof.json';hp.write_text(json.dumps(dict(headerRoot=str(headers),headers={'tiny.h':a.identity(headers/'tiny.h')['sha256']})))
            for stage in p.STAGES:
                item,summary,origin=producer(root,stage);build=root/stage
                (build/'CMakeCache.txt').write_text('ORCA_MODEL_SOURCE_COMMIT:STRING='+a.SOURCE+'\nCMAKE_CACHEFILE_DIR:INTERNAL='+str(build)+'\n')
                binding=build/'approved-tbb-binding.json';binding.write_text(json.dumps(module.validate(runtime,headers,rule)))
                graphpath=build/'actual-compiler-inputs.json'
                graph=json.loads(graphpath.read_text());graph['runtime']=[a.identity(runtime),a.identity(headers/'tiny.h')];graphpath.write_text(json.dumps(graph))
                item['fullIndex']['files']=[a.identity(graphpath) if Path(r['path'])==graphpath else r for r in item['fullIndex']['files']]
                Path(item['seal']['path']).write_text(json.dumps(item['fullIndex']));item['seal']=a.identity(item['seal']['path']);origin['seal']=item['seal']['sha256']
                normal=build/'normal-plan.json';normal.write_text(json.dumps(dict(stages={stage:dict(
                    configureArgv=['-B',str(build),'-DORCA_MODEL_SOURCE_COMMIT:STRING='+a.SOURCE],buildArgv=['cmake','--build',str(build)])})))
                extra=[('normal-plan',normal),('approved-rule',rule),('approved-verifier',verifier),('header-manifest',hp)]
                appendix=root/(stage+'-appendix.json');appendix.write_text(json.dumps(dict(schema='b5f-companion-source-appendix-v1',
                    origin=p.companion.origin(item,stage),evidenceTiming=p.companion.TIMING,
                    entries=[dict(role=role,pin=a.identity(path),firstObservedUtc='now') for role,path in extra])))
                basis=dict(normalPlan=a.identity(normal))
                requests[stage]=[dict(role='cache',stage=stage,producerBuildRoot=str(build),producerOutputPath=str(build/'CMakeCache.txt'),basis=basis),
                    dict(role='approved-binding',stage=stage,producerBuildRoot=str(build),producerOutputPath=str(binding),basis=dict(basis,
                        approvedRule=a.identity(rule),approvedVerifier=a.identity(verifier),binding=a.identity(binding),headerProof=a.identity(hp)))]
                items[stage]=item;summaries[stage]=summary;expected[stage]=origin;appendices[stage]=a.identity(appendix)
            plan=dict(producers=items,resourceSummaries=summaries,sourceAppendices=appendices,observationRequests=requests)
            evidence=root/'evidence';evidence.mkdir()
            with patch.dict(p.ORIGINS,expected),patch.object(p.companion,'APPROVED_RULE_SHA',a.identity(rule)['sha256']),patch.object(p,'source_check',lambda p:None):
                p.observe_inputs(plan,evidence,lambda *x:None)
                for stage in p.STAGES:
                    receipt=a.small_json(evidence/stage/'receipt.json')
                    self.assertEqual(receipt['evidenceTiming'],p.companion.TIMING)
                    self.assertEqual(receipt['origin']['stage'],stage)
                    self.assertEqual([x['role'] for x in receipt['companions']],['cache','approved-binding'])
                    p.companion.validate(receipt,items[stage],stage)

    def test_source_snapshot_license_headers_and_zip_exact_closure(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);repo=root/'sdk';repo.mkdir();out=root/'out';out.mkdir()
            names=['LICENSE.txt','mobile-slicing/tests/honor_multihead_thermal_test.cpp','deps_src/nlohmann/json.hpp',
                   'src/orca_model_sdk/include/orca/model/model_sdk_c.h','src/orca_mobile_slicing_sdk/include/orca/mobile/slicing_sdk_c.h']
            for name in names:
                path=repo/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(b'/*license*/ tiny-source')
            dep=root/'dependency';dep.mkdir();(dep/'NOTICE').write_bytes(b'upstream notice')
            manifest=root/'dependencies.json';manifest.write_text(json.dumps([dict(key='DEP_SOURCE',path=str(dep),files=[dict(path='NOTICE',sha256=a.identity(dep/'NOTICE')['sha256'])])]))
            cache=root/'CMakeCache.txt';cache.write_text('DEP_SOURCE:PATH='+str(dep)+'\n')
            receipts={}
            for stage in p.STAGES:
                folder=root/stage;folder.mkdir();receipt=folder/'receipt.json'
                receipt.write_text(json.dumps(dict(schema=p.companion.SCHEMA,origin=dict(stage=stage),
                    companions=[dict(role='cache',current=a.identity(cache))])))
                receipts[stage]=dict(receipt=a.identity(receipt))
            archive=root/'cgal.zip';archive.write_bytes(b'archive fixture')
            plan=dict(sdkRepository=str(repo),output=str(out),git='never-git',dependencyManifest=a.identity(manifest),
                      cgalArchive=a.identity(archive),companions=receipts)
            with patch.object(p.subprocess,'check_output',return_value='\0'.join(names).encode()+b'\0'),patch.object(p,'CGAL_SHA',a.identity(archive)['sha256']):
                sources=p.snapshot(plan,lambda:None)
            with zipfile.ZipFile(out/'source-snapshot.zip') as z:
                self.assertEqual(set(z.namelist()),{x['path'] for x in sources}|{'source-files.json','dependency-source-inputs.json'})
                self.assertEqual(json.loads(z.read('source-files.json')),sources)
                for ref in sources:self.assertEqual(hashlib.sha256(z.read(ref['path'])).hexdigest(),ref['sha256'])
            self.assertEqual((out/'include/orca/model/model_sdk_c.h').read_bytes(),(out/'include/orca/mobile/model_sdk_c.h').read_bytes())
            self.assertTrue((out/'licenses/dependencies/DEP_SOURCE/NOTICE').is_file())


class AndroidConcreteContract(unittest.TestCase):
    EVIDENCE=Path('D:/Codex/artifacts/task-20261125-b5f-sdk-package-tooling/evidence')
    _actual_cache=None

    @classmethod
    def actual_metadata(cls):
        if cls._actual_cache is None:
            plan=json.loads((cls.EVIDENCE/'android-concrete-binding-preparation-v2/observe-plan-DISABLED.json').read_text(encoding='utf8'))
            native=json.loads(Path('D:/Codex/artifacts/task-20261124-v102-desktop-sdk-sync/evidence/android-source-current-readonly-v1/android-current-source-map.json').read_text(encoding='utf8'))
            receipts={}
            for stage,item in plan['producers'].items():
                index=p.companion.index(item);refs=list(index)
                for filename in ('actual-compiler-inputs.json','new-object-inventory.json'):
                    ref=next(r for r in index if Path(r['path']).name==filename)
                    self_path=Path(ref['path']);assert self_path.stat().st_size<=4*1024*1024
                    refs+=list(p.pins(json.loads(self_path.read_text(encoding='utf8'))))
                refs += [item['rawSource']['raw'],dict(item['rawSource']['raw'],path=item['rawSource']['buildPath']),item['seal']]
                appendix=plan['sourceAppendices'][stage]
                data=json.loads(Path(appendix['path']).read_text(encoding='utf8'))
                refs += [appendix]+[row['pin'] for row in data['entries']]
                before=p.original_refs(refs)
                current={row['role']:row['current'] for row in native['stages'][stage]['companionCurrentMap']}
                entries=[dict(request,current=current[request['role']],firstObservedUtc='<FUTURE-FIRST-OBSERVED-UTC>',
                              evidenceTiming=p.companion.TIMING) for request in plan['observationRequests'][stage]]
                receipts[stage]=dict(schema=p.companion.SCHEMA,origin=p.companion.origin(item,stage),sourceAppendix=appendix,
                    evidenceTiming=p.companion.TIMING,driftBefore=before,driftAfter=before,companions=entries)
            cls._actual_cache=(plan,receipts)
        return cls._actual_cache

    def test_typed_json_bool_int_float_nested_order_and_no_mutation(self):
        original=dict(a=[True,1,1.0,None],b={'x':False});before=copy.deepcopy(original)
        self.assertTrue(p.typed_equal(original,dict(b={'x':False},a=[True,1,1.0,None])))
        for left,right in [(True,1),(False,0),(1,1.0),({'x':[True]},{'x':[1]}),([1,2],[2,1]),({'x':1},{'x':1,'y':None})]:
            self.assertFalse(p.typed_equal(left,right))
            self.assertFalse(p.typed_member(left,[right]))
        self.assertFalse(p.typed_contains({'row':{'proof':True}},{'proof':1}))
        self.assertTrue(p.typed_contains({'row':{'proof':True,'extra':'kept'}},{'proof':True}))
        self.assertEqual(p.typed_json(original),p.typed_json(before))
        for bad in (float('nan'),float('inf'),{1:'wrong key'},(1,2),object()):
            with self.assertRaises(ValueError):p.typed_json(bad)

    def test_actual_sealed_true_integer_and_other_typed_substitutions_rejected(self):
        plan,_=self.actual_metadata();original=p.typed_json(plan)
        changes=[lambda q:q['producers']['model-android']['fullIndex']['files'][-1].update(identityFromCompletedProducerReceipt=1),
                 lambda q:q['producers']['model-android']['fullIndex'].update(actualExitCode=0.0),
                 lambda q:q['producers']['model-android']['terminal'].update(sdkExecuted=0),
                 lambda q:q['producers']['model-android']['rawSource'].update(cleanBuild=1),
                 lambda q:q['producers']['model-android']['fullIndex']['files'].reverse(),
                 lambda q:q['producers']['model-android']['fullIndex'].update(extra='substitution')]
        real_verify=a.verify
        def metadata_only(ref):
            self.assertLessEqual(Path(ref['path']).stat().st_size,4*1024*1024)
            self.assertEqual(Path(ref['path']).suffix,'.json');return real_verify(ref)
        with patch.object(a,'verify',metadata_only):
            for change in changes:
                changed=copy.deepcopy(plan);change(changed)
                with self.assertRaises(ValueError):p.original_graph(changed,'model-android',verify_all=False)
        self.assertEqual(p.typed_json(plan),original)

    def test_actual_slicing_receipt_9154_full_records_fits_only_android_role_reader(self):
        plan,receipts=self.actual_metadata()
        self.assertEqual(len(receipts['slicing-android']['driftBefore']),9154)
        with tempfile.TemporaryDirectory() as d:
            for stage,receipt in receipts.items():
                folder=Path(d)/stage;folder.mkdir();file=folder/'receipt.json'
                before=p.typed_json(receipt)
                p.write_role(file,receipt,'receipt',stage,plan['producers'][stage])
                ref=a.identity(file)
                if stage=='slicing-android':
                    self.assertGreater(ref['bytes'],4*1024*1024)
                    self.assertLess(ref['bytes'],p.RECEIPT_LIMIT)
                    with self.assertRaises(ValueError):a.small_json(file)
                loaded=p.read_role(ref,'receipt',stage,plan['producers'][stage])
                self.assertEqual(p.typed_json(loaded),before)
                self.assertEqual(p.typed_json(receipt),before)
                for candidate in (dict(ref,sha256='0'*64),dict(ref,bytes=ref['bytes']+1)):
                    with self.assertRaises(ValueError):p.read_role(candidate,'receipt',stage,plan['producers'][stage])

    def test_role_caps_wrong_role_invalid_json_and_no_global_limit_change(self):
        self.assertEqual(p.RECEIPT_LIMIT,16*1024*1024);self.assertEqual(p.MANIFEST_LIMIT,32*1024*1024)
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);file=root/'receipt.json'
            value=dict(schema=p.companion.SCHEMA,origin=dict(stage='model-android'))
            p.write_role(file,value,'receipt','model-android')
            ref=a.identity(file)
            for role,stage in [('manifest',None),('receipt','slicing-android'),('unknown',None)]:
                with self.assertRaises(ValueError):p.read_role(ref,role,stage)
            for role,name,limit in [('receipt','receipt.json',p.RECEIPT_LIMIT),('manifest','manifest.json',p.MANIFEST_LIMIT)]:
                candidate=dict(path=str(root/name),bytes=limit+1,sha256='0'*64)
                with patch.object(Path,'open',side_effect=AssertionError('over-limit file opened')):
                    with self.assertRaises(ValueError):p.read_role(candidate,role,'model-android')
            for raw in (b'{broken',b'{"x":1,"x":2}',b'{"x":NaN}'):
                file.write_bytes(raw)
                with self.assertRaises(ValueError):p.read_role(a.identity(file),'receipt','model-android')
            other=root/'not-receipt.json';other.write_text(json.dumps(value))
            with self.assertRaises(ValueError):p.read_role(a.identity(other),'receipt','model-android')
            with patch.object(p,'RECEIPT_LIMIT',64),patch.object(Path,'open',side_effect=AssertionError('oversize write opened')):
                with self.assertRaises(ValueError):p.write_role(file,dict(value,padding='x'*100),'receipt','model-android')

    def test_actual_complete_source_name_manifest_budget_not_source_contents(self):
        plan,_=self.actual_metadata()
        names=json.loads((self.EVIDENCE/'android-concrete-contract-dev-v1/fixed-git-source-names.json').read_text(encoding='utf8'))['selectedTrackedNames']
        metadata=Path(plan['dependencyManifest']['path'])
        self.assertLessEqual(metadata.stat().st_size,16*1024*1024)
        deps=json.loads(metadata.read_text(encoding='utf8'))
        # Repository hashes are explicit length placeholders for this size-only fixture.
        sources=[dict(path=n,sha256='0'*64) for n in names]
        sources += [dict(path='dependencies/'+item['key']+'/'+row['path'],sha256=row['sha256']) for item in deps for row in item['files']]
        p.register_names([row['path'] for row in sources]+['source-files.json','dependency-source-inputs.json'])
        notices=['licenses/'+row['path'] for row in sources if
                 Path(row['path']).name.upper().split('.')[0] in {'LICENSE','COPYING','COPYRIGHT','NOTICE'}
                 or 'LICENSES' in [part.upper() for part in Path(row['path']).parts]]
        generated=['source-snapshot.zip','licenses/nlohmann-json-LICENSE.MIT','dependencies/CGAL-6.2.1.zip',
                   'include/orca/mobile/model_sdk_c.h','include/orca/model/model_sdk_c.h',
                   'include/orca/mobile/slicing_sdk_c.h','manifest.json']
        release_names=['android/arm64-v8a/'+name for name in plan['runtimeInputs']]
        p.register_names(notices+generated+release_names)
        fixture=dict(schemaVersion=p.SCHEMA,files=[],sourceFiles=sources,fixtureHashesOnly=True)
        with tempfile.TemporaryDirectory() as d:
            out=Path(d)/'payload';out.mkdir();file=out/'manifest.json'
            p.write_role(file,fixture,'manifest');ref=a.identity(file)
            self.assertGreater(ref['bytes'],12_319_817);self.assertLess(ref['bytes'],p.MANIFEST_LIMIT)
            with self.assertRaises(ValueError):a.small_json(file)
            parsed=p.read_role(ref,'manifest')
            self.assertEqual(len(parsed['sourceFiles']),len(sources))
            self.assertEqual(p.typed_json(parsed),p.typed_json(fixture))
            p.write_bundle(out,Path(d)/'fixture.zip')
            with self.assertRaises(ValueError):p.write_bundle(out,out/'recursive.zip')

    def test_actual_distribution_index_preserves_all_refs_without_large_target_io(self):
        plan,_=self.actual_metadata();fixture=copy.deepcopy(plan)
        fixture['companions']={stage:None for stage in p.STAGES}
        real_verify=a.verify
        def metadata_only(ref):
            self.assertEqual(Path(ref['path']).suffix,'.json')
            self.assertLessEqual(Path(ref['path']).stat().st_size,4*1024*1024)
            return real_verify(ref)
        with patch.object(a,'verify',metadata_only):index=p.provenance_index(fixture,plan['toolFiles'])
        self.assertLess(len(p.role_bytes(index,'evidence-index')),p.RECEIPT_LIMIT)
        rows={p.key(row['identity']['path']):row for row in index['entries']}
        for stage,item in fixture['producers'].items():
            for ref in p.companion.index(item)+[item['seal'],item['rawSource']['raw']]:
                self.assertTrue(p.typed_member(ref,rows[p.key(ref['path'])]['originalRefs']))
        for row in rows.values():
            if Path(row['identity']['path']).suffix.lower() in {'.so','.obj','.o','.exe','.log'} or Path(row['identity']['path']).name=='execution.json':
                self.assertEqual(row['distribution'],'external-only');self.assertIsNone(row['payloadPath'])
            if row['distribution']=='distributed-copy':
                self.assertTrue(row['roles']);self.assertLessEqual(row['identity']['bytes'],p.RECEIPT_LIMIT)
        self.assertTrue(any(Path(row['identity']['path']).name=='execution.json' and row['identity']['bytes']<p.RECEIPT_LIMIT
                            and row['distribution']=='external-only' for row in rows.values()))

    def test_distribution_copies_only_declared_small_metadata_and_post_verification_is_unfiltered(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);data=root/'source';data.mkdir();out=root/'output';out.mkdir()
            terminal=data/'actual-terminal.json';terminal.write_text('{"original":true}')
            execution=data/'execution.json';execution.write_bytes(b'execution stays external even tiny')
            raw=data/'liborca_model_sdk_c.so';raw.write_bytes(b'raw stays external')
            unknown=data/'unlisted.json';unknown.write_bytes(b'{}')
            annotated=dict(a.identity(raw),identityFromCompletedProducerReceipt=True)
            refs=[a.identity(terminal),a.identity(execution),annotated,a.identity(unknown)]
            seal=data/'seal.json';seal.write_text('{}');refs.append(a.identity(seal))
            item=dict(fullIndex=dict(files=refs),originalRecords={},seal=a.identity(seal),rawSource=dict(raw=a.identity(raw)))
            plan=dict(output=str(out),producers={'model-android':item},companions={})
            with patch.object(p,'original_graph',return_value=(item,{},refs+[a.identity(raw)])):
                index_ref=p.distribute_provenance(plan)
            index=p.read_role(dict(index_ref,path=str(out/index_ref['path'])),'evidence-index')
            rows={p.key(row['identity']['path']):row for row in index['entries']}
            self.assertEqual(len(rows[p.key(raw)]['originalRefs']),2)
            self.assertEqual(rows[p.key(unknown)]['distribution'],'external-only')
            self.assertTrue(terminal.exists() and execution.exists() and raw.exists())
            copied=[out/row['payloadPath'] for row in rows.values() if row['distribution']=='distributed-copy']
            self.assertTrue(copied)
            self.assertFalse(any(path.name.endswith(('execution.json','.so','unlisted.json')) for path in copied))
            calls=[]
            with patch.object(a,'verify',side_effect=lambda ref:calls.append(ref) or Path(ref['path'])):
                p.verify_closure(refs)
            self.assertIn(p.key(raw),{p.key(ref['path']) for ref in calls})
            self.assertIn(p.key(execution),{p.key(ref['path']) for ref in calls})
            # A declared metadata role still cannot bypass the distribution budget.
            oversize=dict(a.identity(seal),bytes=p.RECEIPT_LIMIT+1)
            oversized_refs=[ref for ref in refs if p.key(ref['path'])!=p.key(seal)]+[oversize]
            oversized_item=dict(item,seal=oversize,fullIndex=dict(files=oversized_refs))
            oversized_plan=dict(plan,producers={'model-android':oversized_item})
            with patch.object(p,'original_graph',return_value=(oversized_item,{},oversized_refs)):
                budget_index=p.provenance_index(oversized_plan)
            row=next(row for row in budget_index['entries'] if p.key(row['identity']['path'])==p.key(seal))
            self.assertEqual(row['roles'],['producer-seal'])
            self.assertEqual(row['distribution'],'external-only');self.assertIsNone(row['payloadPath'])
            self.assertIn('exceeds16MiB',row['reason'])

    def test_typed_actual_receipt_and_appendix_guards_precede_old_validator(self):
        plan,receipts=self.actual_metadata()
        original=p.typed_json(plan)
        # These calls inspect only JSON/tool-source metadata. They do not execute the original verifier.
        real_verify=a.verify
        def metadata_only(ref):
            path=Path(ref['path'])
            self.assertTrue(path.suffix=='.json' or path.name=='verify_approved_tbb.py')
            self.assertLessEqual(path.stat().st_size,4*1024*1024)
            return real_verify(ref)
        with patch.object(a,'verify',metadata_only),patch.object(p.companion,'validate',side_effect=AssertionError('no actual validator')):
            for stage in p.STAGES:p.typed_receipt_guard(plan,stage,receipts[stage])
            modified=json.loads(json.dumps(receipts['model-android']))
            for label in ('driftBefore','driftAfter'):
                annotated=next(r for r in modified[label] if r.get('identityFromCompletedProducerReceipt') is True)
                annotated['identityFromCompletedProducerReceipt']=1
            with self.assertRaisesRegex(ValueError,'typed.*coverage'):p.typed_receipt_guard(plan,'model-android',modified)
            modified=copy.deepcopy(receipts['model-android']);modified['origin']['terminal']['sdkExecuted']=0
            with self.assertRaisesRegex(ValueError,'typed receipt origin'):p.typed_receipt_guard(plan,'model-android',modified)
            modified=copy.deepcopy(receipts['model-android']);modified['companions'][0]['basis']['normalPlan']['bytes']=15988.0
            with self.assertRaisesRegex(ValueError,'typed observation basis'):p.typed_receipt_guard(plan,'model-android',modified)
            appendix=json.loads(Path(plan['sourceAppendices']['model-android']['path']).read_text(encoding='utf8'))
            appendix['origin']['terminal']['sdkExecuted']=0
            with patch.object(a,'small_json',return_value=appendix):
                with self.assertRaisesRegex(ValueError,'typed appendix origin'):
                    p.appendix_records(plan['sourceAppendices']['model-android'],plan['producers']['model-android'],'model-android')
        self.assertEqual(p.typed_json(plan),original)


class AndroidPinTracks(unittest.TestCase):
    def test_physical_projection_keeps_original_annotations_and_is_independent(self):
        for annotation in (dict(identityFromCompletedProducerReceipt=True),dict(hashEvidence='original recovery receipt')):
            bare=dict(path='D:/original/raw.so',bytes=123,sha256='a'*64)
            annotated=dict(bare,**annotation);refs=[annotated,bare,copy.deepcopy(annotated)]
            before=copy.deepcopy(refs)
            index=p.exact_refs(refs)
            self.assertEqual(list(index.values()),[dict(path=p.key(bare['path']),bytes=123,sha256='a'*64)])
            provenance=p.original_refs(refs)
            self.assertEqual(provenance,[annotated,bare])
            self.assertEqual(refs,before)
            next(iter(index.values()))['bytes']=456
            self.assertEqual(refs,before)

    def test_annotation_types_are_preserved_and_real_identity_conflicts_refused(self):
        bare=dict(path='D:/original/raw.so',bytes=123,sha256='a'*64)
        self.assertEqual(len(p.original_refs([dict(bare,observed=True),dict(bare,observed=1)])),2)
        for other in (dict(bare,bytes=124),dict(bare,sha256='b'*64)):
            with self.subTest(other=other),self.assertRaisesRegex(ValueError,'conflicting'):
                p.original_refs([dict(bare,proof='keep'),other])
        for other in (dict(bare,bytes=True),dict(bare,bytes=-1),dict(bare,sha256=None),dict(bare,path='relative')):
            with self.subTest(other=other),self.assertRaisesRegex(ValueError,'malformed'):
                p.exact_refs([other])

    def test_original_graph_preserves_both_shapes_and_rejects_replaced_original_metadata(self):
        with tempfile.TemporaryDirectory() as d:
            for stage,extra in [('model-android',dict(identityFromCompletedProducerReceipt=True)),
                                ('slicing-android',dict(hashEvidence='actual original raw'))]:
                item,summary,expected=producer(Path(d),stage)
                annotated=dict(item['rawSource']['raw'],**extra)
                item['fullIndex']['files'].append(annotated)
                Path(item['seal']['path']).write_text(json.dumps(item['fullIndex']))
                item['seal']=a.identity(item['seal']['path']);expected['seal']=item['seal']['sha256']
                plan=dict(producers={stage:item},resourceSummaries={stage:summary});before=copy.deepcopy(plan)
                with patch.dict(p.ORIGINS,{stage:expected}):
                    _,_,refs=p.original_graph(plan,stage,verify_all=False)
                    self.assertIn(annotated,refs);self.assertIn(item['rawSource']['raw'],refs)
                    self.assertEqual(plan,before)
                    for key in ('fullIndex','rawSource'):
                        changed=copy.deepcopy(plan)
                        if key=='fullIndex':changed['producers'][stage][key]['files'][-1].update(extra='substituted')
                        else:changed['producers'][stage][key]['raw'].update(extra='substituted')
                        with self.assertRaisesRegex(ValueError,'differs|substituted'):
                            p.original_graph(changed,stage,verify_all=False)

    def test_tiny_observer_preserves_annotated_and_bare_drift_and_strict_membership(self):
        original_producer=producer;real_validate=p.companion.validate;seen=[]
        def annotated_producer(root,stage):
            item,summary,expected=original_producer(root,stage)
            obj=root/stage/'tiny.o';obj.write_bytes(b'synthetic object')
            inventory=root/stage/'new-object-inventory.json'
            inventory.write_text(json.dumps(dict(objects=[a.identity(obj)])))
            item['fullIndex']['files']=[a.identity(inventory) if Path(r['path'])==inventory else r
                                       for r in item['fullIndex']['files']]
            extra=dict(identityFromCompletedProducerReceipt=True) if stage=='model-android' else dict(hashEvidence='original recovery')
            item['fullIndex']['files'].append(dict(item['rawSource']['raw'],**extra))
            Path(item['seal']['path']).write_text(json.dumps(item['fullIndex']))
            item['seal']=a.identity(item['seal']['path']);expected['seal']=item['seal']['sha256']
            return item,summary,expected
        def validate(receipt,item,stage):
            real_validate(receipt,item,stage)
            annotated=next(r for r in item['fullIndex']['files'] if Path(r['path']).name==p.NAMES[stage])
            bare=item['rawSource']['raw']
            self.assertNotEqual(annotated,bare)
            inventory=next(r for r in item['fullIndex']['files'] if Path(r['path']).name=='new-object-inventory.json')
            object_pin=next(p.pins(a.small_json(inventory['path'])))
            for variant in (annotated,bare,object_pin):
                self.assertIn(variant,receipt['driftBefore']);self.assertIn(variant,receipt['driftAfter'])
                bad=copy.deepcopy(receipt)
                missing=[r for r in bad['driftBefore'] if r!=variant]
                bad['driftBefore']=missing;bad['driftAfter']=copy.deepcopy(missing)
                with self.assertRaisesRegex(ValueError,'coverage'):real_validate(bad,item,stage)
            bad=copy.deepcopy(receipt);bad['origin']['stage']='foreign-stage'
            with self.assertRaisesRegex(ValueError,'origin'):real_validate(bad,item,stage)
            seen.append(stage)
        with patch(__name__+'.producer',annotated_producer),patch.object(p.companion,'validate',validate):
            AndroidPackage().test_android_two_producer_observation_uses_actual_approved_basis_and_first_observed()
        self.assertEqual(set(seen),set(p.STAGES))

    def test_actual_two_stage_small_json_boundary_without_any_target_io(self):
        evidence=Path('D:/Codex/artifacts/task-20261125-b5f-sdk-package-tooling/evidence')
        original=json.loads((evidence/'prepared-v1/binding.json').read_text(encoding='utf8'))
        field=json.loads((evidence/'se-real-binding-fieldmap.json').read_text(encoding='utf8'))
        plan=dict(producers={s:original['producers'][s] for s in p.STAGES},
                  resourceSummaries={s:field['stages'][s]['resourceSummary'] for s in p.STAGES})
        before=copy.deepcopy(plan);verified=[];real_verify=a.verify
        allowed={'key-seal.json','actual-recovery-seal.json','actual-terminal.json','raw-source.json',
                 'recovery-origin.json','actual-compiler-inputs.json','new-object-inventory.json'}
        def metadata_only(ref):
            path=Path(ref['path'])
            self.assertIn(path.name,allowed)
            self.assertLessEqual(path.stat().st_size,4*1024*1024)
            verified.append(str(path));return real_verify(ref)
        with patch.object(a,'verify',metadata_only),patch('subprocess.Popen',side_effect=AssertionError('no process')):
            for stage in p.STAGES:
                item,source,refs=p.original_graph(plan,stage,verify_all=False)
                raw=item['rawSource']['raw']
                indexed=item['fullIndex'].get('files',item['fullIndex'].get('refs'))
                annotated=next(r for r in indexed if p.key(r['path'])==p.key(raw['path']))
                self.assertNotEqual(annotated,raw)
                self.assertIn(annotated,refs);self.assertIn(raw,refs)
                for ref in indexed+list(item.get('originalRecords',{}).values())+list(p.pins(source)):
                    self.assertIn(ref,refs)
                inventory=next(r for r in indexed if Path(r['path']).name=='new-object-inventory.json')
                for ref in p.pins(a.small_json(metadata_only(inventory))):self.assertIn(ref,refs)
                self.assertEqual(set(p.exact_refs([annotated,raw])[p.key(raw['path'])]),{'path','bytes','sha256'})
        self.assertEqual(plan,before)
        self.assertTrue(verified)


class AndroidNamespace(unittest.TestCase):
    KEYS=['FETCHCONTENT_SOURCE_DIR_'+k for k in ('BOOST','CEREAL','EIGEN','LIBNOISE','NLOPT','PNG','TBB','ZLIB')]+['ORCA_MOBILE_EXPAT_SOURCE']

    def fixture(self,root,keys=('DEP_SOURCE',),refs=None,tracked_extra=()):
        repo=root/'sdk';repo.mkdir();out=root/'out';out.mkdir()
        names=['LICENSE.txt','mobile-slicing/tests/honor_multihead_thermal_test.cpp','deps_src/nlohmann/json.hpp',
               'src/orca_model_sdk/include/orca/model/model_sdk_c.h','src/orca_mobile_slicing_sdk/include/orca/mobile/slicing_sdk_c.h']
        for name in names:
            path=repo/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(b'/*license*/ tiny-source')
        dep=root/'dep';dep.mkdir();(dep/'NOTICE').write_bytes(b'notice')
        files=refs if refs is not None else [dict(path='NOTICE',sha256=a.identity(dep/'NOTICE')['sha256'])]
        manifest=root/'dependency.json';manifest.write_text(json.dumps([dict(key=k,path=str(dep),files=files) for k in keys]))
        cache=root/'cache';cache.write_text(''.join(k+':PATH='+str(dep)+'\n' for k in keys),encoding='utf8')
        receipts={}
        for stage in p.STAGES:
            folder=root/stage;folder.mkdir();receipt=folder/'receipt.json'
            receipt.write_text(json.dumps(dict(schema=p.companion.SCHEMA,origin=dict(stage=stage),
                companions=[dict(role='cache',current=a.identity(cache))])))
            receipts[stage]=dict(receipt=a.identity(receipt))
        cgal=root/'cgal';cgal.write_bytes(b'fixture archive')
        plan=dict(sdkRepository=str(repo),output=str(out),git='never',dependencyManifest=a.identity(manifest),
                  cgalArchive=a.identity(cgal),companions=receipts)
        return plan, ('\0'.join(names+list(tracked_extra))+'\0').encode()

    def assert_snapshot_rejected_without_writes(self,plan,tracked):
        with patch.object(p.subprocess,'check_output',return_value=tracked), \
             patch.object(p.zipfile,'ZipFile',side_effect=AssertionError('ZIP opened before namespace rejection')) as writer, \
             patch.object(a,'stream_copy',side_effect=AssertionError('copy before namespace rejection')) as copier:
            with self.assertRaises(ValueError):p.snapshot(plan,lambda:None)
            writer.assert_not_called();copier.assert_not_called()
        self.assertFalse(list(Path(plan['output']).rglob('*')))
        self.assertFalse((Path(plan['output'])/'source-snapshot.zip').exists())
        self.assertFalse((Path(plan['output'])/'candidate-result.json').exists())

    def test_original_review_dotdot_namespace_is_rejected_before_any_write(self):
        with tempfile.TemporaryDirectory() as d:
            plan,tracked=self.fixture(Path(d),keys=('../escaped',))
            self.assert_snapshot_rejected_without_writes(plan,tracked)

    def test_namespace_empty_path_drive_device_and_noncanonical_keys(self):
        for key in ('','.', '..','A/B','A\\B','C:/ABS','\\\\server\\share','lower','CON','PRN','AUX','NUL','COM1','LPT9','DEP_SOURCE.','DEP_SOURCE ','ÉXAMPLE'):
            with self.subTest(key=key),tempfile.TemporaryDirectory() as d:
                plan,tracked=self.fixture(Path(d),keys=(key,))
                self.assert_snapshot_rejected_without_writes(plan,tracked)

    def test_duplicate_namespace_and_source_names(self):
        for keys,refs,extra in [(('DEP_SOURCE','DEP_SOURCE'),None,()),
                (('DEP_SOURCE','dep_source'),None,()),
                (('DEP_SOURCE',),[dict(path='NOTICE',sha256=hashlib.sha256(b'notice').hexdigest())]*2,()),
                (('DEP_SOURCE',),None,('LICENSE.txt',))]:
            with self.subTest(keys=keys,extra=extra),tempfile.TemporaryDirectory() as d:
                plan,tracked=self.fixture(Path(d),keys,refs,extra)
                self.assert_snapshot_rejected_without_writes(plan,tracked)

    def test_dependency_ref_paths_rejected_before_any_write(self):
        for rel in ('../NOTICE','a/../NOTICE','a//NOTICE','/NOTICE','C:/NOTICE','a\\NOTICE',
                    'COM1.txt','x/NUL','notice.','notice ','name:stream','bad\x00name','x?y'):
            with self.subTest(rel=rel),tempfile.TemporaryDirectory() as d:
                plan,tracked=self.fixture(Path(d),refs=[dict(path=rel,sha256=hashlib.sha256(b'notice').hexdigest())])
                self.assert_snapshot_rejected_without_writes(plan,tracked)

    def test_final_names_case_unicode_file_directory_and_reserved_metadata(self):
        for names in [('a','A'),('src/café','src/cafe\u0301'),('a','a/b'),('A/B','a'),
                      ('source-files.json','SOURCE-FILES.JSON'),
                      ('dependency-source-inputs.json','dependency-source-inputs.json'),
                      ('licenses/NOTICE','LICENSES/notice'),('manifest.json','manifest.json')]:
            with self.subTest(names=names),self.assertRaises(ValueError):p.register_names(names)
        for name in ('CON.txt','AUX.md','a/LPT1.log','COM¹','a.','a ','a/../b','/a','D:/a'):
            with self.subTest(name=name),self.assertRaises(ValueError):p.register_names([name])

    def test_license_target_link_non_directory_and_prefix_are_refused(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);(root/'licenses').write_bytes(b'not a directory')
            with self.assertRaises(ValueError):p.safe_target(root,'licenses/NOTICE')
        with tempfile.TemporaryDirectory() as d,patch.object(Path,'is_symlink',lambda value:value.name=='licenses'):
            with self.assertRaises(ValueError):p.safe_target(Path(d),'licenses/NOTICE')
        with self.assertRaises(ValueError):p.register_names(['licenses/foo','licenses/FOO/bar'])
        for name in ('licenses/../escape','licenses/C:/escape','licenses/a/NUL'):
            with self.assertRaises(ValueError):p.safe_target(Path('unused'),name)

    def test_all_nine_canonical_namespaces_keep_source_notice_header_and_zip_hashes(self):
        with tempfile.TemporaryDirectory() as d:
            plan,tracked=self.fixture(Path(d),keys=self.KEYS)
            with patch.object(p.subprocess,'check_output',return_value=tracked),patch.object(p,'CGAL_SHA',plan['cgalArchive']['sha256']):
                sources=p.snapshot(plan,lambda:None)
            out=Path(plan['output'])
            with zipfile.ZipFile(out/'source-snapshot.zip') as z:
                self.assertEqual(set(z.namelist()),{r['path'] for r in sources}|{'source-files.json','dependency-source-inputs.json'})
                self.assertEqual(json.loads(z.read('source-files.json')),sources)
                for ref in sources:self.assertEqual(hashlib.sha256(z.read(ref['path'])).hexdigest(),ref['sha256'])
                for key in self.KEYS:self.assertIn('dependencies/'+key+'/NOTICE',z.namelist())
            for key in self.KEYS:self.assertEqual((out/'licenses/dependencies'/key/'NOTICE').read_bytes(),b'notice')
            self.assertEqual((out/'include/orca/model/model_sdk_c.h').read_bytes(),(out/'include/orca/mobile/model_sdk_c.h').read_bytes())
            self.assertTrue((out/'include/orca/mobile/slicing_sdk_c.h').is_file())

    def test_final_bundle_exact_manifest_set_and_bytes(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);out=root/'out';out.mkdir();(out/'payload').write_bytes(b'tiny')
            declared=[dict(path='payload',sizeBytes=4,sha256=hashlib.sha256(b'tiny').hexdigest())]
            (out/'manifest.json').write_text(json.dumps(dict(schemaVersion=p.SCHEMA,files=declared)))
            p.write_bundle(out,root/'bundle.zip')
            with zipfile.ZipFile(root/'bundle.zip') as z:self.assertEqual(set(z.namelist()),{'payload','manifest.json'})
            (out/'extra').write_bytes(b'extra')
            with self.assertRaises(ValueError):p.write_bundle(out,root/'extra.zip')
            self.assertFalse((root/'extra.zip').exists())
            (out/'extra').unlink();(out/'payload').write_bytes(b'changed')
            with self.assertRaises(ValueError):p.write_bundle(out,root/'changed.zip')
            self.assertFalse((root/'changed.zip').exists())

    def test_final_bundle_malicious_duplicate_and_metadata_names_precede_zip_open(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);out=root/'out';out.mkdir()
            for names in (('../escape',),('a','A'),('a','a/b'),('manifest.json',)):
                (out/'manifest.json').write_text(json.dumps(dict(schemaVersion=p.SCHEMA,files=[dict(path=n,sizeBytes=0,sha256='0'*64) for n in names])))
                with patch.object(p.zipfile,'ZipFile',side_effect=AssertionError('ZIP opened early')) as writer:
                    with self.assertRaises(ValueError):p.write_bundle(out,root/'bad.zip')
                    writer.assert_not_called()
                self.assertFalse((root/'bad.zip').exists())


if __name__=='__main__':unittest.main()
