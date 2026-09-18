"""Bounded regressions for Git identity, deleted dependencies and report safety."""
from pathlib import Path
import tempfile
import unittest

import yaml

import architecture_review as ar


class ArchitectureReviewTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="orca-architecture-test-")
        self.root = Path(self.temp.name).resolve()
        self.addCleanup(self.temp.cleanup)
        self.git("init", "-q")
        self.git("config", "user.name", "Local Test")
        self.git("config", "user.email", "test@example.invalid")

    def git(self, *args):
        return ar.git(self.root, *args).decode().strip()

    def commit(self):
        self.git("add", ".")
        self.git("commit", "-qm", "fixture")
        return self.git("rev-parse", "HEAD")

    def test_readme_sync_preserves_other_content_and_is_idempotent(self):
        config = ar.load_map(Path(__file__).resolve().parents[1])
        path = self.root / "README.md"
        original = "# Existing project\r\n\r\nKeep this content.\r\n".encode()
        path.write_bytes(original)
        self.assertTrue(ar.sync_readme(self.root, config))
        generated = path.read_bytes()
        self.assertTrue(generated.endswith(original))
        self.assertFalse(ar.sync_readme(self.root, config))
        self.assertFalse(ar.sync_readme(self.root, config, check=True))
        self.assertEqual(path.read_bytes(), generated)
        # A changed business capability must invalidate README without writing in CI.
        config["product_overview"]["planned"]["slicing"][0]["label"] = "Changed capability"
        with self.assertRaisesRegex(ValueError, "stale"):
            ar.sync_readme(self.root, config, check=True)
        self.assertEqual(path.read_bytes(), generated)
        self.assertTrue(ar.sync_readme(self.root, config))
        self.assertTrue(path.read_bytes().endswith(original))
        self.assertEqual(path.read_text(encoding="utf-8").count(ar.README_START), 1)

    def test_readme_rejects_damaged_markers_without_overwriting(self):
        config = ar.load_map(Path(__file__).resolve().parents[1])
        path = self.root / "README.md"
        for content in [ar.README_START, ar.README_END,
                        ar.README_END + ar.README_START,
                        ar.README_START * 2 + ar.README_END]:
            path.write_text(content, encoding="utf-8")
            with self.assertRaises(ValueError):
                ar.sync_readme(self.root, config)
            self.assertEqual(path.read_text(encoding="utf-8"), content)

    def test_staged_unstaged_untracked_and_deleted_are_reported(self):
        for name in ["a.py", "b.py", "delete.py"]:
            (self.root / name).write_text("pass\n")
        head = self.commit()
        (self.root / "a.py").write_text("x = 1\n")
        self.git("add", "a.py")
        (self.root / "b.py").write_text("x = 2\n")
        (self.root / "delete.py").unlink()
        (self.root / "new space.py").write_text("pass\n")
        changes = {c["path"]: c["status"] for c in ar.change_list(self.root, head, head, True)}
        self.assertEqual(changes, {"a.py": "M", "b.py": "M", "delete.py": "D", "new space.py": "A"})
        self.assertEqual(ar.change_list(self.root, head, head), [])

    def test_git_blobs_are_exact_even_with_binary_and_newlines(self):
        data = b"one\x00two\r\n"
        (self.root / "blob").write_bytes(data)
        head = self.commit()
        self.assertEqual(ar.read_blobs(self.root, ar.tree(self.root, head), ["blob"]), {"blob": data})

    def test_module_specific_paths_override_broad_ownership(self):
        config = {"modules": [{"id": "desktop", "paths": ["src/GUI"]}, {"id": "adapter", "paths": ["src/GUI/AI/Orca"]}]}
        self.assertEqual(ar.module_for("src/GUI/AI/Orca/Adapter.cpp", config), "adapter")
        self.assertEqual(ar.module_for("unmapped.bin", config), "unknown")

    def test_deleted_dependency_keeps_old_consumers_visible(self):
        config = {"modules": [{"id": "lib", "label": "库", "paths": ["lib"]}, {"id": "app", "label": "应用", "paths": ["app"]}]}
        baseline = {"edges": [{"source": "app/a.py", "target": "lib/b.py", "kind": "CALLS"}], "files": ["app/a.py", "lib/b.py"], "identity": "a", "cache_hit": False}
        current = {"edges": [], "files": ["app/a.py"], "identity": "b", "cache_hit": False, "node_count": 1, "unresolved_edges": 0}
        report = ar.make_report(config, baseline, current, [{"path": "lib/b.py", "status": "D"}], {})
        self.assertEqual(report["potential_files"], ["app/a.py"])
        self.assertEqual(report["relationships"][0]["status"], "removed")

    def test_impact_is_directional_and_bounded(self):
        graph = {"edges": [{"source": "a", "target": "b", "kind": "CALLS"}, {"source": "b", "target": "c", "kind": "IMPORTS_FROM"}, {"source": "z", "target": "a", "kind": "CALLS"}]}
        self.assertEqual(ar.impact(graph, {"c"}), {"b", "a"})
        self.assertEqual(ar.impact(graph, {"a"}), {"z"})

    def test_paths_cannot_escape_snapshot(self):
        for path in ["../escape.py", "/tmp/a.py", "C:/x.py", ".git/config", "a\\b"]:
            with self.assertRaises(ValueError):
                ar.valid_path(path)

    def test_ambiguous_include_stays_unresolved(self):
        files = {"one/common.hpp", "two/common.hpp"}
        self.assertIsNone(ar.include_target("caller.cpp", "common.hpp", files))
        self.assertEqual(ar.include_target("one/caller.cpp", "common.hpp", files), "one/common.hpp")

    def test_html_cannot_execute_a_filename(self):
        document = ar.render_html({"path": "</script><script>alert(1)</script>"})
        self.assertNotIn("</script><script>alert(1)", document)
        self.assertIn("\\u003c/script", document)

    def test_workflow_publishes_only_report_files_without_executing_pr_code(self):
        workflow = Path(__file__).resolve().parents[1] / ".github/workflows/architecture-impact.yml"
        config = yaml.load(workflow.read_text(encoding="utf-8"), Loader=yaml.BaseLoader)
        self.assertNotIn("pull_request_target", config["on"])
        self.assertEqual(config["permissions"], {"contents": "read"})
        self.assertNotIn("paths", config["on"]["pull_request"])
        self.assertTrue(any("--readme check" in step.get("run", "")
                            for step in config["jobs"]["analyze"]["steps"]))
        upload = next(step for step in config["jobs"]["analyze"]["steps"] if "upload-artifact" in step.get("uses", ""))["with"]
        self.assertEqual(upload["include-hidden-files"], "true")
        expected = ["index.html", "details.html", "archify-receipts.json", "report.json", "summary.md"]
        expected += [view + suffix for view in ["l0-context", "l1-modules", "l2-generation", "l2-slicing"] for suffix in [".html", ".json"]]
        self.assertEqual(set(upload["path"].split()), {f".tmp/architecture-review/report/{name}" for name in expected})
        publish = config["jobs"]["publish"]
        self.assertFalse(any("run" in step or "checkout" in step.get("uses", "") for step in publish["steps"]))

    def test_archify_never_invents_edges_and_discloses_omitted_relations(self):
        report = {"modules": [{"id": key, "label": key, "changed": 1, "potential": 0} for key in ["generation", "contracts", "desktop"]],
                  "relationships": [{"source": "generation", "target": "contracts", "status": "removed"},
                                    {"source": "generation", "target": "desktop", "status": "added"}],
                  "changes": [], "potential_files": [], "mode": "PR 提交", "base": "a" * 40, "head": "b" * 40}
        spec = ar.archify_spec(report)
        self.assertEqual(len(spec["connections"]), 1)
        edge = spec["connections"][0]
        self.assertEqual((edge["from"], edge["to"], edge["label"], edge["variant"]), ("generation", "contracts", "移除依赖", "dashed"))
        self.assertIn("其余 1 条", spec["cards"][1]["items"][0])
        self.assertIn("新增 1 · 移除 1", spec["cards"][1]["items"][1])
        report["relationships"] = []
        self.assertEqual(ar.archify_spec(report)["connections"], [])

    def test_archify_setup_rejects_tampered_archive_before_extracting(self):
        folder = self.root / ".tmp/architecture-review"
        folder.mkdir(parents=True)
        (folder / f"archify-{ar.ARCHIFY_VERSION}.zip").write_bytes(b"not-the-official-archive")
        with self.assertRaisesRegex(RuntimeError, "checksum mismatch"):
            ar.setup_archify(self.root)
        self.assertFalse(ar.archify_home(self.root).exists())

    def test_python_state_vocabulary_is_not_imported_or_taken_from_comments(self):
        data = b'import nonexistent_application\nstate: str = "preprocessing"\n# job.state = "fake"\njob.state = "running"\nif job.state in {"failed", "stopped"}:\n    pass\n'
        states = ar.extract_states(data, "python_job_state")
        self.assertEqual(set(states), {"preprocessing", "running", "failed", "stopped"})
        self.assertEqual(states["running"], 4)

    def test_business_preserves_new_removed_states_and_missing_evidence(self):
        config = {"modules": [{"id": "slicing", "paths": ["workflow.hpp"]}],
                  "evidence": [{"id": "states", "path": "workflow.hpp", "needle": "enum class WorkflowState"},
                               {"id": "missing", "path": "missing.cpp", "needle": "absent"}],
                  "journeys": [{"id": "slicing", "state_source": "states", "extractor": "cpp_workflow_enum", "state_meanings": {"Idle": "就绪"}, "phases": []}]}
        report = {"changes": [{"path": "workflow.hpp"}]}
        ar.enrich_business(report, config, {"workflow.hpp": b'enum class WorkflowState { Idle, Old };'},
                           {"workflow.hpp": b'enum class WorkflowState { Idle, New };'})
        states = {s["name"]: s for s in report["business"]["journeys"][0]["states"]}
        self.assertEqual(states["New"]["delta"], "added")
        self.assertFalse(states["New"]["known"])
        self.assertEqual(states["Old"]["delta"], "removed")
        self.assertFalse(report["business"]["evidence"][1]["found"])
        self.assertTrue(report["business"]["evidence"][0]["source_changed"])

    def test_business_evidence_uses_requested_commit_not_dirty_source(self):
        (self.root / "state.py").write_text('job.state = "old"\n')
        head = self.commit()
        (self.root / "state.py").write_text('job.state = "new"\n')
        config = {"evidence": [{"path": "state.py"}]}
        self.assertIn(b'"old"', ar.evidence_sources(self.root, head, config)["state.py"])
        self.assertIn(b'"new"', ar.evidence_sources(self.root, head, config, True)["state.py"])

    def test_business_impact_is_file_scoped_and_keeps_potential_distinct(self):
        config = {"modules": [{"id": "gen", "paths": ["gen"]}],
                  "evidence": [{"id": "state", "path": "gen/state.py", "needle": "job.state"},
                               {"id": "other", "path": "gen/other.py", "needle": "x"}],
                  "journeys": [{"id": "gen", "state_source": "state", "extractor": "python_job_state", "state_meanings": {},
                                "phases": [{"id": "unchanged", "states": [], "evidence": ["state"]},
                                           {"id": "affected", "states": [], "evidence": ["other"]}]}],
                  "business_flows": [{"id": "gen", "modules": ["gen"], "evidence": ["state"]}]}
        report = {"changes": [{"path": "gen/deleted.py", "module": "gen", "status": "D"}],
                  "potential_files": ["gen/other.py"]}
        source = {"gen/state.py": b'job.state = "ready"', "gen/other.py": b'x = 1'}
        ar.enrich_business(report, config, source, source)
        self.assertEqual(report["business"]["flows"][0]["impact"]["kind"], "changed")
        phases = report["business"]["journeys"][0]["phases"]
        self.assertEqual(phases[0]["impact"]["kind"], "unchanged")
        self.assertEqual(phases[1]["impact"]["kind"], "potential")
        self.assertEqual(ar.path_impact(report, ["gen/deleted.py"])["changed"], ["gen/deleted.py"])
        config["business_flows"][0]["impact_scope"] = "evidence"
        ar.enrich_business(report, config, source, source)
        self.assertEqual(report["business"]["flows"][0]["impact"]["kind"], "unchanged")

    def test_branch_inventory_excludes_removed_and_unobserved_states(self):
        journey = {"label": "流程", "phases": [], "rules": [{"title": "条件"}, {"title": "恢复"}],
                   "states": [{"name": "Waiting", "delta": "existing"}, {"name": "Old", "delta": "removed"}],
                   "state_groups": [{"id": "waiting", "label": "支线", "states": [
                       {"name": name, "type": "neutral", "label": name} for name in ["Waiting", "Old", "Absent"]]}]}
        spec = ar.journey_spec({"business": {"evidence": []}}, journey)
        self.assertEqual([state["sublabel"] for state in spec["states"]], ["Waiting"])
        self.assertEqual(spec["transitions"], [])

    def test_real_crg_cpp_and_python_relationships_and_cache(self):
        config = {"identity": "fixture"}
        blobs = {"mod.py": b"def target():\n    return 1\n", "use.py": b"from mod import target\ndef caller():\n    return target()\n", "lib.hpp": b"int target();\n", "use.cpp": b'#include "lib.hpp"\nint caller() { return target(); }\n'}
        cache = self.root / "cache"
        first = ar.analyze(blobs, config, cache)
        second = ar.analyze(blobs, config, cache)
        self.assertGreater(first["node_count"], len(blobs))
        self.assertTrue(any(e["source"] == "use.py" and e["target"] == "mod.py" for e in first["edges"]))
        self.assertTrue(any(e["source"] == "use.cpp" and e["target"] == "lib.hpp" for e in first["edges"]))
        self.assertTrue(second["cache_hit"])
        revised = ar.analyze({k: v for k, v in blobs.items() if k != "mod.py"}, config, cache)
        self.assertNotEqual(revised["identity"], first["identity"])
        self.assertNotIn("mod.py", revised["files"])


if __name__ == "__main__":
    unittest.main()
