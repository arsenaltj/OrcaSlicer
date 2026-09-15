"""Exercise the Windows packager with real subprocesses and fake native tools.

No native Orca build, installed application, SDK, credentials or network is used.
The fixture compiles a tiny C# command recorder using the Windows framework
compiler. The real MSIX PowerShell script stages disposable fixture files.
"""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import unittest


SOURCE = Path(__file__).resolve().parent
PWSH = shutil.which("pwsh.exe")
CSC = Path(os.environ.get("WINDIR", "C:/Windows")) / "Microsoft.NET/Framework64/v4.0.30319/csc.exe"
FAKE_NATIVE = r'''
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Threading;
using System.Web.Script.Serialization;
class FakePackageTool {
    static JavaScriptSerializer Json = new JavaScriptSerializer();
    static string Env(string name) { return Environment.GetEnvironmentVariable(name) ?? ""; }
    static string After(string[] args, string flag) { return args[Array.IndexOf(args, flag) + 1]; }
    static void Trace(string phase, string kind, string[] args) {
        string directory = Env("FAKE_PACKAGE_TRACE");
        if (directory == "") return;
        Directory.CreateDirectory(directory);
        long ticks = DateTime.UtcNow.Ticks;
        string name = ticks.ToString() + "_" + Process.GetCurrentProcess().Id + "_" + phase + ".json";
        File.WriteAllText(Path.Combine(directory, name), Json.Serialize(new {
            phase = phase, kind = kind, args = args, cwd = Directory.GetCurrentDirectory(),
            ticks = ticks, pid = Process.GetCurrentProcess().Id
        }));
    }
    static string[] Files(string root, string pattern, SearchOption option) {
        return Directory.GetFiles(root, pattern, option).Select(p => p.Substring(root.Length).TrimStart('\\', '/').Replace('\\','/')).OrderBy(p => p).ToArray();
    }
    static int Main(string[] args) {
        string exe = Process.GetCurrentProcess().MainModule.FileName;
        string kind = Path.GetFileNameWithoutExtension(exe).ToLowerInvariant();
        if (args.Length > 0 && args[0] == "--linger") {
            Trace("start", "descendant", args); Thread.Sleep(120000); return 0;
        }
        if (kind == "7z") kind = args.Contains("-tzip") ? "portable-zip" : "pdb";
        if (kind == "cpack") kind = "nsis";
        if (kind == "makeappx") kind = "msix";
        Trace("start", kind, args);
        Console.WriteLine("native stdout " + kind);
        Console.Error.WriteLine("native stderr " + kind);
        if (Env("FAKE_PACKAGE_BARRIER") == "1" && (kind == "nsis" || kind == "pdb")) {
            string other = kind == "nsis" ? "pdb" : "nsis";
            DateTime deadline = DateTime.UtcNow.AddSeconds(60);
            bool ready = false;
            while (!ready && DateTime.UtcNow < deadline) {
                foreach (string record in Directory.GetFiles(Env("FAKE_PACKAGE_TRACE"), "*_start.json")) {
                    try { if (File.ReadAllText(record).Contains("\"kind\":\"" + other + "\"")) ready = true; }
                    catch (IOException) { }
                }
                if (!ready) Thread.Sleep(25);
            }
            if (!ready) return 31;
        }
        if (Env("FAKE_PACKAGE_CHILD") == kind) {
            var start = new ProcessStartInfo(exe, "--linger");
            start.UseShellExecute = false; start.CreateNoWindow = true;
            Process.Start(start);
        }
        int delay;
        if (!Int32.TryParse(Env("FAKE_PACKAGE_DELAY_MS"), out delay)) delay = 700;
        if (Env("FAKE_PACKAGE_FAIL") == kind) delay = 150;
        Thread.Sleep(delay);
        if (Env("FAKE_PACKAGE_FAIL") == kind) { Trace("failed", kind, args); return 29; }
        if (Env("FAKE_PACKAGE_MUTATE") == kind) File.AppendAllText(Path.Combine(Env("FAKE_PACKAGE_BUILD"), "OrcaSlicer", "orca-slicer.exe"), "mutated");
        if (Env("FAKE_PACKAGE_MISSING") != kind) {
            string output;
            object contents;
            if (kind == "nsis") {
                string directory = After(args, "-B"); Directory.CreateDirectory(directory);
                output = Path.Combine(directory, "OrcaSlicer_Windows_Installer_V1_x64.exe");
                contents = Files(Path.Combine(Directory.GetCurrentDirectory(), "OrcaSlicer"), "*", SearchOption.AllDirectories);
            } else if (kind == "msix") {
                output = After(args, "/p");
                contents = Files(After(args, "/d"), "*", SearchOption.AllDirectories);
            } else {
                output = args.First(p => p.EndsWith(".7z") || p.EndsWith(".zip"));
                contents = kind == "pdb" ? Files(Directory.GetCurrentDirectory(), "*.pdb", SearchOption.TopDirectoryOnly) : Files(args.Last(), "*", SearchOption.AllDirectories);
            }
            Directory.CreateDirectory(Path.GetDirectoryName(output));
            File.WriteAllText(output, Json.Serialize(new { kind = kind, contents = contents }));
        }
        Trace("end", kind, args);
        return 0;
    }
}
'''


