import copy
import json
from pathlib import Path
import tempfile
import unittest
import adapter as a
import companion as c
import test_companion_fix1


class CanonicalFix2(unittest.TestCase):
    def fixture(self,root,stage):
        item,r,e,build,pin,extra=test_companion_fix1.CompanionFix1().fixture(root)
        sdk='orca_model_sdk' if stage=='model-windows' else 'orca_mobile_slicing_sdk'
        if stage=='slicing-windows':
            item['rawSource']['buildPath']=str(build/'Release/orca_mobile_slicing_sdk_c.dll')
            normal=pin(build/'normal-plan.json',json.dumps(dict(stages={stage:dict(
                configureArgv=['-B',str(build),'-DORCA_MODEL_SOURCE_COMMIT='+a.SOURCE],
                buildArgv=['cmake','--build',str(build),'--target',sdk])})))
        else:normal=e['basis']['normalPlan']
        runtime=build/'Release/tbb12.dll';runtime.write_bytes(b'MZtiny')
        lib=runtime.with_suffix('.lib')
        dependency=pin(build/'tbb.vcxproj','<Project><PropertyGroup Condition="\'$(Configuration)|$(Platform)\'==\'Release|x64\'"><OutDir>'+str(runtime.parent)+'/</OutDir><TargetName>tbb12</TargetName><TargetExt>.dll</TargetExt></PropertyGroup><ItemDefinitionGroup Condition="\'$(Configuration)|$(Platform)\'==\'Release|x64\'"><Link><ImportLibrary>'+str(lib)+'</ImportLibrary></Link></ItemDefinitionGroup></Project>')
        sdktext='<Project><ItemDefinitionGroup Condition="\'$(Configuration)|$(Platform)\'==\'Release|x64\'"><Link><AdditionalDependencies>tbb12.lib;other.lib;%(AdditionalDependencies)</AdditionalDependencies></Link></ItemDefinitionGroup></Project>'
        sdkproject=pin(build/(sdk+'.vcxproj'),sdktext)
        dep_log=pin(build/'tbb.write.1.tlog',str(runtime)+'\n'+str(lib))
        sdk_log=pin(build/'sdk.read.1.tlog','^'+item['rawSource']['buildPath']+'\n'+str(lib))
        e.update(role='runtime',stage=stage,current=a.identity(runtime))
        e['basis']=dict(normalPlan=normal,dependencyProject=dependency,dependencyTarget='tbb',dependencyImportLib='tbb12.lib',
                        project=sdkproject,linkTlog=dep_log,sdkLinkTlog=sdk_log)
        return item,r,e,build,pin,extra,sdktext

    def accepted(self,item,r,e,build,pins):
        roles={'normal-plan.json':'normal-plan','tbb.vcxproj':'dependency-project',
               'tbb.write.1.tlog':'link-tlog','sdk.read.1.tlog':'link-tlog'}
        members=[]
        for x in pins:
            if x in c.index(item):continue
            name=Path(x['path']).name
            members.append(dict(role=roles.get(name,'project'),pin=x,firstObservedUtc='now'))
        ap=build/'appendix.json';ap.write_text(json.dumps(dict(schema='b5f-companion-source-appendix-v1',
            origin=c.origin(item,e['stage']),evidenceTiming=c.TIMING,entries=members)))
        r.update(sourceAppendix=a.identity(ap),companions=[e],origin=c.origin(item,e['stage']))
        r['driftBefore'] += [a.identity(ap)]+[x['pin'] for x in members]
        receipt=build/'receipt.json';receipt.write_text(json.dumps(r));ref=a.identity(receipt)
        seal=build/'companion-seal.json';seal.write_text(json.dumps(dict(schema='b5f-companion-seal-v1',receipt=ref)))
        return c.accepted(dict(companion=dict(receipt=ref,seal=a.identity(seal))),item,e['stage'])

    def test_positive_both_stages_and_real_project_reference(self):
        for stage in ('model-windows','slicing-windows'):
            with self.subTest(stage=stage),tempfile.TemporaryDirectory() as d:
                item,r,e,build,pin,extra,text=self.fixture(Path(d),stage)
                self.assertEqual(self.accepted(item,r,e,build,extra()),[e['current']])
                ref=pin(Path(e['basis']['project']['path']),'<Project><ItemGroup Condition="\'$(Configuration)|$(Platform)\'==\'Release|x64\'"><ProjectReference Include="tbb.vcxproj"/></ItemGroup></Project>')
                e['basis']['project']=ref
                c.basis(e,item,stage,[],extra())

    def test_stage_negatives_empty_comment_debug_substring_unrelated_and_tlogs(self):
        for stage in ('model-windows','slicing-windows'):
            for variant in ('empty','comment','debug','substring','unrelated','sdklog','deplog'):
                with self.subTest(stage=stage,variant=variant),tempfile.TemporaryDirectory() as d:
                    item,r,e,build,pin,extra,text=self.fixture(Path(d),stage)
                    if variant=='empty':e['basis']['dependencyImportLib']=''
                    if variant=='comment':text='<Project><!-- tbb.vcxproj tbb12.lib are deliberately NOT dependencies --></Project>'
                    if variant=='debug':text=text.replace('Release|x64','Debug|x64')
                    if variant=='substring':text=text.replace('tbb12.lib','not-tbb12.lib-extra')
                    if variant=='unrelated':e['basis']['dependencyImportLib']='other.lib'
                    if variant in ('sdklog','deplog'):
                        key='sdkLinkTlog' if variant=='sdklog' else 'linkTlog'
                        p=Path(e['basis'][key]['path']);e['basis'][key]=pin(p,'prefix'+str(build/'Release/tbb12.lib')+'suffix')
                    e['basis']['project']=pin(Path(e['basis']['project']['path']),text)
                    with self.assertRaises(ValueError):c.basis(e,item,stage,[],extra())

    def test_complete_accepted_rejects_original_comment_only_counterexample(self):
        for stage in ('model-windows','slicing-windows'):
            for empty in (True,False):
                with self.subTest(stage=stage,empty=empty),tempfile.TemporaryDirectory() as d:
                    item,r,e,build,pin,extra,text=self.fixture(Path(d),stage)
                    e['basis']['dependencyImportLib']='' if empty else 'unrelated.lib'
                    e['basis']['project']=pin(Path(e['basis']['project']['path']),'<Project><!-- tbb.vcxproj is not a dependency --></Project>')
                    with self.assertRaises(ValueError):self.accepted(item,r,e,build,extra())


if __name__=='__main__':unittest.main()
