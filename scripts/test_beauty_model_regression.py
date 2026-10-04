import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import subprocess
import copy

import run_beauty_model_regression as runner


class ModelRegressionTests(unittest.TestCase):
    def model(self):
        return dict(id='sample', path='sample.glb', sha256='a'*64,
                    category='unknown', split='regression', stage='original')

    def test_duplicate_content_cannot_be_holdout(self):
        a = self.model()
        b = dict(a, id='another', split='holdout_candidate')
        with self.assertRaises(ValueError):
            runner.validate({'models': [a, b]})

    def test_unsafe_id_and_unverified_holdout_rejected(self):
        for edit in ({'id': '../escape'}, {'split': 'holdout'}, {'sha256': 'bad'}):
            with self.subTest(edit=edit), self.assertRaises(ValueError):
                runner.validate({'models': [dict(self.model(), **edit)]})

    def test_missing_or_empty_probe_does_not_pass(self):
        self.assertFalse(runner.check_probe({}, 'a'*64))
        self.assertFalse(runner.check_probe(dict(sha256='a'*64, source_unchanged=True,
                                                faces=0, pieces=1, changed_faces=0), 'a'*64))

    def test_performance_summary_excludes_first_and_warmup(self):
        rows = [dict(phase=phase, operation_seconds=value,
                     stages_seconds={name: value for name in runner.STAGES},
                     output_sha256='a'*64, changed_faces=0, memory=None)
                for phase, value in [('first_process_pass', 99), ('warmup', 88), ('warm', 1), ('warm', 3), ('warm', 2)]]
        result = runner.performance_summary({'measurements': rows}, 3)
        self.assertEqual(result['warm_operation_seconds'], {'median': 2, 'min': 1, 'max': 3})
        self.assertIsNone(result['process_lifetime_peak_working_set_bytes'])
        for corruption in ('missing', 'signature', 'nan', 'changed'):
            bad = copy.deepcopy(rows)
            if corruption == 'missing': bad.pop()
            if corruption == 'signature': bad[-1]['output_sha256'] = 'b'*64
            if corruption == 'nan': bad[-1]['stages_seconds']['partition'] = float('nan')
            if corruption == 'changed': bad[-1]['changed_faces'] = 1
            with self.subTest(corruption=corruption), self.assertRaises(ValueError):
                runner.performance_summary({'measurements': bad}, 3)

    def test_runner_success_failure_timeout_and_no_overwrite(self):
        for mode in ('pass', 'mismatch', 'timeout', 'no_report', 'external_material', 'manifest_changed',
                     'quality_pass', 'quality_missing', 'quality_wrong_source'):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                source = root/'sample.glb'
                source.write_bytes(b'frozen-model')
                if mode == 'external_material':
                    source = root/'sample.obj'
                    source.write_text('mtllib private.mtl\n')
                exe = root/'probe.exe'
                exe.write_bytes(b'fixture-executable')
                model = dict(self.model(), path=source.name, sha256=runner.digest(source))
                if mode == 'mismatch':
                    model['sha256'] = '0'*64
                manifest = root/'manifest.json'
                manifest.write_text(json.dumps({'models': [model]}))
                def fake_run(*args, **kwargs):
                    if mode == 'manifest_changed':
                        manifest.write_text('{}')
                    if mode == 'timeout':
                        raise subprocess.TimeoutExpired('fixture', 1)
                    if mode != 'no_report':
                        Path(kwargs['env']['ORCA_TARGET_OUTPUT']).write_text(json.dumps(
                            dict(sha256=model['sha256'], source_unchanged=True,
                                 faces=12, pieces=1, changed_faces=0)))
                    if mode in ('quality_pass', 'quality_wrong_source'):
                        folder = Path(kwargs['env']['ORCA_TARGET_QUALITY_DIR'])
                        folder.mkdir()
                        (folder/'manifest.json').write_text(json.dumps({'source_sha256':
                            model['sha256'] if mode == 'quality_pass' else '0'*64}))
                    return subprocess.CompletedProcess(args, 0)
                before = runner.digest(source)
                with patch.object(runner.subprocess, 'run', side_effect=fake_run) as call:
                    result = runner.run(manifest, exe, root/'out', timeout=1, quality=mode.startswith('quality_'))
                    self.assertEqual(result['passed'], mode in ('pass', 'quality_pass'))
                    if mode in ('mismatch', 'external_material'):
                        call.assert_not_called()
                self.assertEqual(runner.digest(source), before)
                manifest.write_text(json.dumps({'models': [model]}))
                with self.assertRaises(FileExistsError):
                    runner.run(manifest, exe, root/'out')

    def test_stage_replay_rejects_missing_mismatched_or_unequal_exports(self):
        for mode in ('pass', 'missing', 'wrong_input', 'wrong_output', 'wrong_ablation', 'incomplete'):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                source = root/'sample.glb'; source.write_bytes(b'frozen-model')
                exe = root/'probe.exe'; exe.write_bytes(b'fixture')
                model = dict(self.model(), path=source.name, sha256=runner.digest(source))
                manifest = root/'manifest.json'; manifest.write_text(json.dumps({'models': [model]}))
                def fake_run(*args, **kwargs):
                    env = kwargs['env']
                    self.assertEqual(env['ORCA_TARGET_QUALITY_STAGES'], '1')
                    Path(env['ORCA_TARGET_OUTPUT']).write_text(json.dumps(dict(
                        sha256=model['sha256'], source_unchanged=True, faces=12, pieces=1, changed_faces=0)))
                    folder = Path(env['ORCA_TARGET_QUALITY_DIR']); folder.mkdir()
                    meta = dict(source_sha256=model['sha256'], loaded_mesh_colors_sha256='a'*64,
                                geometry_id='geometry', palette=[], output_sha256='b'*64)
                    (folder/'manifest.json').write_text(json.dumps(meta))
                    for stage in ('01-create28', '02-regularize', '03-smooth', '04-ablation-no-regularize'):
                        if mode == 'missing' and stage == '02-regularize': continue
                        path = folder/stage; path.mkdir()
                        item = dict(meta)
                        if mode == 'incomplete' and stage == '02-regularize': item.pop('geometry_id')
                        if mode == 'wrong_input' and stage == '01-create28': item['geometry_id']='other'
                        if mode == 'wrong_output' and stage == '03-smooth': item['output_sha256']='c'*64
                        if mode == 'wrong_ablation' and stage.startswith('04'): item['source_sha256']='0'*64
                        (path/'manifest.json').write_text(json.dumps(item))
                    return subprocess.CompletedProcess(args, 0)
                with patch.object(runner.subprocess, 'run', side_effect=fake_run):
                    result = runner.run(manifest, exe, root/'out', quality=True, quality_stages=True)
                self.assertEqual(result['passed'], mode == 'pass')

    def test_quality_export_cannot_pollute_performance_measurements(self):
        with self.assertRaises(ValueError):
            runner.run('unused', 'unused', 'unused', repeats=5, quality=True)
        with self.assertRaises(ValueError):
            runner.run('unused', 'unused', 'unused', quality_stages=True)


if __name__ == '__main__':
    unittest.main()
