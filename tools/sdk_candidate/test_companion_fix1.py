import copy
import json
from pathlib import Path
import tempfile
import unittest
import importlib.util
from unittest.mock import patch
import adapter as a
import companion as c
import test_companion


class CompanionFix1(unittest.TestCase):
    def fixture(self,root):
        item,r,e=test_companion.CompanionTests().fixture(root)
        build=Path(e['producerBuildRoot']);release=build/'Release';release.mkdir()
        refs=[]
        def pin(path,text):
            path.parent.mkdir(parents=True,exist_ok=True);path.write_text(text);ref=a.identity(path);refs.append(ref);return ref
        def extra():return refs
        return item,r,e,build,pin,extra

    def test_real_release_property_shape_runner_lib_and_win_runtimes(self):
        for stage in ('model-windows','slicing-windows'):
            with self.subTest(stage=stage),tempfile.TemporaryDirectory() as d:
                item,r,e,build,pin,extra=self.fixture(Path(d))
                if stage=='slicing-windows':
                    item['rawSource']['buildPath']=str(build/'Release/orca_mobile_slicing_sdk_c.dll')
                    e['stage']=stage
                    plan=Path(e['basis']['normalPlan']['path'])
                    data=a.small_json(plan);data['stages'][stage]=data['stages'].pop('model-windows')
                    data['stages'][stage]['buildArgv']=['cmake','--build',str(build),'--target','orca_mobile_slicing_sdk','orca_mobile_slicing_json_gate']
                    plan.write_text(json.dumps(data));e['basis']['normalPlan']=a.identity(plan);extra().append(e['basis']['normalPlan'])
                sdk='orca_model_sdk' if stage=='model-windows' else 'orca_mobile_slicing_sdk'
                runner='orca_model_runner' if stage=='model-windows' else 'orca_mobile_slicing_json_gate'
                exe=build/'Release'/(runner+'.exe');exe.write_bytes(b'MZ'+a.SOURCE.encode())
                project=pin(build/(runner+'.vcxproj'),'''<Project><PropertyGroup>
<OutDir Condition="'$(Configuration)|$(Platform)'=='Debug|x64'">BAD/</OutDir>
<OutDir Condition="'$(Configuration)|$(Platform)'=='Release|x64'">$(ProjectDir)$(Configuration)/</OutDir>
<TargetName Condition="'$(Configuration)|$(Platform)'=='Release|x64'">'''+runner+'''</TargetName>
<TargetExt Condition="'$(Configuration)|$(Platform)'=='Release|x64'">.exe</TargetExt></PropertyGroup></Project>''')
                log=pin(build/(runner+'.write.1.tlog'),str(exe))
                e.update(role='runner',current=a.identity(exe));e['basis'].update(target=runner,project=project,linkTlog=log)
                c.basis(e,item,stage,[],extra())
                lib=build/'Release'/('orca_model_sdk_c.lib' if stage=='model-windows' else 'orca_mobile_slicing_sdk_c.lib');lib.write_bytes(b'!<arch>\n')
                sdkproject=pin(build/(sdk+'.vcxproj'),'<Project><ItemDefinitionGroup Condition="\'$(Configuration)|$(Platform)\'==\'Release|x64\'"><Link><ImportLibrary>'+str(lib)+'</ImportLibrary><AdditionalDependencies>tbb12.lib</AdditionalDependencies></Link></ItemDefinitionGroup></Project>')
                e.update(role='import-lib',current=a.identity(lib));e['basis'].update(target=sdk,project=sdkproject,linkTlog=pin(build/'sdk.tlog',str(lib)))
                c.basis(e,item,stage,[],extra())
                runtime=build/'runtime/Release/tbb12.dll';runtime.parent.mkdir(parents=True);runtime.write_bytes(b'MZtiny')
                dependency=pin(build/'tbb.vcxproj','<Project><PropertyGroup Condition="\'$(Configuration)|$(Platform)\'==\'Release|x64\'"><OutDir>'+str(runtime.parent)+'/</OutDir><TargetName>tbb12</TargetName><TargetExt>.dll</TargetExt></PropertyGroup></Project>')
                e.update(role='runtime',current=a.identity(runtime));e['basis'].update(project=sdkproject,dependencyProject=dependency,
                    dependencyTarget='tbb',dependencyImportLib='tbb12.lib',linkTlog=pin(build/'tbb.tlog',str(runtime)+'\n'+str(runtime.with_suffix('.lib'))),
                    sdkLinkTlog=pin(build/'sdk-runtime.tlog',item['rawSource']['buildPath']+'\n'+str(runtime.with_suffix('.lib'))))
                c.basis(e,item,stage,[],extra())

    def test_fixed_android_roles_headers_and_validator_reject_substitution(self):
        with tempfile.TemporaryDirectory() as d:
            item,r,e,build,pin,extra=self.fixture(Path(d));stage='model-android'
            item['rawSource']['buildPath']=str(build/'liborca_model_sdk_c.so');e['stage']=stage
            plan=Path(e['basis']['normalPlan']['path']);data=a.small_json(plan);data['stages'][stage]=data['stages'].pop('model-windows');plan.write_text(json.dumps(data))
            e['basis']['normalPlan']=a.identity(plan);extra().append(e['basis']['normalPlan'])
            headers=build/'headers';headers.mkdir();header=pin(headers/'tiny.h','header')
            lib=build/'libtbb.so';lib.write_bytes(b'tiny-runtime')
            rule=pin(build/'approved_tbb.json',json.dumps(dict(runtime=dict(library='libtbb.so',sha256=a.identity(lib)['sha256'],bytes=lib.stat().st_size),headers={'tiny.h':header['sha256']},upstream={})))
            original=Path(c.__file__).resolve().parents[2]/'src/orca_model_sdk/repair_core/verify_approved_tbb.py'
            verifier=pin(build/'verify_approved_tbb.py',original.read_text(encoding='utf8'))
            spec=importlib.util.spec_from_file_location('tiny_real_approved_verifier',original)
            module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
            binding=pin(build/'approved-tbb-binding.json',json.dumps(module.validate(lib,headers,Path(rule['path']))))
            hp=pin(build/'header-proof.json',json.dumps(dict(headerRoot=str(headers),headers={'tiny.h':header['sha256']})))
            e.update(role='runtime',current=a.identity(lib));e['basis'].update(approvedRule=rule,approvedVerifier=verifier,binding=binding,headerProof=hp)
            graph=[a.identity(lib),header]
            # Tiny stand-in roles are only permitted under explicit test patch;
            # production uses literal frozen source hashes, never patchable input.
            self.assertEqual(verifier['sha256'],c.APPROVED_VERIFIER_SHA)
            with patch.object(c,'APPROVED_RULE_SHA',rule['sha256']):
                c.basis(e,item,stage,graph,extra())
                e.update(role='approved-binding',current=binding);c.basis(e,item,stage,graph,extra())
                bad=copy.deepcopy(e);bad['basis']['approvedVerifier']=pin(build/'unrelated.py','def validate(*a): return {}')
                with self.assertRaises(ValueError):c.basis(bad,item,stage,graph,extra())
                Path(hp['path']).write_text(json.dumps(dict(headerRoot=str(build),headers={'tiny.h':header['sha256']})))
                e['basis']['headerProof']=a.identity(hp['path']);extra().append(e['basis']['headerProof'])
                with self.assertRaises(ValueError):c.basis(e,item,stage,graph,extra())
            with self.assertRaises(ValueError):c.basis(e,item,stage,graph,extra())

    def test_whole_root_ancestor_descendant_overlap_all_stages(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d)
            producers={s:dict(rawSource=dict(buildPath=str(root/s/('Release/sdk.dll' if s.endswith('windows') else 'sdk.so')))) for s in a.STAGES}
            source=root/'source'
            c.separate_output(root/'independent',producers,source)
            for s,p in producers.items():
                build=c.build_root(p,s)
                for out in (build,build/'fresh',build.parent):
                    with self.assertRaises(ValueError):c.separate_output(out,producers,source)
            for out in (source,source/'fresh',root):
                with self.assertRaises(ValueError):c.separate_output(out,producers,source)

    def test_appendix_does_not_change_old_seal_and_rejects_wrong_origin(self):
        with tempfile.TemporaryDirectory() as d:
            item,r,e,build,pin,extra=self.fixture(Path(d));old=Path(item['seal']['path']).read_bytes()
            project=pin(build/'future.vcxproj','<Project/>')
            data=dict(schema='b5f-companion-source-appendix-v1',origin=c.origin(item,'model-windows'),evidenceTiming=c.TIMING,
                entries=[dict(role='project',pin=project,firstObservedUtc='now')])
            ref=pin(Path(d)/'appendix.json',json.dumps(data));r['sourceAppendix']=ref
            self.assertEqual(c.appendix(r,item,'model-windows'),[project])
            self.assertEqual(Path(item['seal']['path']).read_bytes(),old)
            with self.assertRaises(ValueError):c.appendix(r,item,'model-android')

if __name__=='__main__':unittest.main()
