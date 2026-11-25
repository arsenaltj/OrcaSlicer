import copy
import json
from pathlib import Path
import tempfile
import unittest
import types
from unittest.mock import patch
import adapter as a
import contracts


class Fix1(unittest.TestCase):
    def fixture(self, root, bad_resource=None):
        def put(path, value):
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(json.dumps(value), encoding='utf8')
            return a.identity(path)
        producers, inputs = {}, []
        for stage in a.STAGES:
            p = root / stage
            cache = put(p / 'CMakeCache.txt', {'cache': stage})
            filename = ('orca_model_sdk_c.dll' if stage == 'model-windows' else
                        'orca_mobile_slicing_sdk_c.dll' if stage == 'slicing-windows' else
                        'liborca_model_sdk_c.so' if stage == 'model-android' else 'liborca_mobile_slicing_sdk_c.so')
            raw = put(p / filename, {'raw': stage})
            rs = dict(source=a.SOURCE, raw=raw, buildPath=raw['path'], cleanBuild=True,
                      copiedPredecessorObjects=0, priorObjectsReused=False)
            terminal = dict(source=a.SOURCE, actualExitCode=0, sourceBindings=1, sdkExecuted=False)
            if stage == 'model-windows':
                terminal.update(fullSourceProvenancePassed=True, ownerPid16320Absent=True)
            elif stage == 'slicing-windows':
                terminal.update(buildAndSourcePassed=True, resourceReturned=True)
            elif stage == 'slicing-android':
                terminal.update(state='SCOPED_SOURCE_RECOVERY_PASSED', ownerProcessAbsent=True)
            else:
                terminal.update(records=[dict(name=n, exitCode=0, state='returned', monitorErrors=0, runtimeViolations=0)
                    for n in ('configure','actual-ninja-commands','build','actual-ninja-deps')], owner29816Absent=True, sourceClean=True)
            terminal_pin = put(p / 'actual-terminal.json', terminal)
            execution = put(p / 'execution.json', {'large-record-replaced-by-tiny-data': True})
            if stage == 'model-windows':
                resource = dict(source=a.SOURCE, buildState='returned', ownerPidsRemaining=0,
                    buildExitCode=0, configureExitCode=0, monitorErrors=0, runtimeViolations=0,
                    executionSha256=execution['sha256'])
            else:
                names = ['configure', 'build'] if stage.endswith('windows') else ['configure','actual-ninja-commands','build','actual-ninja-deps']
                rows = [dict(name=n, exitCode=0, state='returned', monitorErrors=[], runtimeViolations=[], cancelReason=None) for n in names]
                resource = dict(source=a.SOURCE, records=rows, commands=rows,
                    originalRecordSha256=execution['sha256'], originalRecordBytes=execution['bytes'])
            if stage == 'model-windows' and bad_resource == 'failed': resource['buildExitCode']=72
            if stage == 'model-windows' and bad_resource == 'wrong-record': resource['executionSha256']='0'*64
            if stage == 'model-windows' and bad_resource == 'unreturned': resource['ownerPidsRemaining']=1
            resource_pin = put(p / 'resource-summary.json', resource)
            raw_pin = put(p / 'raw-source.json', rs)
            layout = {stage + '/CMakeCache.txt': cache,
                      stage + ('/Release/' if stage.endswith('windows') else '/') + filename: raw}
            extra = {'model-windows':['Release/orca_model_runner.exe','Release/orca_model_sdk_c.lib','raw-tbb/tbb12.dll','Release/tbb12.dll'],
                     'slicing-windows':['Release/orca_mobile_slicing_json_gate.exe','Release/orca_mobile_slicing_sdk_c.lib','raw-tbb/tbb12.dll','Release/tbb12.dll','Release/tbbmalloc.dll','Release/Clipper2.dll'],
                     'model-android':['approved-tbb-binding.json'], 'slicing-android':['libtbb.so']}[stage]
            for name in extra:
                layout[stage + '/' + name] = put(p / name, {'input': name})
            inp = put(p / 'actual-compiler-inputs.json', list(layout.values()))
            refs = [terminal_pin, raw_pin, execution, resource_pin, inp,
                    put(p / 'actual-source-command-flags.json', {}), put(p / 'new-object-inventory.json', {})]
            full = dict(files=refs)
            seal = put(p / 'seal.json', full)
            common = dict(source=a.SOURCE, stage=stage, seal=seal, rawSource=rs, cache=cache, execution=execution)
            resource_proof = put(p / 'resource-proof.json', dict(common, schema='sdk-producer-resource-v1', summary=resource_pin))
            source_proof = put(p / 'source-proof.json', dict(common, schema='sdk-producer-source-v1', terminal=terminal_pin,
                canonicalInputs=layout, runtimeFiles={k:v for k,v in layout.items() if k.endswith(('.dll','.so'))}))
            producers[stage] = dict(terminal=terminal, rawSource=rs, seal=seal, fullIndex=full, originalRecords={}, resourceProof=[resource_proof],sourceProof=[source_proof])
            inputs.extend(dict(destination=k, source=v) for k,v in layout.items())
        plan = dict(producers=producers, stageInputs=inputs, sdkRepository=str(root / 'repo'),
                    toolSource='a'*40, ownerDirectory=str(root / 'owner'))
        files = [Path(a.__file__), Path(contracts.__file__), Path(a.__file__).with_name('static_proof.py')]
        for name in ('src/orca_model_sdk/tools/package_sdk.py','src/orca_model_sdk/tools/strip_android_model.py','src/orca_model_sdk/repair_core/verify_approved_tbb.py','src/orca_model_sdk/repair_core/approved_tbb.json','mobile-slicing/build_android_arm64.cjs'):
            put(root / 'repo' / name, {})
            files.append(root / 'repo' / name)
        for name in ('build_resources.py','build_owner.py','qa_owner_safety.py'):
            put(root / 'owner' / name, {})
            files.append(root / 'owner' / name)
        for key in ('python','node','readobj','readelf','strip','cgalArchive','dependencyManifest'):
            path = root / key
            put(path, [dict(path=str(root / 'dependency'), files=[{'sha256':'pinned'}])] if key=='dependencyManifest' else {})
            files.append(path); plan[key]=str(path)
        put(root / 'tbb/LICENSE.txt', {})
        plan.update(tbbSource=str(root / 'tbb'), toolFiles=[a.identity(x) for x in files],
                    postprocessProof=a.identity(Path(a.__file__).with_name('static_proof.py')))
        return plan

    def test_complete_tiny_contract_and_review_negatives(self):
        with tempfile.TemporaryDirectory() as d, patch.object(contracts.subprocess, 'check_output', side_effect=lambda argv, **k: 'a'*40+'\n' if argv[-1]=='HEAD' else ''):
            plan = self.fixture(Path(d))
            contracts.check(plan, a)
            for mutate in (lambda p:p['producers']['model-windows'].update(fullIndex={'files':[]}),
                           lambda p:p.update(toolFiles=[]), lambda p:p.update(postprocessProof={}),
                           lambda p:p['stageInputs'].pop(), lambda p:p['producers']['model-android'].update(resourceProof=[{}])):
                bad=copy.deepcopy(plan); mutate(bad)
                with self.assertRaises((ValueError, KeyError)):
                    contracts.check(bad, a)
            stage=plan['producers']['model-windows']
            proof=a.small_json(stage['resourceProof'][0]['path'])
            resource=Path(proof['summary']['path'])
            resource.write_text(json.dumps(dict(state='failed', actualExitCode=72, runtimeViolations=['low_memory'], resourceReturned=False)))
            with self.assertRaises(ValueError): contracts.check(plan, a)

    def test_wrong_schema_not_windows(self):
        terminal=dict(source=a.SOURCE,actualExitCode=0,sourceBindings=1,sdkExecuted=False,records=[])
        raw=dict(source=a.SOURCE,cleanBuild=True,copiedPredecessorObjects=0,priorObjectsReused=False)
        with self.assertRaises(ValueError): a.terminal_ok('model-windows',terminal,raw)

    def test_valid_hash_failed_unknown_record_and_unreturned_semantics(self):
        for failure in ('failed','wrong-record','unreturned'):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as d:
                plan=self.fixture(Path(d), failure)
                with self.assertRaises(ValueError): contracts.check(plan,a)

    def test_go_closure_rejects_missing_input_before_effect(self):
        with tempfile.TemporaryDirectory() as d:
            plan=self.fixture(Path(d))
            with self.assertRaises(ValueError): contracts.go_closure(plan, {'closure':[]}, a)

    def test_stream_copy_and_zip_cancel(self):
        import zipfile
        with tempfile.TemporaryDirectory() as d:
            root=Path(d); src=root/'input';src.write_bytes(b'tiny')
            with patch.object(a,'IO_MONITOR',side_effect=ValueError('cancel')):
                with self.assertRaises(ValueError): a.stream_copy(src,root/'partial')
                with zipfile.ZipFile(root/'partial.zip','w') as z:
                    with self.assertRaises(ValueError): a.zip_copy(z,src,'input')

    def test_tail_and_save_failure_never_publish(self):
        with tempfile.TemporaryDirectory() as d:
            for reason in ('sample','save'):
                path=Path(d)/reason;path.mkdir()
                def fail(): raise OSError(reason)
                with self.assertRaises(OSError):
                    a.publish_result(path, {'candidate':'tiny'}, fail if reason=='sample' else lambda:None,
                                     fail if reason=='save' else lambda:None)
                self.assertFalse((path/'candidate-result.json').exists())
            path=Path(d)/'ok';path.mkdir()
            a.publish_result(path, {'candidate':'tiny'}, lambda:None,lambda:None)
            self.assertTrue((path/'candidate-result.json').exists())

    def test_actual_execute_tail_failure_no_complete_receipt(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);plan=self.fixture(root)
            plan.update(source=a.SOURCE, staging=str(root/'stage'), output=str(root/'output'),
                        evidence=str(root/'evidence'), debug=str(root/'debug'), ndk=str(root/'ndk'))
            planpath=root/'plan.json';planpath.write_text('{}')
            digest=a.identity(planpath)['sha256']
            go=dict(source=a.SOURCE,planSha256=digest,planPath=str(planpath),artifactOnlyAuthorized=True,
                    allowModelCapabilitiesLoad=True,closure=[],cancel=str(root/'cancel'))
            resources=types.ModuleType('build_resources')
            resources.gate=lambda *x:None
            resources.sample=lambda:dict(freeKiB=1, processes=[])
            resources.heavy=lambda p:False
            owner=types.ModuleType('build_owner')
            owner.admit_cancel=lambda p:None
            def run(argv,path,record,*args,**kwargs):
                Path(path).write_text('');record.update(exitCode=0)
            owner.run_owned=run
            def git(argv,**k):return (a.TREE if argv[-1]=='HEAD^{tree}' else a.SOURCE if argv[-1]=='HEAD' else '')+'\n'
            with patch.dict(a.sys.modules, {'build_resources':resources,'build_owner':owner}), \
                 patch.object(a,'validate_binding',lambda p:None),patch.object(contracts,'go_closure',lambda *x:None), \
                 patch.object(a,'verify_indexes',lambda p:None),patch.object(a,'copy_inputs',lambda *x:None), \
                 patch.object(a,'select_tbb',lambda *x:None),patch.object(a,'finish_manifest',lambda p:{'tiny':'candidate'}), \
                 patch.object(a.subprocess,'check_output',git):
                with self.assertRaisesRegex(ValueError,'IO resource'):
                    a.execute(plan,go,digest)
            evidence=Path(plan['evidence'])
            self.assertFalse((evidence/'candidate-result.json').exists())
            self.assertEqual(a.small_json(evidence/'execution.json')['state'],'failed_partial_no_retry')

if __name__=='__main__': unittest.main()
