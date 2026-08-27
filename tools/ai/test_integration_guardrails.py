from __future__ import annotations

import copy
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT_PATH = REPO_ROOT / "scripts" / "verify_ai_integration.py"
LOCK_PATH = REPO_ROOT / "docs" / "architecture" / "ai-integration-lock.json"
SPEC = importlib.util.spec_from_file_location("verify_ai_integration", SCRIPT_PATH)
assert SPEC is not None and SPEC.loader is not None
GUARDRAILS = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GUARDRAILS)


class IntegrationGuardrailTests(unittest.TestCase):
    def setUp(self) -> None:
        self.document = json.loads(LOCK_PATH.read_text(encoding="utf-8"))

    def test_repository_lock_passes_schema_and_source_checks(self) -> None:
        self.assertEqual([], GUARDRAILS.validate_document(self.document))
        self.assertEqual([], GUARDRAILS.validate_source_constants(self.document, REPO_ROOT))

    def test_rejects_short_or_changed_feature_sha(self) -> None:
        document = copy.deepcopy(self.document)
        document["feature_sources"]["model_generation"]["sha"] = "db81edc2"

        errors = GUARDRAILS.validate_document(document)

        self.assertTrue(any(error["code"] == "contract.constant" for error in errors))
        self.assertTrue(any(error["code"] == "schema.sha" for error in errors))

    def test_rejects_changed_upstream_pin(self) -> None:
        document = copy.deepcopy(self.document)
        document["upstream"]["sha"] = "0" * 40

        errors = GUARDRAILS.validate_document(document)

        self.assertTrue(any("upstream.sha" in error["message"] for error in errors))

    def test_rejects_unsafe_or_overlapping_boundary_paths(self) -> None:
        document = copy.deepcopy(self.document)
        document["boundaries"]["model_generation_owned_paths"].append("../outside")
        document["boundaries"]["smart_slicing_owned_paths"].append("tools/ai/orca_ai_sidecar.py")

        errors = GUARDRAILS.validate_document(document)

        self.assertTrue(any(error["code"] == "boundary.path" for error in errors))
        self.assertTrue(any(error["code"] == "boundary.overlap" for error in errors))

    def test_rejects_parent_child_ownership_overlap(self) -> None:
        document = copy.deepcopy(self.document)
        document["boundaries"]["shared_runtime_owned_paths"].append("tools/ai")

        errors = GUARDRAILS.validate_document(document)

        self.assertTrue(any(error["code"] == "boundary.overlap" for error in errors))

    def test_requires_every_shared_runtime_owner(self) -> None:
        document = copy.deepcopy(self.document)
        document["boundaries"]["shared_runtime_owned_paths"].remove("src/slic3r/Utils/Http.cpp")

        errors = GUARDRAILS.validate_document(document)

        self.assertTrue(any(error["code"] == "boundary.shared_runtime" for error in errors))

    def test_rejects_changed_integration_receipt(self) -> None:
        document = copy.deepcopy(self.document)
        document["integration_receipts"]["model_generation"]["integration_commit"] = "0" * 40

        errors = GUARDRAILS.validate_document(document)

        self.assertTrue(any(error["code"] == "contract.constant" for error in errors))

    def test_historical_receipts_match_source_and_integration_git_objects(self) -> None:
        head = subprocess.run(
            ["git", "rev-parse", "HEAD"],
            cwd=REPO_ROOT,
            check=True,
            capture_output=True,
            text=True,
            encoding="utf-8",
        ).stdout.strip()
        errors, receipts = GUARDRAILS.validate_integration_receipts(self.document, REPO_ROOT, head)

        self.assertEqual([], errors)
        self.assertEqual(4, len(receipts["model_generation"]["verified_paths"]))
        self.assertEqual(2, len(receipts["smart_slicing"]["verified_paths"]))

    def test_rejects_duplicate_development_ports(self) -> None:
        document = copy.deepcopy(self.document)
        document["runtime_contract"]["development_ports"]["integration"] = 18765

        errors = GUARDRAILS.validate_document(document)

        self.assertTrue(any(error["code"] == "runtime.ports" for error in errors))

    def test_rejects_product_port_assigned_to_development(self) -> None:
        document = copy.deepcopy(self.document)
        document["runtime_contract"]["development_ports"]["model_generation"] = 18764

        errors = GUARDRAILS.validate_document(document)

        self.assertTrue(any(error["code"] == "runtime.product_port" for error in errors))

    def test_malformed_port_is_reported_without_crashing(self) -> None:
        document = copy.deepcopy(self.document)
        document["runtime_contract"]["development_ports"]["integration"] = []

        errors = GUARDRAILS.validate_document(document)

        self.assertTrue(any(error["code"] == "runtime.ports" for error in errors))

    def test_dependency_scan_accepts_current_repository(self) -> None:
        self.assertEqual([], GUARDRAILS.validate_dependency_boundaries(REPO_ROOT))

    def test_dependency_scan_rejects_forbidden_fixture_references(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            fixtures = {
                "src/slic3r/AI/ModelGeneration/Bad.cpp": (
                    '#include "slic3r/AI/SmartSlicing/Application/SmartSlicingCoordinator.hpp"\n'
                ),
                "src/slic3r/GUI/ModelGenerationPanel.hpp": (
                    '#include "slic3r/AI/SmartSlicing/IModelArtifactConsumer.hpp"\n'
                ),
                "src/slic3r/AI/SmartSlicing/Domain/Bad.hpp": "#include <wx/panel.h>\n",
                "src/slic3r/AI/SmartSlicing/Application/Bad.cpp": '#include "tripo_client.hpp"\n',
                "src/slic3r/GUI/AI/SmartSlicing/Bad.cpp": (
                    '#include "ModelGenerationPanel.hpp"\nconst char* module = "openai_preprocessor";\n'
                ),
            }
            for relative_path, content in fixtures.items():
                path = root / relative_path
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(content, encoding="utf-8")

            errors = GUARDRAILS.validate_dependency_boundaries(root)

        self.assertEqual(5, len(errors))
        self.assertTrue(all(error["code"] == "boundary.dependency" for error in errors))
        messages = "\n".join(error["message"] for error in errors)
        self.assertIn("model-generation isolation", messages)
        self.assertIn("Domain/Application include isolation", messages)
        self.assertIn("SmartSlicing GUI provider/model-panel isolation", messages)

    def test_source_constant_check_reports_drift(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            for relative_path, _, _ in GUARDRAILS._source_requirements(self.document):
                path = root / relative_path
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("intentionally missing constants\n", encoding="utf-8")

            errors = GUARDRAILS.validate_source_constants(self.document, root)

        self.assertEqual(len(GUARDRAILS._source_requirements(self.document)), len(errors))
        self.assertTrue(all(error["code"] == "source.constant" for error in errors))

    def test_local_transport_security_is_part_of_source_contract(self) -> None:
        labels = {label for _, _, label in GUARDRAILS._source_requirements(self.document)}

        self.assertTrue(
            {
                "native Sidecar local-only transport",
                "native Sidecar session proof",
                "local HTTP redirect denial",
                "local HTTP proxy bypass",
                "local HTTP explicit proxy disable",
                "Sidecar request proof validation",
                "installed Sidecar session requirement",
                "installed Sidecar build identity gate",
                "installed Sidecar parent lifetime monitor",
                "Sidecar child environment allowlist",
                "Sidecar parent identity propagation",
                "bundled runtime dependency manifest",
                "installed runtime verification",
                "pull-request self-hosted runner denial",
                "authenticated Sidecar graceful shutdown",
                "deferred remote-job recovery",
            }.issubset(labels)
        )

    def test_source_contract_rejects_full_desktop_environment_inheritance(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            path = root / "src/slic3r/GUI/AIServiceManager.cpp"
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(
                "auto unsafe = boost::this_process::environment();\n",
                encoding="utf-8",
            )

            errors = GUARDRAILS.validate_source_constants(self.document, root)

        self.assertTrue(any(error["code"] == "source.forbidden" for error in errors))

    def test_source_contract_rejects_secret_inheritance_in_general_ci(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            path = root / ".github/workflows/build_all.yml"
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("secrets: inherit\n", encoding="utf-8")

            errors = GUARDRAILS.validate_source_constants(self.document, root)

        self.assertTrue(any(error["code"] == "source.forbidden" for error in errors))

    def test_rejects_tracked_package_credentials(self) -> None:
        errors = GUARDRAILS.validate_tracked_paths(
            [
                "tools/ai/orca_ai_internal_defaults.json",
                ".env.production",
                "docs/example.env.txt",
            ]
        )

        self.assertEqual(2, len(errors))
        self.assertTrue(all(error["code"] == "security.tracked_secret" for error in errors))

    def test_secret_content_scan_reports_location_without_value(self) -> None:
        secret = "tripo_live_0123456789abcdefghijklmnopqrstuvwxyz"
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            (root / "unsafe.py").write_text(f'TRIPO_API_KEY = "{secret}"\n', encoding="utf-8")
            (root / "safe.py").write_text('TRIPO_API_KEY = "test-tripo"\n', encoding="utf-8")

            errors = GUARDRAILS.validate_tracked_secret_content(root, ["unsafe.py", "safe.py"])

        self.assertEqual(1, len(errors))
        self.assertEqual("security.secret_content", errors[0]["code"])
        self.assertIn("unsafe.py:1", errors[0]["message"])
        self.assertNotIn(secret, errors[0]["message"])

    def test_json_cli_supports_skip_git(self) -> None:
        completed = subprocess.run(
            [sys.executable, str(SCRIPT_PATH), "--json", "--skip-git"],
            cwd=REPO_ROOT,
            check=False,
            capture_output=True,
            text=True,
            encoding="utf-8",
        )

        self.assertEqual(0, completed.returncode, completed.stderr)
        report = json.loads(completed.stdout)
        self.assertTrue(report["ok"])
        self.assertTrue(report["git_checks_skipped"])
        self.assertEqual([], report["errors"])


if __name__ == "__main__":
    unittest.main()
