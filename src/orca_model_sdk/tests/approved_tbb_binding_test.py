"""Exercise the real build/producer input guards without linking a dummy SDK."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

SOURCE = Path(__file__).resolve().parents[1]
SPEC = SOURCE / 'repair_core/approved_tbb.json'
VERIFY = SOURCE / 'repair_core/verify_approved_tbb.py'
module_spec = importlib.util.spec_from_file_location('approved_tbb', VERIFY)
guard = importlib.util.module_from_spec(module_spec)
module_spec.loader.exec_module(guard)


class BindingTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='approved-tbb-', dir=ARGS.workspace)
        self.root = Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def verify(self, library=None, include=None, output=None):
        return subprocess.run([sys.executable, str(VERIFY), '--library', str(library or ARGS.library),
            '--include', str(include or ARGS.include), '--manifest', str(SPEC),
            '--output', str(output or self.root/'binding.json')], capture_output=True, text=True)

    def test_exact_old_runtime_and_headers(self):
        target = self.root/'binding.json'
        self.assertEqual(self.verify(output=target).returncode, 0)
        binding = json.loads(target.read_text())
        self.assertEqual(binding['runtime']['sha256'], 'cc093ffa23653600950960121ab5c4261e6586aa14f7dadc91aab6773daaa038')
        self.assertEqual(binding['headerCount'], 139)
        self.assertFalse(binding['runtimeBuiltByThisProducer'])
        original = target.read_bytes()
        self.assertEqual(self.verify(output=target).returncode, 0)
        self.assertEqual(target.read_bytes(), original)

    def test_missing_relative_and_different_runtime_create_no_binding(self):
        wrong = self.root/'libtbb.so'
        wrong.write_bytes(Path(ARGS.library).read_bytes() + b'changed')
        for library in [self.root/'missing.so', Path('libtbb.so'), wrong]:
            with self.subTest(library=library):
                self.assertNotEqual(self.verify(library=library).returncode, 0)
                self.assertFalse((self.root/'binding.json').exists())

    def test_mutated_missing_extra_headers_create_no_binding(self):
        include = self.root/'include'
        shutil.copytree(ARGS.include, include)
        first = sorted(p for p in include.rglob('*') if p.is_file())[0]
        original = first.read_bytes()
        first.write_bytes(original + b'changed')
        self.assertNotEqual(self.verify(include=include).returncode, 0)
        first.unlink()
        self.assertNotEqual(self.verify(include=include).returncode, 0)
        first.write_bytes(original)
        (include/'unexpected.h').write_text('unexpected')
        self.assertNotEqual(self.verify(include=include).returncode, 0)
        self.assertFalse((self.root/'binding.json').exists())

    def test_conflicting_binding_is_not_overwritten(self):
        target = self.root/'binding.json'
        target.write_text('{"other":true}')
        self.assertNotEqual(self.verify(output=target).returncode, 0)
        self.assertEqual(target.read_text(), '{"other":true}')
        self.assertFalse(target.with_suffix('.json.next').exists())

    def configure(self, name, extra):
        project = self.root/name
        project.mkdir()
        text = ('cmake_minimum_required(VERSION 3.22)\nproject(binding NONE)\n'
            'add_library(orca_model_sdk INTERFACE)\n'
            f'set(Python3_EXECUTABLE "{Path(sys.executable).as_posix()}")\n'
            f'set(ORCA_MODEL_DEPENDENCY_ROOT "{Path(ARGS.include).parents[1].as_posix()}")\n'
            + extra + '\n'
            f'include("{(SOURCE/"cmake/ApprovedAndroidTbb.cmake").as_posix()}")\n')
        (project/'CMakeLists.txt').write_text(text)
        return subprocess.run(['cmake', '-S', str(project), '-B', str(project/'build'), '-G', 'Ninja',
            f'-DCMAKE_MAKE_PROGRAM={ARGS.ninja}'], capture_output=True, text=True)

    def test_cmake_missing_explicit_runtime_fails(self):
        result = self.configure('missing', '')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('explicit absolute', result.stderr)
        self.assertFalse((self.root/'missing/build/approved-tbb-binding.json').exists())

    def test_cmake_conflicting_target_fails(self):
        result = self.configure('conflict', 'add_library(TBB::tbb INTERFACE IMPORTED)')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Conflicting TBB target', result.stderr)

    def test_cmake_wrong_runtime_fails_before_binding(self):
        wrong = self.root/'libtbb.so'
        wrong.write_bytes(b'not approved')
        result = self.configure('wrong', f'set(ORCA_MODEL_ANDROID_TBB_RUNTIME "{wrong.as_posix()}")')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('SHA/size mismatch', result.stderr)
        self.assertFalse((self.root/'wrong/build/approved-tbb-binding.json').exists())

    def test_producer_missing_or_tampered_binding_writes_no_package(self):
        host = self.root/'host'
        android = self.root/'android'
        host.mkdir(); android.mkdir()
        output = self.root/'package'
        args = [sys.executable, str(SOURCE/'tools/package_sdk.py'), '--windows-build', str(host),
            '--android-build', str(android), '--repair-tbb-source', str(Path(ARGS.include).parent),
            '--output', str(output)]
        result = subprocess.run(args, capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Missing approved Android TBB build binding', result.stderr)
        self.assertFalse(output.exists())
        binding = guard.validate(ARGS.library, ARGS.include, SPEC)
        binding['runtimeBuiltByThisProducer'] = True
        (android/'approved-tbb-binding.json').write_text(json.dumps(binding))
        result = subprocess.run(args, capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('no longer matches', result.stderr)
        self.assertFalse(output.exists())


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--library', required=True)
    parser.add_argument('--include', required=True)
    parser.add_argument('--workspace', required=True)
    parser.add_argument('--ninja', required=True)
    ARGS, remaining = parser.parse_known_args()
    Path(ARGS.workspace).mkdir(parents=True, exist_ok=True)
    unittest.main(argv=[sys.argv[0]] + remaining)
