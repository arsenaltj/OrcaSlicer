"""Exercise the real Windows build scripts with disposable, offline fake tools.

The fake EXEs use the prebuilt console launcher shipped with pip and run a
small Python fixture. Unlike a .cmd shim, an EXE returns to a batch caller
without requiring CALL. No CMake, MSBuild, compiler, network access, or
application build is performed. Python's pip package supplies the launcher;
the test never installs or downloads anything.
"""

from __future__ import annotations

import datetime as dt
import hashlib
import importlib.resources
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import sysconfig
import tempfile
import textwrap
import unittest
import zipfile


ROOT = Path(__file__).resolve().parents[1]
STAGES = ["deps-configure", "deps-build", "configure", "build", "gettext", "install"]

FAKE_TOOL_SCRIPT = r'''
import json
import os
from pathlib import Path
import sys

tool = FAKE_TOOL
args = sys.argv[1:]
cwd = Path.cwd()
if tool == "msbuild":
    stage = "probe"
elif tool == "gettext":
    stage = "gettext"
elif "--build" not in args:
    stage = "deps-configure" if cwd.parent.name == "deps" else "configure"
else:
    target = args[args.index("--target") + 1]
    stage = {"deps": "deps-build", "install": "install"}.get(target, "build")
record = {"tool": tool, "stage": stage, "args": args, "cwd": str(cwd)}
with open(os.environ["FAKE_BUILD_EVENTS_PATH"], "a", encoding="utf-8") as stream:
    stream.write(json.dumps(record) + "\n")
if tool == "msbuild":
    os.write(1, b"17.14.5\n")
if tool == "cmake":
    if "-G" in args:
        (cwd / ".fake-generator").write_text(args[args.index("-G") + 1])
    if "--build" in args:
        generator_file = cwd / ".fake-generator"
        if generator_file.exists() and "Ninja" in generator_file.read_text():
            (cwd / ".ninja_log").write_text("# ninja log v5\n")
        for arg in args:
            if arg.lower().startswith("/bl:"):
                log = Path(arg[4:].split(";", 1)[0])
                log.parent.mkdir(parents=True, exist_ok=True)
                log.write_bytes(b"offline fake MSBuild log\n")
if stage == os.environ.get("FAKE_BUILD_BREAK_METRICS_STAGE"):
    Path(os.environ["ORCA_CI_METRICS_PATH"] + ".stage.json").unlink()
code = int(os.environ.get("FAKE_BUILD_FAIL_CODE", "37")) if stage == os.environ.get("FAKE_BUILD_FAIL_STAGE") else 0
os._exit(code)
'''


