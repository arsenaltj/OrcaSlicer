"""Tiny Android fixtures; no SDK, native tools, device or real raw reads."""
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
                  runtimeInputs={n:dict(stage=s,source=dict(path='D:/'+n)) for s,n in p.NAMES.items()})
        plan['runtimeInputs']['libtbb.so']=dict(stage='model-android',source=dict(path='D:/libtbb.so'))
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
            receipt=root/'receipt.json';receipt.write_text(json.dumps(dict(companions=[dict(role='cache',current=a.identity(cache))])))
            archive=root/'cgal.zip';archive.write_bytes(b'archive fixture')
            plan=dict(sdkRepository=str(repo),output=str(out),git='never-git',dependencyManifest=a.identity(manifest),
                      cgalArchive=a.identity(archive),companions={s:dict(receipt=a.identity(receipt)) for s in p.STAGES})
            with patch.object(p.subprocess,'check_output',return_value='\0'.join(names).encode()+b'\0'),patch.object(p,'CGAL_SHA',a.identity(archive)['sha256']):
                sources=p.snapshot(plan,lambda:None)
            with zipfile.ZipFile(out/'source-snapshot.zip') as z:
                self.assertEqual(set(z.namelist()),{x['path'] for x in sources}|{'source-files.json','dependency-source-inputs.json'})
                self.assertEqual(json.loads(z.read('source-files.json')),sources)
                for ref in sources:self.assertEqual(hashlib.sha256(z.read(ref['path'])).hexdigest(),ref['sha256'])
            self.assertEqual((out/'include/orca/model/model_sdk_c.h').read_bytes(),(out/'include/orca/mobile/model_sdk_c.h').read_bytes())
            self.assertTrue((out/'licenses/dependencies/DEP_SOURCE/NOTICE').is_file())


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
        receipt=root/'receipt';receipt.write_text(json.dumps(dict(companions=[dict(role='cache',current=a.identity(cache))])))
        cgal=root/'cgal';cgal.write_bytes(b'fixture archive')
        plan=dict(sdkRepository=str(repo),output=str(out),git='never',dependencyManifest=a.identity(manifest),
                  cgalArchive=a.identity(cgal),companions={s:dict(receipt=a.identity(receipt)) for s in p.STAGES})
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
            (out/'manifest.json').write_text(json.dumps(dict(files=declared)))
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
                (out/'manifest.json').write_text(json.dumps(dict(files=[dict(path=n,sizeBytes=0,sha256='0'*64) for n in names])))
                with patch.object(p.zipfile,'ZipFile',side_effect=AssertionError('ZIP opened early')) as writer:
                    with self.assertRaises(ValueError):p.write_bundle(out,root/'bad.zip')
                    writer.assert_not_called()
                self.assertFalse((root/'bad.zip').exists())


if __name__=='__main__':unittest.main()
