import json
from pathlib import Path
import tempfile
import unittest

import yaml
import architecture_site as site


class ArchitectureSiteTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.report = self.root / "report"
        self.report.mkdir()
        self.output = self.root / "site"
        self.args = dict(pr=14, run=123, head="a" * 40, base="b" * 40,
                         repository="arsenaltj/OrcaSlicer", title='<script>alert("x")</script>')
        for name in site.FILES:
            (self.report / name).write_text("placeholder", encoding="utf-8")
        (self.report / "report.json").write_text(json.dumps(dict(schema=1, head="a" * 40, base_tip="b" * 40, mode="PR 提交")), encoding="utf-8")

    def build(self, **changes):
        return site.assemble(self.report, self.output, **{**self.args, **changes})

    def test_version_history_sandbox_and_only_allowlisted_files(self):
        (self.report / "credential.txt").write_text("do not copy")
        key = self.build()
        self.assertFalse((self.output / key / "view/credential.txt").exists())
        wrapper = (self.output / key / "index.html").read_text(encoding="utf-8")
        self.assertIn('sandbox="allow-scripts allow-downloads"', wrapper)
        self.assertNotIn("allow-same-origin", wrapper)
        self.assertNotIn('<script>alert', (self.output / "index.html").read_text(encoding="utf-8"))
        self.assertEqual(self.build(), key)
        self.build(run=124)
        self.assertEqual(len(json.loads((self.output / "history.json").read_text(encoding="utf-8"))), 2)
        self.assertTrue((self.output / key / "index.html").exists())
        self.assertIn("/124/", (self.output / "pr/14/index.html").read_text(encoding="utf-8"))
        self.build(run=122)
        self.assertIn("/124/", (self.output / "pr/14/index.html").read_text(encoding="utf-8"))

    def test_source_mismatch_and_changed_existing_version_are_rejected(self):
        with self.assertRaisesRegex(ValueError, "identity mismatch"):
            self.build(head="c" * 40)
        self.assertFalse(self.output.exists())
        self.build()
        (self.report / "index.html").write_text("changed")
        with self.assertRaisesRegex(ValueError, "immutable"):
            self.build()

    def test_invalid_identity_and_missing_files_are_rejected(self):
        with self.assertRaises(ValueError): self.build(head="../../escape")
        with self.assertRaises(ValueError): self.build(pr=0)
        (self.report / "details.html").unlink()
        with self.assertRaisesRegex(ValueError, "Missing"):
            self.build()
        self.assertFalse(self.output.exists())

    def test_publisher_uses_default_branch_code_and_separate_archive(self):
        path = Path(__file__).resolve().parents[1] / ".github/workflows/architecture-pages.yml"
        workflow = yaml.load(path.read_text(encoding="utf-8"), Loader=yaml.BaseLoader)
        self.assertEqual(set(workflow["on"]), {"workflow_run"})
        steps = workflow["jobs"]["publish"]["steps"]
        checkouts = [s["with"] for s in steps if "checkout@" in s.get("uses", "")]
        self.assertEqual([c["ref"] for c in checkouts], ["${{ github.workflow_sha }}", "codex/architecture-pages"])
        self.assertEqual(workflow["concurrency"]["cancel-in-progress"], "false")
        self.assertIn("run-id", next(s for s in steps if "download-artifact@" in s.get("uses", ""))["with"])


if __name__ == "__main__":
    unittest.main()