@unittest.skipUnless(os.name == "nt", "requires the Windows batch interpreter")
class WindowsBuildFixture(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="orca ci metrics ")
        self.addCleanup(self.temporary.cleanup)
        self.work = Path(self.temporary.name) / "source space 中文"
        self.work.mkdir()
        (self.work / "scripts").mkdir()
        (self.work / "deps").mkdir()
        self.tools = self.work / "fake tools"
        self.tools.mkdir()
        self.events_path = self.work / "fake-events.jsonl"
        self.metrics_path = self.work / "metrics output 中文" / "stages.jsonl"
        self.helper = self.work / "scripts" / "ci_windows_build_metrics.ps1"
        self.powershell = shutil.which("powershell.exe")
        self.assertIsNotNone(self.powershell, "Windows PowerShell is required by the batch script")

        # Copy only the scripts under test. The CMake source/dependency trees do
        # not exist in this disposable checkout, so a real build cannot occur.
        shutil.copy2(ROOT / "build_release_vs.bat", self.work)
        self.batch_sha256 = hashlib.sha256((self.work / "build_release_vs.bat").read_bytes()).hexdigest()
        if (ROOT / "scripts" / self.helper.name).exists():
            shutil.copy2(ROOT / "scripts" / self.helper.name, self.helper)
            self.helper_sha256 = hashlib.sha256(self.helper.read_bytes()).hexdigest()
        platform = sysconfig.get_platform()
        launcher_name = {"win-amd64": "t64.exe", "win-arm64": "t64-arm.exe", "win32": "t32.exe"}[platform]
        launcher = importlib.resources.files("pip._vendor.distlib").joinpath(launcher_name).read_bytes()
        for tool in ("cmake", "msbuild", "gettext"):
            archive = io.BytesIO()
            with zipfile.ZipFile(archive, "w") as bundle:
                bundle.writestr("__main__.py", f"FAKE_TOOL = {tool!r}\n" + textwrap.dedent(FAKE_TOOL_SCRIPT))
            shebang = f'#!"{sys.executable}"\n'.encode("utf-8")
            (self.tools / f"{tool}.exe").write_bytes(launcher + shebang + archive.getvalue())
        (self.work / "scripts" / "run_gettext.bat").write_bytes(
            b'@echo off\r\ngettext.exe\r\nexit /b %errorlevel%\r\n'
        )

        # Do not inherit signing/provider settings or a developer toolchain.
        allowed = ("SYSTEMROOT", "WINDIR", "COMSPEC", "TEMP", "TMP", "PROCESSOR_ARCHITECTURE", "PROCESSOR_ARCHITEW6432")
        self.env = {key: os.environ[key] for key in allowed if key in os.environ}
        windows = Path(os.environ["SYSTEMROOT"])
        self.env.update({
            "PATH": os.pathsep.join(map(str, (self.tools, Path(sys.base_prefix), windows / "System32", windows, Path(self.powershell).parent))),
            "PYTHONUTF8": "1",
            "PYTHONDONTWRITEBYTECODE": "1",
            "FAKE_BUILD_EVENTS_PATH": str(self.events_path),
        })

    def execute(self, command: list[str], env: dict[str, str] | None = None) -> subprocess.CompletedProcess[str]:
        result = subprocess.run(command, cwd=self.work, env=env or self.env, capture_output=True, timeout=180)
        return subprocess.CompletedProcess(command, result.returncode, result.stdout.decode("utf-8", errors="replace"), result.stderr.decode("utf-8", errors="replace"))

    def run_build(self, *args: str, metrics: bool = False, build_logs: bool = False, fail_stage: str = "", code: int = 37, break_metrics_stage: str = "") -> subprocess.CompletedProcess[str]:
        env = self.env.copy()
        if metrics:
            env["ORCA_CI_METRICS_PATH"] = str(self.metrics_path)
        if build_logs:
            env["ORCA_CI_BUILD_LOGS"] = "1"
        if fail_stage:
            env["FAKE_BUILD_FAIL_STAGE"] = fail_stage
            env["FAKE_BUILD_FAIL_CODE"] = str(code)
        if break_metrics_stage:
            env["FAKE_BUILD_BREAK_METRICS_STAGE"] = break_metrics_stage
        command = [str(Path(os.environ["SYSTEMROOT"]) / "System32" / "cmd.exe"), "/d", "/c", "build_release_vs.bat", *args]
        return self.execute(command, env)

    def run_helper(self, action: str, stage: str, *, code: int = 0, env: dict[str, str] | None = None) -> subprocess.CompletedProcess[str]:
        self.assertTrue(self.helper.is_file(), "the real metrics helper must exist")
        return self.execute([self.powershell, "-NoLogo", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File", str(self.helper), "-Action", action, "-Stage", stage, "-ExitCode", str(code)], env)

    def events(self) -> list[dict]:
        if not self.events_path.exists():
            return []
        return [json.loads(line) for line in self.events_path.read_text(encoding="utf-8-sig").splitlines() if line.strip()]

    def metrics(self) -> list[dict]:
        self.assertTrue(self.metrics_path.is_file(), "metrics JSONL was not written")
        return [json.loads(line) for line in self.metrics_path.read_text(encoding="utf-8-sig").splitlines() if line.strip()]

    def assert_success(self, result: subprocess.CompletedProcess[str]) -> None:
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def assert_record(self, record: dict, stage: str, code: int) -> None:
        self.assertEqual(record["schema"], "orca.windows-build-metrics/v1")
        self.assertEqual(record["stage"], stage)
        self.assertEqual(record["exit_code"], code)
        self.assertIsInstance(record["exit_code"], int)
        self.assertIsInstance(record["duration_seconds"], (float, int))
        self.assertGreaterEqual(record["duration_seconds"], 0)
        timestamp = dt.datetime.fromisoformat(record["start_utc"].replace("Z", "+00:00"))
        self.assertEqual(timestamp.utcoffset(), dt.timedelta(0))


class MetricsHelperTests(WindowsBuildFixture):
    def test_helper_rejects_missing_or_relative_opt_in_path(self) -> None:
        self.assertNotEqual(self.run_helper("Start", "build").returncode, 0)
        for path in ("relative-metrics.jsonl", self.work.drive + "relative-metrics.jsonl", str(self.metrics_path)[len(self.work.drive):]):
            with self.subTest(path=path):
                env = self.env | {"ORCA_CI_METRICS_PATH": path}
                self.assertNotEqual(self.run_helper("Start", "build", env=env).returncode, 0)
        self.assertFalse((self.work / "relative-metrics.jsonl").exists())
        self.assertFalse(self.metrics_path.exists())

    def test_jsonl_preserves_stage_identity_and_failure_without_replacing_previous_records(self) -> None:
        env = self.env | {
            "ORCA_CI_METRICS_PATH": str(self.metrics_path),
            "ORCA_CI_METRICS_GENERATOR": "Visual Studio 17 2022",
            "ORCA_CI_METRICS_CONFIGURATION": "RelWithDebInfo",
            "ORCA_CI_METRICS_ARCHITECTURE": "ARM64",
            "ORCA_CI_METRICS_BUILD_DIR": str(self.work / "build space 中文"),
        }
        for stage, code in (("configure", 0), ("build", 37)):
            with self.subTest(stage=stage):
                self.assert_success(self.run_helper("Start", stage, env=env))
                self.assert_success(self.run_helper("Complete", stage, code=code, env=env))
        records = self.metrics()
        self.assertEqual(len(records), 2)
        for record, stage, code in zip(records, ("configure", "build"), (0, 37)):
            self.assert_record(record, stage, code)
            self.assertEqual(record["generator"], "Visual Studio 17 2022")
            self.assertEqual(record["configuration"], "RelWithDebInfo")
            self.assertEqual(record["architecture"], "ARM64")
            self.assertEqual(Path(record["build_directory"]), self.work / "build space 中文")

    def test_interrupted_start_and_wrong_completion_preserve_unfinished_stage(self) -> None:
        env = self.env | {"ORCA_CI_METRICS_PATH": str(self.metrics_path)}
        self.assert_success(self.run_helper("Start", "configure", env=env))
        state_path = Path(str(self.metrics_path) + ".stage.json")
        original_state = state_path.read_bytes()
        self.assertNotEqual(self.run_helper("Start", "build", env=env).returncode, 0)
        self.assertNotEqual(self.run_helper("Complete", "build", env=env).returncode, 0)
        self.assertEqual(state_path.read_bytes(), original_state)
        self.assertFalse(self.metrics_path.exists())
        self.assert_success(self.run_helper("Complete", "configure", env=env))
        self.assertFalse(state_path.exists())
        self.assertEqual(len(self.metrics()), 1)

    def test_log_reference_is_captured_at_start_and_existence_at_completion(self) -> None:
        log_path = self.work / "logs space 中文" / "build.binlog"
        env = self.env | {
            "ORCA_CI_METRICS_PATH": str(self.metrics_path),
            "ORCA_CI_METRICS_BUILD_LOG_KIND": "msbuild_binlog",
            "ORCA_CI_METRICS_BUILD_LOG": str(log_path),
        }
        self.assert_success(self.run_helper("Start", "build", env=env))
        log_path.parent.mkdir()
        log_path.write_bytes(b"offline fixture")
        changed = env | {"ORCA_CI_METRICS_BUILD_LOG": str(log_path.with_name("wrong.binlog"))}
        self.assert_success(self.run_helper("Complete", "build", env=changed))
        record = self.metrics()[0]
        self.assertEqual(record["build_log_kind"], "msbuild_binlog")
        self.assertEqual(Path(record["build_log"]), log_path)
        self.assertIs(record["build_log_exists"], True)
        self.assertIs(record["includes_link"], True)


class BatchBuildMetricsTests(WindowsBuildFixture):
    def test_batch_uses_crlf_for_reliable_call_and_goto_labels(self) -> None:
        data = (ROOT / "build_release_vs.bat").read_bytes()
        self.assertNotIn(b"\n", data.replace(b"\r\n", b""), "cmd label offsets require CRLF throughout the batch script")
        self.assertFalse(data.startswith(b"\xef\xbb\xbf"), "cmd must not interpret a UTF-8 BOM as a command")

    def test_fake_executables_return_native_exit_codes_without_running_build_tools(self) -> None:
        env = self.env | {"FAKE_BUILD_FAIL_STAGE": "probe", "FAKE_BUILD_FAIL_CODE": "23"}
        result = self.execute([str(self.tools / "msbuild.exe"), "-version"], env)
        self.assertEqual(result.returncode, 23, result.stdout + result.stderr)
        self.assertIn("17.14.5", result.stdout)
        self.assertEqual(self.events()[0]["args"], ["-version"])

    def test_default_build_preserves_targets_and_has_no_telemetry(self) -> None:
        self.assert_success(self.run_build("x64", "tests"))
        events = [event for event in self.events() if event["stage"] != "probe"]
        self.assertEqual([event["stage"] for event in events], STAGES)
        self.assertFalse(self.metrics_path.exists())
        self.assertIn("-DBUILD_TESTS=ON", events[2]["args"])
        for index, target in ((1, "deps"), (3, "ALL_BUILD"), (5, "install")):
            args = events[index]["args"]
            self.assertEqual(args[args.index("--target") + 1], target)
            self.assertEqual(args[args.index("--config") + 1], "Release")
        self.assertEqual(events[1]["args"][-2:], ["--", "-m"])
        self.assertEqual(events[3]["args"][-2:], ["--", "-m"])
        self.assertEqual(events[5]["args"], ["--build", ".", "--target", "install", "--config", "Release"])
        self.assertNotIn("/bl:", json.dumps(events).lower())

    def test_each_native_failure_stops_subsequent_stages_with_and_without_metrics(self) -> None:
        for enabled in (False, True):
            for index, stage in enumerate(STAGES):
                with self.subTest(metrics=enabled, stage=stage):
                    self.events_path.unlink(missing_ok=True)
                    self.metrics_path.unlink(missing_ok=True)
                    code = 31 + index
                    result = self.run_build("x64", metrics=enabled, fail_stage=stage, code=code)
                    self.assertEqual(result.returncode, code, result.stdout + result.stderr)
                    self.assertNotIn("Build completed", result.stdout)
                    actual = [event["stage"] for event in self.events() if event["stage"] != "probe"]
                    self.assertEqual(actual, STAGES[: index + 1])
                    if enabled:
                        records = self.metrics()
                        self.assertEqual([record["stage"] for record in records], actual)
                        for record, expected_stage in zip(records, actual):
                            self.assert_record(record, expected_stage, code if expected_stage == stage else 0)
                    else:
                        self.assertFalse(self.metrics_path.exists())

    def test_msbuild_logs_require_both_opt_ins(self) -> None:
        for enabled, logs in ((False, True), (True, False), (True, True)):
            with self.subTest(metrics=enabled, build_logs=logs):
                self.events_path.unlink(missing_ok=True)
                self.metrics_path.unlink(missing_ok=True)
                self.assert_success(self.run_build("slicer", "x64", metrics=enabled, build_logs=logs))
                calls = [event for event in self.events() if event["tool"] == "cmake"]
                flags = [arg for call in calls for arg in call["args"] if arg.lower().startswith("/bl:")]
                if enabled and logs:
                    self.assertTrue(flags, "explicit build-log opt-in must produce a binary log")
                    self.assertTrue(all(";ProjectImports=None" in flag for flag in flags))
                    for flag in flags:
                        path = Path(flag[4:].split(";", 1)[0])
                        self.assertEqual(path.parent, self.metrics_path.parent)
                        self.assertTrue(path.exists())
                else:
                    self.assertEqual(flags, [])
                if enabled:
                    records = self.metrics()
                    self.assertEqual([record["stage"] for record in records], STAGES[2:])
                    for record, stage in zip(records, STAGES[2:]):
                        self.assert_record(record, stage, 0)

    def test_metrics_write_failure_never_replaces_a_native_failure_or_runs_install(self) -> None:
        for fail_stage, expected in (("build", 43), ("", 1)):
            with self.subTest(native_failure=bool(fail_stage)):
                self.events_path.unlink(missing_ok=True)
                self.metrics_path.unlink(missing_ok=True)
                result = self.run_build("slicer", "x64", metrics=True, fail_stage=fail_stage, code=43, break_metrics_stage="build")
                self.assertEqual(result.returncode, expected, result.stdout + result.stderr)
                stages = [event["stage"] for event in self.events() if event["stage"] != "probe"]
                self.assertEqual(stages, ["configure", "build"])
                self.assertNotIn("Build completed", result.stdout)

    def test_negative_metrics_start_failure_stops_before_any_native_stage(self) -> None:
        # Only this disposable helper is replaced, to simulate a failing
        # PowerShell recorder process without crashing any real process.
        self.helper.write_text("exit -7\n", encoding="ascii")
        result = self.run_build("slicer", "x64", metrics=True)
        self.assertEqual(result.returncode, (-7) & 0xFFFFFFFF, result.stdout + result.stderr)
        self.assertEqual([event for event in self.events() if event["stage"] != "probe"], [])
        self.assertNotIn("Build completed", result.stdout)

    def test_ninja_arm64_clang_debug_keeps_architecture_targets_tests_and_install(self) -> None:
        self.assert_success(self.run_build("debug", "arm64", "-x", "-l", "tests", metrics=True, build_logs=True))
        events = self.events()
        self.assertEqual([event["stage"] for event in events], STAGES)
        for index in (0, 2):
            args = events[index]["args"]
            self.assertEqual(args[args.index("-G") + 1], "Ninja Multi-Config")
            self.assertIn("-DCMAKE_C_COMPILER=clang-cl", args)
            self.assertIn("-DCMAKE_CXX_COMPILER=clang-cl", args)
            self.assertNotIn("-A", args)
            self.assertEqual(Path(events[index]["cwd"]).name, "build-dbg-arm64")
        self.assertIn("-DBUILD_TESTS=ON", events[2]["args"])
        self.assertIn("all", events[3]["args"])
        self.assertIn("install", events[5]["args"])
        self.assertNotIn("/bl:", json.dumps(events).lower())
        self.assertNotIn("-m", [arg for event in events for arg in event["args"]])
        records = self.metrics()
        self.assertEqual([record["stage"] for record in records], STAGES)
        for record, stage in zip(records, STAGES):
            self.assert_record(record, stage, 0)
            self.assertEqual(record["generator"], "Ninja Multi-Config")
            self.assertEqual(record["configuration"], "Debug")
            self.assertEqual(record["architecture"].lower(), "arm64")
        self.assertIn(".ninja_log", json.dumps(records))

    def test_deps_only_and_visual_studio_clang_keep_the_existing_toolset_boundary(self) -> None:
        self.assert_success(self.run_build("deps", "arm64", "-l", metrics=True))
        events = [event for event in self.events() if event["stage"] != "probe"]
        self.assertEqual([event["stage"] for event in events], STAGES[:2])
        args = events[0]["args"]
        self.assertEqual(args[args.index("-A") + 1].lower(), "arm64")
        self.assertNotIn("ClangCL", args)
        self.assertNotIn("-DCMAKE_C_COMPILER=clang-cl", args)
        self.assertEqual([record["stage"] for record in self.metrics()], STAGES[:2])
        self.events_path.unlink()
        self.metrics_path.unlink()
        self.assert_success(self.run_build("slicer", "debuginfo", "-l", metrics=True))
        events = [event for event in self.events() if event["stage"] != "probe"]
        args = events[0]["args"]
        self.assertEqual(args[args.index("-T") + 1], "ClangCL")
        self.assertIn("-DCMAKE_BUILD_TYPE=RelWithDebInfo", args)
        self.assertIn("-DBUILD_TESTS=OFF", args)
        self.assertEqual(Path(events[0]["cwd"]).name, "build-dbginfo")


if __name__ == "__main__":
    unittest.main()
