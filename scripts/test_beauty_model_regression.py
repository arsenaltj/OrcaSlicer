import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import subprocess

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

    def test_runner_success_failure_timeout_and_no_overwrite(self):
        for mode in ('pass', 'mismatch', 'timeout', 'no_report', 'external_material', 'manifest_changed'):
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
                    return subprocess.CompletedProcess(args, 0)
                before = runner.digest(source)
                with patch.object(runner.subprocess, 'run', side_effect=fake_run) as call:
                    result = runner.run(manifest, exe, root/'out', timeout=1)
                    self.assertEqual(result['passed'], mode == 'pass')
                    if mode in ('mismatch', 'external_material'):
                        call.assert_not_called()
                self.assertEqual(runner.digest(source), before)
                manifest.write_text(json.dumps({'models': [model]}))
                with self.assertRaises(FileExistsError):
                    runner.run(manifest, exe, root/'out')


if __name__ == '__main__':
    unittest.main()