@unittest.skipUnless(os.name == "nt" and PWSH and CSC.is_file(), "requires Windows, PowerShell 7 and framework C# compiler")
class WindowsPackagingTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.native_temp = tempfile.TemporaryDirectory(prefix="orca-packager-native-")
        cls.native = Path(cls.native_temp.name)
        source = cls.native / "fake.cs"
        source.write_text(FAKE_NATIVE, encoding="utf-8")
        cls.fake = cls.native / "fake.exe"
        result = subprocess.run(
            [str(CSC), "/nologo", "/r:System.Web.Extensions.dll", f"/out:{cls.fake}", str(source)],
            capture_output=True, text=True, timeout=60,
        )
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    @classmethod
    def tearDownClass(cls):
        cls.native_temp.cleanup()

    def setUp(self):
        # A PATH entry cannot contain ';' on Windows; the actual build/output
        # arguments below still exercise semicolons and shell metacharacters.
        self.temp = tempfile.TemporaryDirectory(prefix="orca packaging & ")
        self.root = Path(self.temp.name)
        self.repo = self.root / "repo"
        self.scripts = self.repo / "scripts"
        self.scripts.mkdir(parents=True)
        self.script = self.scripts / "ci_windows_packaging.ps1"
        shutil.copy2(SOURCE / self.script.name, self.script)
        shutil.copytree(SOURCE / "msix", self.scripts / "msix")
        (self.repo / "version.inc").write_text('set(SoftFever_VERSION "1.2.3")\n', encoding="utf-8")
        self.build = self.repo / "build ;& spaces"
        (self.build / "OrcaSlicer/resources").mkdir(parents=True)
        (self.build / "OrcaSlicer/orca-slicer.exe").write_bytes(b"fixture app, never executed")
        (self.build / "OrcaSlicer/resources/profiles.zip").write_bytes(b"fixture profiles")
        (self.build / "OrcaSlicer/data with spaces.txt").write_text("fixture resource", encoding="utf-8")
        (self.build / "src/Release").mkdir(parents=True)
        for name in ("orca-slicer.pdb", "debug with spaces.pdb"):
            (self.build / "src/Release" / name).write_bytes(name.encode())
        (self.build / "CPackConfig.cmake").write_text("# fixture CPack config\n", encoding="utf-8")
        self.tools = self.root / "tools"
        self.tools.mkdir()
        for name in ("cpack.exe", "7z.exe"):
            shutil.copy2(self.fake, self.tools / name)
        sdk = self.root / "sdk/Windows Kits/10/bin/10.0.99999.0/x64"
        sdk.mkdir(parents=True)
        shutil.copy2(self.fake, sdk / "makeappx.exe")
        self.env = dict(os.environ)
        for key in tuple(self.env):
            if key.startswith("FAKE_PACKAGE_"):
                del self.env[key]
        self.env.update({
            "PATH": str(self.tools) + os.pathsep + self.env.get("PATH", ""),
            "ProgramFiles(x86)": str(self.root / "sdk"),
            "PROCESSOR_ARCHITECTURE": "AMD64",
            "FAKE_PACKAGE_BUILD": str(self.build),
            "FAKE_PACKAGE_TRACE": str(self.root / "traces"),
        })
        self.metrics = self.root / "metrics.json"

    def tearDown(self):
        self.temp.cleanup()

    def command(self, *extra):
        return [str(PWSH), "-NoProfile", "-NonInteractive", "-File", str(self.script),
                "-BuildDir", str(self.build), "-Version", "PR-123", "-MetricsPath", str(self.metrics), *map(str, extra)]

    def run_package(self, *extra, expected=0):
        result = subprocess.run(self.command(*extra), env=self.env, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=120)
        if expected == 0:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def report(self):
        return json.loads(self.metrics.read_text(encoding="utf-8-sig"))

    def traces(self):
        return [json.loads(p.read_text(encoding="utf-8-sig")) for p in sorted((self.root / "traces").glob("*.json"))]

    def test_dry_run_is_read_only_and_uses_isolated_outputs(self):
        before = sorted(str(p.relative_to(self.root)) for p in self.root.rglob("*"))
        result = self.run_package("-DryRun")
        plan = json.loads(result.stdout)
        self.assertEqual([job["name"] for job in plan["jobs"]], ["nsis", "pdb", "msix"])
        self.assertEqual(plan["max_parallel"], 2)
        self.assertTrue(all(str(self.build / ".ci-windows-packaging") in job["stdout"] for job in plan["jobs"]))
        self.assertIn("-mx3", plan["jobs"][1]["arguments"])
        self.assertEqual(before, sorted(str(p.relative_to(self.root)) for p in self.root.rglob("*")))

    def test_profiles_preserve_inputs_and_outputs_and_limit_parallelism(self):
        reports = []
        for profile in ("baseline", "optimized"):
            self.metrics = self.root / f"{profile}.json"
            self.env["FAKE_PACKAGE_TRACE"] = str(self.root / profile / "traces")
            self.env["FAKE_PACKAGE_BARRIER"] = "1" if profile == "optimized" else "0"
            self.run_package("-Profile", profile, "-OutputDirectory", self.root / profile / "outputs")
            report = self.report()
            reports.append(report)
            self.assertEqual(report["status"], "succeeded")
            self.assertTrue(report["inputs_unchanged"])
            self.assertTrue(all(job["status"] == "succeeded" for job in report["jobs"]))
            for artifact in report["artifacts"]:
                path = Path(artifact["path"])
                self.assertEqual(hashlib.sha256(path.read_bytes()).hexdigest(), artifact["sha256"])
                contents = json.loads(path.read_text())["contents"]
                self.assertTrue(contents)
                if artifact["kind"] == "pdb":
                    self.assertEqual(contents, ["debug with spaces.pdb", "orca-slicer.pdb"])
            events = [json.loads(p.read_text()) for p in (self.root / profile / "traces").glob("*.json")]
            active = maximum = 0
            for event in sorted(events, key=lambda event: event["ticks"]):
                active += 1 if event["phase"] == "start" else -1
                maximum = max(maximum, active)
            self.assertEqual(maximum, 1 if profile == "baseline" else 2)
            pdb = next(job for job in report["jobs"] if job["name"] == "pdb")
            self.assertIn("-mx9" if profile == "baseline" else "-mx3", pdb["arguments"])
        baseline, optimized = reports
        self.assertEqual(baseline["input_manifest"], optimized["input_manifest"])
        self.assertEqual(baseline["msix_staging_manifest"], optimized["msix_staging_manifest"])
        self.assertEqual({a["kind"] for a in baseline["artifacts"]}, {"nsis", "pdb", "msix", "unused-portable-zip"})
        self.assertEqual({a["kind"] for a in optimized["artifacts"]}, {"nsis", "pdb", "msix"})
        self.assertFalse(list((self.root / "optimized/outputs").glob("*.zip")))

    def test_default_pdb_path_matches_existing_upload_contract(self):
        self.run_package()
        report = self.report()
        pdb = next(a for a in report["artifacts"] if a["kind"] == "pdb")
        self.assertEqual(Path(pdb["path"]), self.build / "src/Release/Debug_PDB_PR-123_for_developers_only.7z")
        msix = next(a for a in report["artifacts"] if a["kind"] == "msix")
        self.assertEqual(Path(msix["path"]), self.build / "OrcaSlicer_Windows_MSIX_PR-123_x64.msix")

    def test_arm64_skips_pdb_without_requiring_debug_directory(self):
        shutil.rmtree(self.build / "src")
        self.run_package("-Architecture", "arm64")
        report = self.report()
        self.assertEqual([job["name"] for job in report["jobs"]], ["nsis", "msix"])
        self.assertEqual({a["kind"] for a in report["artifacts"]}, {"nsis", "msix"})
        self.assertIn("arm64.msix", next(a for a in report["artifacts"] if a["kind"] == "msix")["path"])

    def test_native_failure_stops_running_jobs_and_does_not_publish(self):
        self.env.update({"FAKE_PACKAGE_FAIL": "nsis", "FAKE_PACKAGE_DELAY_MS": "15000"})
        self.run_package(expected=1)
        report = self.report()
        self.assertEqual(report["status"], "failed")
        self.assertIn("exit code 29", report["error"])
        self.assertEqual(report["artifacts"], [])
        self.assertEqual([j["status"] for j in report["jobs"]], ["failed", "terminated", "pending"])
        self.assertIn("native stdout nsis", Path(report["jobs"][0]["stdout"]).read_text())
        self.assertFalse(list(self.build.glob("OrcaSlicer*.exe")))

    def test_process_start_failure_still_stops_sibling_and_writes_metrics(self):
        (self.tools / "7z.exe").write_bytes(b"not an executable")
        self.env["FAKE_PACKAGE_DELAY_MS"] = "15000"
        self.run_package(expected=1)
        report = self.report()
        self.assertEqual(report["status"], "failed")
        self.assertEqual([job["status"] for job in report["jobs"]], ["terminated", "start_failed", "pending"])
        self.assertEqual(report["cleanup_errors"], [])
        self.assertEqual(report["artifacts"], [])

    def test_success_exit_without_required_artifact_fails(self):
        self.env["FAKE_PACKAGE_MISSING"] = "msix"
        self.run_package(expected=1)
        self.assertEqual(self.report()["status"], "failed")
        self.assertIn("Missing or empty output for msix", self.report()["error"])
        self.assertEqual(self.report()["artifacts"], [])

    def test_changed_inputs_invalidate_measurement(self):
        self.env["FAKE_PACKAGE_MUTATE"] = "nsis"
        self.run_package(expected=1)
        self.assertFalse(self.report()["inputs_unchanged"])
        self.assertEqual(self.report()["artifacts"], [])

    def test_existing_artifact_is_never_overwritten(self):
        target = self.build / "OrcaSlicer_Windows_MSIX_PR-123_x64.msix"
        target.write_bytes(b"previous package")
        self.run_package(expected=1)
        self.assertEqual(target.read_bytes(), b"previous package")
        self.assertIn("Output already exists", self.report()["error"])
        self.assertEqual(self.report()["artifacts"], [])

    def test_invalid_arguments_and_missing_pdb_fail_before_writes(self):
        before = set(self.build.rglob("*"))
        for extra in (("-Version", "../../escape"), ("-Architecture", "x86"), ("-BuildDir", "relative"), ("-Profile", "unknown")):
            with self.subTest(extra=extra):
                self.run_package(*extra, expected=1)
                self.assertFalse(self.metrics.exists())
                self.assertEqual(set(self.build.rglob("*")), before)
        for pdb in (self.build / "src/Release").glob("*.pdb"):
            pdb.unlink()
        result = self.run_package(expected=1)
        self.assertIn("no PDB files", result.stderr)
        self.assertFalse(self.metrics.exists())

    def test_arguments_are_literal_not_shell_code(self):
        marker = self.root / "must-not-exist"
        value = "Display $(New-Item '" + str(marker) + "') ; literal"
        self.run_package("-PublisherDisplayName", value)
        stage = Path(self.report()["invocation_directory"]) / "msix-staging/AppxManifest.xml"
        self.assertIn(value, stage.read_text(encoding="utf-8-sig"))
        self.assertFalse(marker.exists())

    def assert_processes_stopped(self, events):
        pids = [event["pid"] for event in events if event["phase"] == "start"]
        if not pids:
            return
        check = subprocess.run([str(PWSH), "-NoProfile", "-NonInteractive", "-Command", "@(Get-Process -Id " + ",".join(map(str, pids)) + " -ErrorAction SilentlyContinue).Count"], capture_output=True, text=True, timeout=30)
        self.assertEqual(check.stdout.strip(), "0")

    def test_timeout_stops_started_children(self):
        self.env["FAKE_PACKAGE_DELAY_MS"] = "120000"
        self.run_package("-TimeoutSeconds", "8", expected=1)
        report = self.report()
        self.assertEqual(report["status"], "timed_out")
        self.assert_processes_stopped(self.traces())
        self.assertEqual(report["artifacts"], [])

    def test_sentinel_cancels_a_live_invocation_and_its_descendants(self):
        cancel = self.root / "cancel"
        self.env.update({"FAKE_PACKAGE_DELAY_MS": "120000", "FAKE_PACKAGE_CHILD": "nsis"})
        process = subprocess.Popen(self.command("-CancellationFile", cancel), env=self.env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, encoding="utf-8", errors="replace")
        try:
            deadline = time.monotonic() + 60
            while not any(event["kind"] == "descendant" for event in self.traces()) and process.poll() is None and time.monotonic() < deadline:
                time.sleep(0.05)
            self.assertTrue(any(event["kind"] == "descendant" for event in self.traces()), "must actually start a native descendant before cancellation")
            cancel.write_text("cancel", encoding="utf-8")
            stdout, stderr = process.communicate(timeout=30)
            self.assertNotEqual(process.returncode, 0, stdout + stderr)
            self.assertEqual(self.report()["status"], "cancelled")
            self.assertEqual(self.report()["artifacts"], [])
            self.assert_processes_stopped(self.traces())
        finally:
            if process.poll() is None:
                subprocess.run(["taskkill.exe", "/PID", str(process.pid), "/T", "/F"], capture_output=True, timeout=10)
                process.communicate(timeout=10)


if __name__ == "__main__":
    unittest.main()
