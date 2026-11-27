"""Normal packaging contract tests. All process/Windows boundaries are fake."""
import ast
import copy
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import adapter as a
import artifact_owner as o
import contracts as c
import suspended_root as s

HERE = Path(__file__).resolve().parent
SUPPORT = Path('D:/Codex/artifacts/task-20261122-v102-api-upstream-integration/evidence/d3cc-validation-tools-v8')


def fixture(root):
    p = dict(source=a.SOURCE, toolSource='a'*40, sdkRepository=str(root/'sdk'),
             staging=str(root/'stage'), bindingPath=str(root/'plan.json'),
             ownerDirectory=str(SUPPORT), artifactOwner=o.__file__,
             tbbSource=str(root/'tbb'), cgalArchive=str(root/'cgal'),
             dependencyManifest=str(root/'deps'), debug=str(root/'debug'), output=str(root/'out'))
    for key, name in [('python','python.exe'),('node','node.exe'),('git','git.exe'),
                      ('readobj','llvm-readobj.exe'),('readelf','llvm-readelf.exe'),('strip','llvm-strip.exe')]:
        path=root/name;path.write_bytes(b'fake executable, never run');p[key]=str(path)
    paths=[HERE/name for name in ('adapter.py','contracts.py','artifact_owner.py','api_context.py',
           'suspended_root.py','suspended_launch.py','companion.py','static_proof.py')]
    paths += [SUPPORT/name for name in o.SUPPORT]
    paths += [Path(p[k]) for k in ('python','node','git','readobj','readelf','strip')]
    paths += [Path(p['sdkRepository'])/name for name in ('src/orca_model_sdk/tools/package_sdk.py',
        'src/orca_model_sdk/tools/strip_android_model.py','src/orca_model_sdk/repair_core/verify_approved_tbb.py',
        'src/orca_model_sdk/repair_core/approved_tbb.json','mobile-slicing/build_android_arm64.cjs')]
    paths += [Path(p[k]) for k in ('cgalArchive','dependencyManifest')]
    for path in paths:
        if not path.exists():path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(b'fake pinned source, never run')
    p['postprocessProof']=a.identity(HERE/'static_proof.py')
    p['toolFiles']=[a.identity(path) for path in paths]
    p['lifecycle']=c.package_lifecycle(p, enabled=True)
    return p


