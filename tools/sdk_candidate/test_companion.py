import copy
import json
from pathlib import Path
import tempfile
import unittest
import adapter as a
import companion as c
import test_fix1


class CompanionTests(unittest.TestCase):
    def fixture(self,root):
        plan=test_fix1.Fix1().fixture(root)
        item=plan['producers']['model-windows']
        build=root/'build'
        output=build/'CMakeCache.txt';build.mkdir()
        output.write_text('ORCA_MODEL_SOURCE_COMMIT:STRING='+a.SOURCE+'\nCMAKE_CACHEFILE_DIR:INTERNAL='+str(build)+'\n')
        item['rawSource']['buildPath']=str(build/'Release/orca_model_sdk_c.dll')
        normal=root/'original-plan.json'
        normal.write_text(json.dumps(dict(stages={'model-windows':dict(
            configureArgv=['-B',str(build),'-DORCA_MODEL_SOURCE_COMMIT:STRING='+a.SOURCE],
            buildArgv=['cmake','--build',str(build),'--target','orca_model_sdk','orca_model_runner'])})))
        item['fullIndex']['files'].append(a.identity(normal))
        Path(item['seal']['path']).write_text(json.dumps(item['fullIndex']))
        item['seal']=a.identity(item['seal']['path'])
        entry=dict(role='cache',stage='model-windows',producerBuildRoot=str(build),
            current=a.identity(output),firstObservedUtc='2026-11-26T00:00:00Z',evidenceTiming=c.TIMING,
            basis=dict(normalPlan=a.identity(normal)))
        graphref=next(x for x in c.index(item) if Path(x['path']).name=='actual-compiler-inputs.json')
        graph=a.small_json(graphref['path'])
        before=c.index(item)+[item['seal'],item['rawSource']['raw']]+graph
        receipt=dict(schema=c.SCHEMA,origin=c.origin(item,'model-windows'),evidenceTiming=c.TIMING,
                     driftBefore=before,driftAfter=before,companions=[entry])
        return item,receipt,entry

    def test_cache_first_observed_and_original_bytes_untouched(self):
        with tempfile.TemporaryDirectory() as d:
            item,r,e=self.fixture(Path(d))
            sealbytes=Path(item['seal']['path']).read_bytes()
            c.validate(r,item,'model-windows')
            self.assertEqual(Path(item['seal']['path']).read_bytes(),sealbytes)
            self.assertEqual(r['companions'][0]['evidenceTiming'],c.TIMING)

    def test_wrong_origin_stage_fake_history_missing_drift_raw_bypass(self):
        with tempfile.TemporaryDirectory() as d:
            item,r,e=self.fixture(Path(d))
            changes=[lambda x:x['origin'].update(stage='model-android'),
                     lambda x:x.update(evidenceTiming='HISTORICAL_COMPILE_HASH'),
                     lambda x:x.update(driftAfter=[]),lambda x:x.update(driftBefore=[],driftAfter=[]),
                     lambda x:x['companions'][0].update(role='raw-sdk'),
                     lambda x:x['companions'][0].update(stage='model-android'),
                     lambda x:x['companions'][0].update(current=item['rawSource']['raw'])]
            for change in changes:
                bad=copy.deepcopy(r);change(bad)
                with self.assertRaises(ValueError):c.validate(bad,item,'model-windows')

    def test_missing_real_target_or_link_keeps_unbound(self):
        with tempfile.TemporaryDirectory() as d:
            item,r,e=self.fixture(Path(d));build=Path(e['producerBuildRoot'])
            lib=build/'Release/orca_model_sdk_c.lib';lib.parent.mkdir();lib.write_bytes(b'!<arch>\n')
            e.update(role='import-lib',current=a.identity(lib))
            e['basis']['target']='invented_target'
            with self.assertRaisesRegex(ValueError,'missing actual target'):c.validate(r,item,'model-windows')
            e['basis']['target']='orca_model_sdk'
            project=build/'orca_model_sdk.vcxproj';project.write_text('<Project/>')
            ref=a.identity(project);item['fullIndex']['files'].append(ref)
            oldseal=item['seal']
            Path(item['seal']['path']).write_text(json.dumps(item['fullIndex']));item['seal']=a.identity(item['seal']['path'])
            r['driftBefore'][:]=[item['seal'] if x==oldseal else x for x in r['driftBefore']]
            r['origin']=c.origin(item,'model-windows');r['driftBefore'].append(ref)
            e['basis']['project']=ref
            with self.assertRaisesRegex(ValueError,'exact project link'):c.validate(r,item,'model-windows')

    def test_collector_no_permission_no_side_effect(self):
        with self.assertRaises((ValueError,KeyError)):
            c.collect({},dict(companionCollectionAuthorized=False,source=a.SOURCE))


if __name__=='__main__':unittest.main()
