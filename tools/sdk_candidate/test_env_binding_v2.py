import copy
import hashlib
import json
import os
from pathlib import Path
import tempfile
import types
import unittest
from unittest.mock import patch
import owner_preflight as p
import test_owner_preflight


class BindingV2(unittest.TestCase):
    def test_only_volatile_value_may_change_actual_map_unmodified(self):
        prepared={'PATH':'exact','PYTHONPATH':'','GIT_CONFIG_VALUE_0':'','CODEX_TOOL_CALL_ID':'prepare'}
        current=dict(prepared,CODEX_TOOL_CALL_ID='actual')
        binding=p.prepare_environment_binding(prepared);snapshot=copy.deepcopy(current)
        audit=p.check_environment_binding(current,binding)
        self.assertEqual(audit['prepareNormalizedSha256'],audit['currentNormalizedSha256'])
        self.assertNotEqual(audit['prepareFullSha256'],audit['currentFullSha256'])
        self.assertEqual(current,snapshot)
        self.assertEqual(current['CODEX_TOOL_CALL_ID'],'actual')
        # Explicit value-only policy normalizes empty only when the key exists.
        p.check_environment_binding(dict(current,CODEX_TOOL_CALL_ID=''),binding)
        missing=dict(current);missing.pop('CODEX_TOOL_CALL_ID')
        with self.assertRaises(ValueError):p.check_environment_binding(missing,binding)
        absent=p.prepare_environment_binding({'PATH':'exact'})
        with self.assertRaises(ValueError):p.check_environment_binding({'PATH':'exact','CODEX_TOOL_CALL_ID':''},absent)

    def test_all_other_values_presence_unicode_and_policy_strict(self):
        env={'PATH':'exact','PYTHONPATH':'','PYTHONNOUSERSITE':'1','GIT_CONFIG_COUNT':'1','GIT_CONFIG_KEY_0':'safe.directory',
             'GIT_CONFIG_VALUE_0':'','UNICODE_TEST':'中文','CODEX_TOOL_CALL_ID':'a'}
        binding=p.prepare_environment_binding(env)
        for key in env:
            if key=='CODEX_TOOL_CALL_ID':continue
            bad=dict(env);bad[key]=bad[key]+'x'
            with self.subTest(key=key),self.assertRaises(ValueError):p.check_environment_binding(bad,binding)
            bad=dict(env);bad.pop(key)
            with self.assertRaises(ValueError):p.check_environment_binding(bad,binding)
        with self.assertRaises(ValueError):p.check_environment_binding(dict(env,UNKNOWN_NEW=''),binding)
        for key,value in [('schema','v1'),('valueOnlyKey','PATH'),('comparisonMarker','other'),('normalizedSha256','0'*64)]:
            bad=dict(binding);bad[key]=value
            with self.assertRaises(ValueError):p.check_environment_binding(env,bad)
        bad=dict(binding,extraNormalization='OTHER')
        with self.assertRaises(ValueError):p.check_environment_binding(env,bad)

    def test_old_schema_and_undeclared_or_conflicting_policy_rejected(self):
        with tempfile.TemporaryDirectory() as d,patch.object(p,'OUTPUT_ROOT',Path(d)),patch.object(p,'verify',lambda *x:None),\
             patch.object(p.subprocess,'check_output',side_effect=lambda argv,**kw:'a'*40+'\n' if argv[-1]=='HEAD' else ''):
            plan,go,now=test_owner_preflight.PreflightTests().fixture(Path(d))
            p.validate(plan,go,'pin',now)
            for role in ('plan','go'):
                a,b=copy.deepcopy(plan),copy.deepcopy(go)
                if role=='plan':a['schema']='artifact-owner-preflight-plan-v1'
                else:b['schema']='artifact-owner-preflight-go-v1'
                with self.assertRaises(ValueError):p.validate(a,b,'pin',now)
            bad=copy.deepcopy(go);bad['environmentBinding']['valueOnlyKey']='PATH'
            with self.assertRaises(ValueError):p.validate(plan,bad,'pin',now)
            bad=copy.deepcopy(go);bad['environmentSha256']='0'*64
            with self.assertRaises(ValueError):p.validate(plan,bad,'pin',now)
            bad=copy.deepcopy(plan);bad['environmentBinding']['prepareFullSha256']='0'*64
            with self.assertRaises(ValueError):p.validate(bad,go,'pin',now)
            for role in ('plan','go'):
                a,b=copy.deepcopy(plan),copy.deepcopy(go)
                if role=='plan':a.pop('environmentBinding')
                else:b.pop('environmentBinding')
                with self.assertRaises((KeyError,ValueError)):p.validate(a,b,'pin',now)

    def test_execute_audit_and_child_use_actual_not_comparison_marker(self):
        before=dict(os.environ);cwd=os.getcwd()
        with tempfile.TemporaryDirectory() as d:
            plan,go,now=test_owner_preflight.PreflightTests().fixture(Path(d))
            actual=dict(before,CODEX_TOOL_CALL_ID='actual-current-tool-id');actual.update(plan['envOverrides'])
            prepared=dict(actual,CODEX_TOOL_CALL_ID='prior-tool-id')
            plan['environmentBinding']=p.prepare_environment_binding(prepared)
            seen=[]
            resources=types.ModuleType('build_resources');resources.gate=lambda *args:None
            resources.sample=lambda:dict(freeKiB=10*1024**2,processes=[]);resources.heavy=lambda p:True
            owner=types.ModuleType('artifact_owner');owner.admit_cancel=lambda x:None
            def run(argv,path,step,*args,**kw):
                seen.append(kw['env']['CODEX_TOOL_CALL_ID'])
                self.assertEqual(kw['env']['CODEX_TOOL_CALL_ID'],'actual-current-tool-id')
                step.update(state='returned',exitCode=0,monitorErrors=[])
            owner.run_owned=run
            with patch.dict(p.sys.modules,{'build_resources':resources,'artifact_owner':owner}),\
                 patch.object(p,'validate',return_value=(Path(plan['output']),actual)),patch.object(p,'verify',lambda *x:None),\
                 patch.object(p,'git_state',lambda *x,**kw:None),patch.object(p,'fresh',lambda *x:None):
                p.execute(plan,go,'pin')
            result=json.loads((Path(plan['output'])/'execution.json').read_text())
            self.assertEqual(seen,['actual-current-tool-id']*2)
            self.assertNotEqual(result['environmentAudit']['prepareFullSha256'],result['environmentAudit']['currentFullSha256'])
            self.assertEqual(result['environmentAudit']['prepareNormalizedSha256'],result['environmentAudit']['currentNormalizedSha256'])
        self.assertEqual(hashlib.sha256(json.dumps(dict(os.environ),sort_keys=True).encode()).hexdigest(),
                         hashlib.sha256(json.dumps(before,sort_keys=True).encode()).hexdigest())
        self.assertEqual(os.getcwd(),cwd)


if __name__=='__main__':unittest.main()
