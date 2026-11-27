import copy
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import adapter as a


class AdapterTests(unittest.TestCase):
    def terminal(self):
        return dict(source=a.SOURCE, actualExitCode=0, sourceBindings=1,
                    sdkExecuted=False, fullSourceProvenancePassed=True, ownerPid16320Absent=True)

    def raw(self):
        return dict(source=a.SOURCE, cleanBuild=True, copiedPredecessorObjects=0, priorObjectsReused=False)

    def test_successful_compact_schema_preserves_original(self):
        terminal, raw = self.terminal(), self.raw()
        original = copy.deepcopy(terminal)
        a.terminal_ok('model-windows', terminal, raw)
        self.assertEqual(terminal, original)

    def test_wrong_source_failed_or_resource_error_rejected(self):
        for field, value in [('source', '434021'), ('actualExitCode', 54), ('ownerPid16320Absent', False)]:
            terminal = self.terminal()
            terminal[field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                a.terminal_ok('model-windows', terminal, self.raw())

    def test_no_object_relabel(self):
        for key, value in [('cleanBuild', False), ('copiedPredecessorObjects', 1), ('priorObjectsReused', True)]:
            raw = self.raw()
            raw[key] = value
            with self.subTest(key=key), self.assertRaises(ValueError):
                a.terminal_ok('model-windows', self.terminal(), raw)

    def test_android_full_four_records_and_tail_failure(self):
        t = dict(source=a.SOURCE, actualExitCode=0, sourceBindings=1, sdkExecuted=False,
                 records=[dict(name=n, exitCode=0, state='returned', monitorErrors=0, runtimeViolations=0)
                          for n in ('configure', 'actual-ninja-commands', 'build', 'actual-ninja-deps')])
        a.terminal_ok('model-android', t, self.raw())
        t['records'][-1]['runtimeViolations'] = 1
        with self.assertRaises(ValueError):
            a.terminal_ok('model-android', t, self.raw())
        t['records'] = t['records'][:2]
        with self.assertRaises(ValueError):
            a.terminal_ok('model-android', t, self.raw())

    def test_unknown_windows_is_closed_before_io_or_process(self):
        with patch('subprocess.Popen', side_effect=AssertionError('must not launch')):
            with self.assertRaisesRegex(ValueError, 'unbound stage'):
                a.validate_binding(dict(source=a.SOURCE, tree=a.TREE, producers=dict.fromkeys(a.STAGES)))

    def test_stream_identity_not_full_json(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / 'execution.json'
            path.write_bytes(b'not JSON; full record streamed unchanged')
            with patch.object(Path, 'read_bytes', side_effect=AssertionError('no whole-file load')):
                pin = a.identity(path)
                a.verify(pin)
            path.write_bytes(b'changed')
            with self.assertRaises(ValueError):
                a.verify(pin)

    def test_stage_cache_exact_no_objects_no_overwrite(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            cache = root / 'cache'
            original = b'ORIGINAL:PATH=D:/producer/not/new/stage\r\n'
            cache.write_bytes(original)
            inputs = [dict(destination=s + '/CMakeCache.txt', source=a.identity(cache)) for s in a.STAGES]
            plan = dict(staging=str(root / 'fresh'), stageInputs=inputs)
            a.copy_inputs(plan)
            self.assertEqual((root / 'fresh/model-windows/CMakeCache.txt').read_bytes(), original)
            with self.assertRaises(FileExistsError):
                a.copy_inputs(plan)
            plan = dict(staging=str(root / 'other'), stageInputs=[dict(destination='bad.obj', source=a.identity(cache))])
            with self.assertRaisesRegex(ValueError, 'object'):
                a.copy_inputs(plan)

    def test_copy_cancel_keeps_partial_and_does_not_copy_next(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            src = root / 'input'
            src.write_bytes(b'x')
            plan = dict(staging=str(root / 'stage'), stageInputs=[dict(destination='first', source=a.identity(src)), dict(destination='second', source=a.identity(src))])
            def cancel():
                raise ValueError('cancel')
            with self.assertRaisesRegex(ValueError, 'cancel'):
                a.copy_inputs(plan, cancel)
            self.assertFalse((root / 'stage/second').exists())

    def test_paths_fail_closed(self):
        for name in ('../input', 'a/../b', '/absolute', 'C:/absolute', 'a\\b'):
            with self.subTest(name=name), self.assertRaises(ValueError):
                a.relative(name)

    def test_full_exports_private_kept_unknown_public_rejected(self):
        names = sorted('orca_mobile_slicing_' + n for n in a.PUBLIC['slicing'])
        def table(names):
            return '\n'.join('Export {\n  Ordinal: %s\n  Name: %s\n  RVA: 0x10\n}' % (i, n) for i, n in names)
        text = table([(1, '?Private@cpp')] + list(zip(range(433, 440), names)))
        self.assertEqual(len(a.parse_exports(text, 'slicing')), 8)
        with self.assertRaises(ValueError):
            a.parse_exports(text + '\n' + table([(440, 'orca_unknown')]), 'slicing')
        with self.assertRaises(ValueError):
            a.parse_exports(text.replace('Ordinal: 433', 'Ordinal: 999'), 'slicing')

    def test_literal_verify_only_and_no_compiler_route(self):
        import ast
        tree = ast.parse(Path(a.__file__).read_text(encoding='utf8'))
        strings = [n.value for n in ast.walk(tree) if isinstance(n, ast.Constant) and isinstance(n.value, str)]
        self.assertIn('--verify-only', strings)
        self.assertNotIn('--build', strings)
        self.assertNotIn('-j2', strings)

    def test_go_missing_explicit_capabilities_load(self):
        with self.assertRaises(ValueError):
            a.execute({}, dict(source=a.SOURCE, planSha256='abc', artifactOnlyAuthorized=True, allowModelCapabilitiesLoad=False), 'abc')


if __name__ == '__main__':
    unittest.main()
