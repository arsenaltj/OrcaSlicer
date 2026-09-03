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

    def test_repository_lock_declares_guarded_modular_architecture(self) -> None:
        self.assertEqual("orcaslicer.ai-integration-lock/v3", self.document["schema"])
        architecture = self.document["architecture_contract"]

        self.assertEqual("desktop_modular_monolith", architecture["pattern"])
        self.assertEqual("gui_feature_hosts", architecture["migration_phase"])
        self.assertEqual("src/slic3r/AI/Contracts", architecture["target_contract_root"])
        self.assertEqual(
            {
                "desktop": "src/slic3r/GUI/AI/AIDesktopFeatureHost.hpp",
                "model_generation": "src/slic3r/GUI/AI/ModelGeneration/ModelGenerationFeatureHost.hpp",
                "smart_slicing": "src/slic3r/GUI/AI/SmartSlicing/SmartSlicingFeatureHost.hpp",
            },
            architecture["feature_hosts"],
        )
        self.assertEqual(
            {
                "src/slic3r/AI/Contracts/GeneratedModelArtifact.hpp",
                "src/slic3r/AI/Contracts/IModelArtifactConsumer.hpp",
                "src/slic3r/AI/Contracts/IPrintablePaletteProvider.hpp",
                "src/slic3r/GUI/AI/Orca/OrcaWorkspaceAdapter.hpp",
            },
            set(self.document["boundaries"]["allowed_cross_feature_contracts"]),
        )

    def test_repository_neutral_contract_layout_passes(self) -> None:
        self.assertEqual([], GUARDRAILS.validate_contract_layout(REPO_ROOT))

    def test_contract_layout_rejects_nonforwarding_legacy_header(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            fixtures = {
                "src/slic3r/AI/Contracts/GeneratedModelArtifact.hpp": "#pragma once\n",
                "src/slic3r/AI/Contracts/IPrintablePaletteProvider.hpp": "#pragma once\n",
                "src/slic3r/AI/Contracts/IModelArtifactConsumer.hpp": "#pragma once\n",
                "src/slic3r/AI/ModelGeneration/GeneratedModelArtifact.hpp": (
                    '#pragma once\n\n#include "slic3r/AI/ModelGeneration/Other.hpp"\n'
                ),
                "src/slic3r/AI/ModelGeneration/IPrintablePaletteProvider.hpp": (
                    '#pragma once\n\n#include "slic3r/AI/Contracts/IPrintablePaletteProvider.hpp"\n'
                ),
                "src/slic3r/AI/SmartSlicing/IModelArtifactConsumer.hpp": (
                    '#pragma once\n\n#include "slic3r/AI/Contracts/IModelArtifactConsumer.hpp"\n'
                ),
                "src/slic3r/GUI/ModelGenerationPanel.hpp": (
                    '#include "slic3r/AI/Contracts/IModelArtifactConsumer.hpp"\n'
                    '#include "slic3r/AI/Contracts/IPrintablePaletteProvider.hpp"\n'
                ),
                "src/slic3r/GUI/AI/Orca/OrcaWorkspaceAdapter.hpp": (
                    '#include "slic3r/AI/Contracts/IModelArtifactConsumer.hpp"\n'
                    '#include "slic3r/AI/Contracts/IPrintablePaletteProvider.hpp"\n'
                ),
            }
            for relative_path, content in fixtures.items():
                path = root / relative_path
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(content, encoding="utf-8")

            errors = GUARDRAILS.validate_contract_layout(root)

        self.assertEqual(1, len(errors))
        self.assertEqual("contract.forwarder", errors[0]["code"])
        self.assertIn("GeneratedModelArtifact.hpp", errors[0]["message"])

    def test_repository_ai_build_boundaries_pass(self) -> None:
        self.assertEqual([], GUARDRAILS.validate_ai_build_boundaries(REPO_ROOT))

    def test_repository_build_info_generation_is_multi_config_safe(self) -> None:
        self.assertEqual([], GUARDRAILS.validate_build_info_generation(REPO_ROOT))

    def test_build_info_generation_rejects_generated_configure_file_output(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            path = root / "src/slic3r/CMakeLists.txt"
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(
                "set(ORCA_BUILD_INFO_SOURCE \"${CMAKE_CURRENT_BINARY_DIR}/BuildInfo.cpp\")\n"
                "configure_file(BuildInfo.cpp.in \"${ORCA_BUILD_INFO_SOURCE}\" @ONLY)\n"
                "set_source_files_properties(\"${ORCA_BUILD_INFO_SOURCE}\" PROPERTIES GENERATED TRUE)\n"
                "add_library(libslic3r_gui STATIC ${ORCA_BUILD_INFO_SOURCE})\n",
                encoding="utf-8",
            )

            errors = GUARDRAILS.validate_build_info_generation(root)

        self.assertTrue(any(error["code"] == "build.multiconfig_generated_source" for error in errors))

    def test_repository_gui_feature_boundaries_pass(self) -> None:
        self.assertEqual([], GUARDRAILS.validate_gui_feature_boundaries(REPO_ROOT))

    def test_gui_feature_boundary_rejects_shared_file_orchestration(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            fixtures = {
                "src/slic3r/GUI/AI/AIDesktopFeatureHost.hpp": "#pragma once\n",
                "src/slic3r/GUI/AI/ModelGeneration/ModelGenerationFeatureHost.hpp": "#pragma once\n",
                "src/slic3r/GUI/AI/ModelGeneration/ModelGenerationPresentation.hpp": "#pragma once\n",
                "src/slic3r/GUI/AI/ModelGeneration/ModelPreview3D.hpp": "#pragma once\n",
                "src/slic3r/GUI/AI/SmartSlicing/SmartSlicingFeatureHost.hpp": "#pragma once\n",
                "src/slic3r/GUI/MainFrame.cpp": '#include "AIServiceManager.hpp"\n',
                "src/slic3r/GUI/MainFrame.hpp": "std::unique_ptr<AIServiceManager> manager;\n",
                "src/slic3r/GUI/Plater.cpp": "std::make_unique<AI::SmartSlicing::SmartSlicingCoordinator>();\n",
            }
            for relative_path, content in fixtures.items():
                path = root / relative_path
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(content, encoding="utf-8")

            errors = GUARDRAILS.validate_gui_feature_boundaries(root)

        codes = {error["code"] for error in errors}
        self.assertIn("gui.mainframe_boundary", codes)
        self.assertIn("gui.plater_boundary", codes)

    def test_build_boundary_rejects_duplicate_gui_source_ownership(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            ai_path = root / "src/slic3r/AI/CMakeLists.txt"
            ai_path.parent.mkdir(parents=True, exist_ok=True)
            ai_path.write_text(
                "add_library(orcaslicer_ai_contracts INTERFACE)\n"
                "add_library(OrcaSlicer::AIContracts ALIAS orcaslicer_ai_contracts)\n"
                "target_include_directories(orcaslicer_ai_contracts INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/../..)\n"
                "target_link_libraries(orcaslicer_ai_contracts INTERFACE boost_headeronly)\n"
                "set(ORCA_AI_SMART_SLICING_SOURCES\n"
                "    SmartSlicing/Application/SmartSlicingCoordinator.cpp\n"
                "    SmartSlicing/Application/PrintabilityInspector.cpp\n"
                "    SmartSlicing/Domain/CandidateComparison.cpp\n"
                "    SmartSlicing/Domain/ParameterProposalValidator.cpp\n"
                ")\n"
                "add_library(orcaslicer_ai_smart_slicing STATIC ${ORCA_AI_SMART_SLICING_SOURCES})\n"
                "add_library(OrcaSlicer::SmartSlicing ALIAS orcaslicer_ai_smart_slicing)\n"
                "target_link_libraries(orcaslicer_ai_smart_slicing PUBLIC OrcaSlicer::AIContracts)\n",
                encoding="utf-8",
            )
            gui_path = root / "src/slic3r/CMakeLists.txt"
            gui_path.write_text(
                "add_subdirectory(AI)\n"
                "set(SLIC3R_GUI_SOURCES\n"
                "    AI/SmartSlicing/Application/SmartSlicingCoordinator.cpp\n"
                ")\n"
                "target_link_libraries(libslic3r_gui OrcaSlicer::AIContracts OrcaSlicer::SmartSlicing)\n",
                encoding="utf-8",
            )

            errors = GUARDRAILS.validate_ai_build_boundaries(root)

        self.assertTrue(any(error["code"] == "build.source_ownership" for error in errors))

    def test_repository_architecture_budgets_pass(self) -> None:
        self.assertEqual([], GUARDRAILS.validate_architecture_budgets(self.document, REPO_ROOT))

    def test_architecture_line_budget_rejects_growth(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            path = root / "large.py"
            path.write_text("first\nsecond\nthird\n", encoding="utf-8")
            document = {
                "upstream": {"sha": "0" * 40},
                "architecture_contract": {
                    "decomposition_line_budgets": {"large.py": 2},
                    "shared_touchpoint_diff_budgets": {},
                },
            }

            errors = GUARDRAILS.validate_architecture_budgets(document, root)

        self.assertTrue(any(error["code"] == "architecture.line_budget" for error in errors))

    def test_architecture_diff_budget_rejects_growth(self) -> None:
        document = copy.deepcopy(self.document)
        document["architecture_contract"]["shared_touchpoint_diff_budgets"][
            "src/slic3r/GUI/MainFrame.cpp"
        ]["max_added_lines"] = 0
        document["architecture_contract"]["shared_touchpoint_diff_budgets"][
            "src/slic3r/GUI/MainFrame.cpp"
        ]["max_net_added_lines"] = 0

        errors = GUARDRAILS.validate_architecture_budgets(document, REPO_ROOT)

        self.assertTrue(any(error["code"] == "architecture.diff_budget" for error in errors))

    def test_architecture_diff_budget_includes_uncommitted_growth(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            relative_path = "shared.cpp"
            path = root / relative_path
            subprocess.run(["git", "init", "--quiet", str(root)], check=True)
            subprocess.run(["git", "-C", str(root), "config", "user.name", "Guardrail Test"], check=True)
            subprocess.run(["git", "-C", str(root), "config", "user.email", "guardrail@example.invalid"], check=True)
            subprocess.run(["git", "-C", str(root), "config", "core.autocrlf", "false"], check=True)
            path.write_text("baseline\n", encoding="utf-8")
            subprocess.run(["git", "-C", str(root), "add", relative_path], check=True)
            subprocess.run(["git", "-C", str(root), "commit", "--quiet", "-m", "baseline"], check=True)
            upstream_sha = subprocess.run(
                ["git", "-C", str(root), "rev-parse", "HEAD"],
                check=True,
                capture_output=True,
                text=True,
                encoding="utf-8",
            ).stdout.strip()
            path.write_text("baseline\nuncommitted growth\n", encoding="utf-8")
            document = {
                "upstream": {"sha": upstream_sha},
                "architecture_contract": {
                    "decomposition_line_budgets": {},
                    "shared_touchpoint_diff_budgets": {
                        relative_path: {"max_added_lines": 0, "max_net_added_lines": 0}
                    },
                },
            }

            errors = GUARDRAILS.validate_architecture_budgets(document, root)

        self.assertTrue(any(error["code"] == "architecture.diff_budget" for error in errors))

    def test_release_promotion_invariants_cannot_be_weakened(self) -> None:
        document = copy.deepcopy(self.document)
        promotion = document["architecture_contract"]["release_promotion"]
        promotion["internal_fast_promotable"] = True
        promotion["production_rebuild_allowed"] = True

        errors = GUARDRAILS.validate_document(document)

        messages = "\n".join(error["message"] for error in errors)
        self.assertIn("internal_fast_promotable", messages)
        self.assertIn("production_rebuild_allowed", messages)

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
        self.assertEqual(9, len(receipts["model_generation"]["verified_paths"]))
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
                "src/slic3r/AI/Contracts/Bad.hpp": (
                    '#include "slic3r/AI/SmartSlicing/Domain/SmartSlicingTypes.hpp"\n'
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

        self.assertEqual(7, len(errors))
        self.assertTrue(all(error["code"] == "boundary.dependency" for error in errors))
        messages = "\n".join(error["message"] for error in errors)
        self.assertIn("model-generation isolation", messages)
        self.assertIn("neutral contract isolation", messages)
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
                "commercial package source identity fail-closed gate",
                "pull-request self-hosted runner denial",
                "authenticated Sidecar graceful shutdown",
                "deferred remote-job recovery",
            }.issubset(labels)
        )

    def test_architecture_ownership_is_part_of_source_contract(self) -> None:
        labels = {label for _, _, label in GUARDRAILS._source_requirements(self.document)}

        self.assertTrue(
            {
                "architecture documentation CODEOWNER",
                "AI composition CODEOWNER",
                "neutral AI contract CODEOWNER",
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

    def test_image2_pro_environment_is_allowlisted_without_full_environment_inheritance(self) -> None:
        service_manager = (REPO_ROOT / "src/slic3r/GUI/AIServiceManager.cpp").read_text(encoding="utf-8")

        self.assertIn('"OPENAI_PRO_API"', service_manager)
        self.assertIn('"OPENAI_PRO_URL"', service_manager)
        self.assertNotIn("boost::this_process::environment()", service_manager)

    def test_internal_release_requires_locked_credentials_but_commercial_still_forbids_them(self) -> None:
        cmake = (REPO_ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        source_cmake = (REPO_ROOT / "src/CMakeLists.txt").read_text(encoding="utf-8")
        packager = (REPO_ROOT / "scripts/package_internal_fast.ps1").read_text(encoding="utf-8")
        wrapper = (REPO_ROOT / "release/build_internal.ps1").read_text(encoding="utf-8")

        self.assertIn("Commercial AI installer packages must not embed provider credentials", cmake)
        self.assertIn('ORCA_AI_DISTRIBUTION_CHANNEL STREQUAL "internal"', cmake)
        self.assertIn('RENAME "orca_ai_internal_defaults.json"', cmake)
        self.assertNotIn("cmake/VerifyAIInstall.cmake", cmake)
        verify_runtime = source_cmake.index("verify_bundled_runtime.py")
        verify_configuration = source_cmake.index("cmake/VerifyAIInstall.cmake")
        self.assertGreater(verify_configuration, verify_runtime)
        self.assertIn("internal package defaults payload", packager)
        self.assertIn("package_internal_locked", packager)
        self.assertIn("-G ZIP", packager)
        self.assertIn("create_internal_defaults.py", wrapper)
        self.assertIn("ORCA_AI_INTERNAL_DEFAULTS_FILE:FILEPATH", wrapper)

    def test_source_contract_rejects_secret_inheritance_in_general_ci(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            path = root / ".github/workflows/build_all.yml"
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("secrets: inherit\n", encoding="utf-8")

            errors = GUARDRAILS.validate_source_constants(self.document, root)

        self.assertTrue(any(error["code"] == "source.forbidden" for error in errors))

    def test_source_contract_rejects_global_commit_compile_definition(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            path = root / "CMakeLists.txt"
            path.write_text(
                'add_definitions("-DGIT_COMMIT_HASH=\\\"deadbee\\\"")\n',
                encoding="utf-8",
            )

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

    def test_secret_content_scan_covers_pro_image_provider_key(self) -> None:
        secret = "provider_pro_0123456789abcdefghijklmnopqrstuvwxyz"
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            (root / "unsafe.ps1").write_text(
                f'$env:OPENAI_PRO_API = "{secret}"\n', encoding="utf-8"
            )

            errors = GUARDRAILS.validate_tracked_secret_content(root, ["unsafe.ps1"])

        self.assertEqual(1, len(errors))
        self.assertEqual("security.secret_content", errors[0]["code"])
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