class PackageLifecycle(unittest.TestCase):
    def test_default_disabled_and_typed_enabled_plan_go(self):
        with tempfile.TemporaryDirectory() as d:
            p=fixture(Path(d));good=copy.deepcopy(p['lifecycle'])
            self.assertEqual(c.normal_lifecycle(p,a,dict(lifecycle=good)),good)
            self.assertFalse(c.package_lifecycle(p)['enabled'])
            for change in (None, c.package_lifecycle(p), dict(good,enabled=1),
                           dict(good,effectiveCreationFlags=0x80400), dict(good,profile='legacy'),
                           dict(good,neverResumedAbortExitCode=True), dict(good,custodian='OTHER'),
                           dict(good,commandTimeoutSeconds=True), dict(good,cwd=str(Path(d)/'wrong')),
                           dict(good,commands=good['commands']+[['python.exe','-c','pass']])):
                with self.subTest(change=change), self.assertRaises(ValueError):
                    c.normal_lifecycle(dict(p,lifecycle=change),a)
                with self.assertRaises(ValueError):c.normal_lifecycle(p,a,dict(lifecycle=change))

    def test_all_runtime_core_and_executable_pins_required_and_verified(self):
        with tempfile.TemporaryDirectory() as d:
            p=fixture(Path(d))
            for index in range(len(p['toolFiles'])):
                q=copy.deepcopy(p);q['toolFiles'].pop(index);q['lifecycle']=c.package_lifecycle(q,enabled=True)
                with self.assertRaisesRegex(ValueError,'missing lifecycle closure|static proof differs'):c.normal_lifecycle(q,a)
            q=copy.deepcopy(p);q['toolFiles'][0]['sha256']='0'*64
            q['lifecycle']=c.package_lifecycle(q,enabled=True)
            with self.assertRaisesRegex(ValueError,'identity changed'):c.normal_lifecycle(q,a)

    def test_owner_rejects_invalid_normal_before_runtime_and_no_legacy_fallback(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);p=fixture(root);argv=o.commands(p)[0]
            kw=dict(permitted_commands=o.commands(p),permitted_images=[p[k] for k in
                    ('python','node','readobj','readelf','strip','git')],cwd=p['sdkRepository'],
                    timeout=7200,lifecycle_profile=s.PROFILE,package_plan=p)
            variants=[dict(kw,lifecycle_profile=None),dict(kw,cwd=str(root)),dict(kw,timeout=60),
                      dict(kw,permitted_commands=o.commands(p)+[['arbitrary']]),
                      dict(kw,package_plan=dict(p,postprocessProof=dict(path=str(root/'arbitrary.py')))),
                      dict(kw,package_plan=dict(p,lifecycle=None))]
            with patch.object(o,'runtime',side_effect=AssertionError('unexpected runtime')) as runtime:
                for bad in variants:
                    with self.assertRaises((AssertionError,ValueError)):
                        o.run_owned(argv,root/'log',{},lambda:None,root/'cancel',lambda:None,**bad)
                runtime.assert_not_called()

    def test_owner_accepts_derived_normal_contract_to_suspended_runtime(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);p=fixture(root);argv=o.commands(p)[0]
            class ReachedSuspended(Exception):pass
            with patch.object(o,'runtime',return_value=(None,)*8), patch.object(s,'SuspendedContext',return_value=object()), \
                 patch.object(s,'runtime',side_effect=ReachedSuspended) as runtime:
                with self.assertRaises(ReachedSuspended):
                    o.run_owned(argv,root/'log',{},lambda:None,root/'cancel',lambda:None,
                        permitted_commands=o.commands(p),permitted_images=[p[k] for k in
                        ('python','node','readobj','readelf','strip','git')],cwd=p['sdkRepository'],
                        timeout=7200,lifecycle_profile=s.PROFILE,package_plan=p)
                runtime.assert_called_once()
            o.TRACE_CONTEXT=None

    def test_adapter_passes_exact_plan_profile_and_budget(self):
        tree=ast.parse(Path(a.__file__).read_text())
        fn=next(n for n in ast.walk(tree) if isinstance(n,ast.FunctionDef) and n.name=='command')
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);p=fixture(root);seen=[]
            def run(argv,path,*args,**kw):seen.append(kw);Path(path).write_text('fake output')
            env=dict(plan=p,lifecycle=p['lifecycle'],repo=Path(p['sdkRepository']),source_check=lambda:None,
                     admit_cancel=lambda x:None,cancel=root/'cancel',state={'commands':[]},
                     gate=lambda *x:None,save=lambda:None,evidence=root,base_env={},os=a.os,
                     commands=o.commands,run_owned=run)
            exec(compile(ast.Module(body=[fn],type_ignores=[]),'normal-adapter-command','exec'),env)
            self.assertEqual(env['command'](o.commands(p)[0],'test'),'fake output')
            self.assertIs(seen[0]['package_plan'],p)
            self.assertEqual(seen[0]['lifecycle_profile'],s.PROFILE)
            self.assertEqual(seen[0]['timeout'],7200)
            self.assertEqual(seen[0]['permitted_commands'],o.commands(p))

    def test_four_producers_still_block_before_contract_or_owner(self):
        with patch.object(o,'runtime',side_effect=AssertionError('forbidden')) as runtime:
            with self.assertRaisesRegex(ValueError,'unbound stage'):
                a.validate_binding(dict(source=a.SOURCE,tree=a.TREE,producers=dict.fromkeys(a.STAGES)))
            runtime.assert_not_called()

    def test_go_requires_exact_full_new_closure(self):
        with tempfile.TemporaryDirectory() as d:
            p=fixture(Path(d));p.update(stageInputs=[],producers={},ownerQualification={})
            go=dict(lifecycle=copy.deepcopy(p['lifecycle']),closure=copy.deepcopy(p['toolFiles']))
            c.go_closure(p,go,a)
            go['closure'].pop()
            with self.assertRaisesRegex(ValueError,'exact closure differs'):c.go_closure(p,go,a)

    def test_qualification_cannot_reuse_old_commit_owner_or_lifecycle(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);p=fixture(root);p['ownerProtocol']=o.PROTOCOL
            pins={Path(x['path']).resolve():x for x in p['toolFiles']}
            report=root/'report.md';report.write_text('synthetic fake qualification, no actual grant')
            receipt=dict(schema='sdk-artifact-owner-qualification-v1',outcome='passed',protocol=o.PROTOCOL,
                         toolSource=p['toolSource'],owner=pins[Path(o.__file__).resolve()],
                         support={n:pins[(SUPPORT/n).resolve()] for n in o.SUPPORT},
                         lifecycle=p['lifecycle'],report=a.identity(report))
            def bind(changes):
                p['ownerQualification']={}
                for role in ('review','qa'):
                    path=root/(role+'.json');path.write_text(json.dumps(dict(receipt,role=role,**changes)))
                    p['ownerQualification'][role]=a.identity(path)
            with patch('shutil.which',return_value=p['git']):
                bind({});c.owner_protocol(p,a)
                for changes in (dict(toolSource='b'*40),dict(owner=dict(receipt['owner'],sha256='0'*64)),
                                dict(lifecycle=None),dict(lifecycle=dict(p['lifecycle'],enabled=1))):
                    bind(changes)
                    with self.assertRaises(ValueError):c.owner_protocol(p,a)


if __name__ == '__main__':unittest.main()
