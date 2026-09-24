"""Protocol/config tests use stdlib only; no provider, torch or network calls."""
import copy
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

import local_semantic_worker as worker


class LocalSemanticWorkerTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="orca semantic 配置 ")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.config = {
            "schema": worker.CONFIG_SCHEMA, "enabled": True,
            "python_executable": sys.executable, "weights_directory": str(self.root),
            "cpu_threads": 4, "timeout_seconds": 120, "cache_bytes": 1024**3,
        }

    def write_config(self, config=None):
        path = self.root / "配置.json"
        path.write_text(json.dumps(self.config if config is None else config), encoding="utf-8")
        return path

    def test_import_does_not_load_ml_dependencies(self):
        script = "import sys; import local_semantic_worker; assert 'torch' not in sys.modules; assert 'facer' not in sys.modules"
        completed = subprocess.run([sys.executable, "-c", script], cwd=Path(worker.__file__).parent,
                                   capture_output=True, timeout=10)
        self.assertEqual(completed.returncode, 0, completed.stderr)

    def test_explicit_unicode_config_and_resource_limits(self):
        self.assertEqual(worker.load_config(self.write_config()), self.config)
        for field, value in (("cpu_threads", 0), ("cpu_threads", 9), ("cpu_threads", True),
                             ("timeout_seconds", 9), ("timeout_seconds", 601),
                             ("cache_bytes", -1), ("cache_bytes", 4 * 1024**3 + 1),
                             ("enabled", 1), ("python_executable", "python"),
                             ("weights_directory", "relative"), ("weights_directory", "x\0x")):
            with self.subTest(field=field, value=value):
                changed = {**self.config, field: value}
                with self.assertRaises(worker.WorkerError):
                    worker.load_config(self.write_config(changed))

    def test_config_rejects_unknown_keys_duplicates_and_nonfinite_values(self):
        path = self.write_config({**self.config, "shell_command": "forbidden"})
        with self.assertRaises(worker.WorkerError):
            worker.load_config(path)
        for text in ('{"enabled":true,"enabled":false}', '{"x":NaN}', '{"x":Infinity}', '[]', '"text"'):
            with self.subTest(text=text):
                path.write_text(text, encoding="utf-8")
                with self.assertRaises(worker.WorkerError):
                    worker.load_config(path)

    def test_config_read_has_size_limit(self):
        path = self.root / "large.json"
        path.write_bytes(b" " * (worker.MAX_CONFIG_BYTES + 1))
        with self.assertRaisesRegex(worker.WorkerError, "input_too_large"):
            worker.load_config(path)

    def test_hash_checks_actual_bytes_and_size(self):
        path = self.root / "weight"
        path.write_bytes(b"abc")
        self.assertEqual(worker.sha256_file(path, 3), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")
        with self.assertRaises(worker.WorkerError):
            worker.sha256_file(path, 4)

    def test_missing_weights_are_detected_before_importing_models(self):
        with self.assertRaisesRegex(worker.WorkerError, "weights_missing_or_invalid"):
            worker.load_models(self.config)

    def test_equal_size_wrong_weight_hash_is_rejected(self):
        with mock.patch.object(worker, "sha256_file", return_value="0" * 64):
            with self.assertRaisesRegex(worker.WorkerError, "weights_hash_mismatch"):
                worker.check_weights(self.root)

    def test_disabled_probe_does_not_load_models(self):
        with mock.patch.object(worker, "load_models", side_effect=AssertionError("must not run")):
            result = worker.probe({**self.config, "enabled": False})
        self.assertEqual(result, {"status": "disabled", "capability_ready": False})

    def test_identity_inspection_uses_actual_weight_bytes_without_loading_ml(self):
        # The production hash reader runs against small owned fixture weights.
        weights = {name: (len(name), hashlib.sha256(name.encode()).hexdigest())
                   for name in ("detector.bin", "parser.bin")}
        for name in weights:
            (self.root / name).write_bytes(name.encode())
        with mock.patch.object(worker, "WEIGHTS", weights), \
             mock.patch.object(worker.importlib.metadata, "version", return_value="fixture-version"), \
             mock.patch.object(worker, "load_models", side_effect=AssertionError("must not load models")):
            result = worker.probe(self.config, identity_only=True)
            self.assertEqual(result["status"], "identity")
            self.assertFalse(result["capability_ready"])
            self.assertFalse(result["mesh_requests_ready"])
            identity = result["identity"]
            self.assertEqual(identity["weights"], {name: data[1] for name, data in weights.items()})
            self.assertEqual(identity["worker_sha256"], worker.sha256_file(Path(worker.__file__)))
            self.assertEqual(identity["python_executable_sha256"], worker.sha256_file(Path(sys.executable)))
            expected = hashlib.sha256(json.dumps(identity, sort_keys=True, separators=(",", ":")).encode()).hexdigest()
            self.assertEqual(result["runtime_fingerprint"], expected)
            (self.root / "parser.bin").write_bytes(b"x" * len("parser.bin"))
            with self.assertRaisesRegex(worker.WorkerError, "weights_hash_mismatch"):
                worker.probe(self.config, identity_only=True)

    def test_identity_cli_in_a_fresh_process_never_imports_ml(self):
        # Fail on attempted imports, including an import swallowed by the worker.
        script = '''import importlib.abc, json, pathlib, sys
import local_semantic_worker as w
class Guard(importlib.abc.MetaPathFinder):
 def find_spec(self, fullname, path=None, target=None):
  if fullname.split('.')[0] in ('torch', 'torchvision', 'facer', 'numpy', 'PIL'):
   raise SystemExit('forbidden ML import: ' + fullname)
sys.meta_path.insert(0, Guard())
w.check_weights = lambda _: {name: spec[1] for name, spec in w.WEIGHTS.items()}
w.importlib.metadata.version = lambda _: 'fixture-version'
raise SystemExit(w.main(sys.argv[1:]))
'''
        output = self.root / "identity.json"
        completed = subprocess.run([sys.executable, "-c", script, "--probe", "--identity-only",
                                    "--config", str(self.write_config()), "--output", str(output)],
                                   cwd=Path(worker.__file__).parent, capture_output=True, timeout=10)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        report = json.loads(output.read_text())
        self.assertEqual(report["status"], "identity")
        self.assertFalse(report["capability_ready"])

    def test_identity_mode_preserves_disabled_and_interpreter_guards(self):
        with mock.patch.object(worker, "check_weights", side_effect=AssertionError("must not read weights")):
            self.assertEqual(worker.probe({**self.config, "enabled": False}, identity_only=True),
                             {"status": "disabled", "capability_ready": False})
            other = self.root / "wrong-python.exe"
            other.write_bytes(b"fixture")
            with self.assertRaisesRegex(worker.WorkerError, "interpreter_identity_mismatch"):
                worker.probe({**self.config, "python_executable": str(other)}, identity_only=True)

    def test_explicit_full_probe_still_loads_models(self):
        with mock.patch.object(worker, "load_models", side_effect=worker.WorkerError("model execution reached")) as loader:
            with self.assertRaisesRegex(worker.WorkerError, "model execution reached"):
                worker.probe(self.config)
        loader.assert_called_once_with(self.config)

    def test_probe_requires_the_configured_interpreter(self):
        other = self.root / "different-python.exe"
        other.write_bytes(b"not executable")
        with self.assertRaisesRegex(worker.WorkerError, "interpreter_identity_mismatch"):
            worker.probe({**self.config, "python_executable": str(other)})

    def test_atomic_response_never_overwrites_prior_result(self):
        path = self.root / "result.json"
        worker.write_response(path, {"ok": True})
        self.assertEqual(json.loads(path.read_text()), {"ok": True})
        self.assertFalse(path.with_name(path.name + ".partial").exists())
        with self.assertRaisesRegex(worker.WorkerError, "response_already_exists"):
            worker.write_response(path, {"ok": False})
        self.assertEqual(json.loads(path.read_text()), {"ok": True})

    def test_partial_and_oversize_response_are_not_published(self):
        path = self.root / "result.json"
        path.with_name(path.name + ".partial").write_bytes(b"interrupted")
        with self.assertRaises(OSError):
            worker.write_response(path, {"ok": True})
        self.assertFalse(path.exists())
        with self.assertRaisesRegex(worker.WorkerError, "response_too_large"):
            worker.write_response(self.root / "too-big.json", {"data": "x" * worker.MAX_RESPONSE_BYTES})
        self.assertFalse((self.root / "too-big.json").exists())

    def test_cli_emits_small_structured_failure_with_no_exception_details(self):
        output = self.root / "result.json"
        result = subprocess.run([sys.executable, "-I", worker.__file__, "--probe", "--config",
                                 str(self.write_config()), "--output", str(output)],
                                capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 2)
        report = json.loads(output.read_text())
        self.assertEqual(report["schema"], worker.PROTOCOL)
        self.assertEqual(report["error_code"], "weights_missing_or_invalid")
        self.assertFalse(report["capability_ready"])
        self.assertNotIn(str(self.root), output.read_text())
        self.assertEqual(result.stderr, b"")

    def test_cli_disabled_is_successfully_unavailable(self):
        output = self.root / "result.json"
        config = self.write_config({**self.config, "enabled": False})
        result = subprocess.run([sys.executable, "-I", worker.__file__, "--probe", "--config",
                                 str(config), "--output", str(output)], capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 0)
        self.assertEqual(json.loads(output.read_text())["status"], "disabled")

    def test_network_hook_rejects_before_socket_operation(self):
        # sys.audit invokes the installed handler without creating or using a socket.
        script = ("import sys; import local_semantic_worker as w; w.restrict_network(); "
                  "sys.audit('socket.connect', None, ('127.0.0.1', 1))")
        result = subprocess.run([sys.executable, "-c", script], cwd=Path(worker.__file__).parent,
                                capture_output=True, timeout=10)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b"network_attempt_rejected", result.stderr)


if __name__ == "__main__":
    unittest.main()
