"""Stable capability discovery for alternate UIs; discovery never executes work."""
from __future__ import annotations

import argparse
from dataclasses import asdict, dataclass
import json
from typing import Any, Mapping

SCHEMA = "orca-atomic-capabilities-v1"

# Checked by verify_ai_integration: these modules cannot depend on the HTTP host,
# widgets, dynamic namespace injection or each other's application workflows.
ATOMIC_MODULES = (
    "config_proposal", "design_workflow", "model_artifact_workflow", "model_color_regions",
    "model_color_space", "model_contracts", "model_creation_policy", "model_generation_workflow",
    "model_job_application", "model_job_lifecycle", "model_job_repository", "model_mesh_repair",
    "model_obj_io", "model_quality_workflow", "model_request", "model_texture_baking",
    "portrait_geometry_reference", "portrait_model_materials", "portrait_multiview_workflow",
)


@dataclass(frozen=True)
class Capability:
    id: str
    title: str
    source: str
    entrypoint: str
    inputs: tuple[str, ...]
    output: str
    effects: tuple[str, ...] = ()
    confirmation: bool = False
    availability: str = "local"
    isolation: str = "independent"
    route: str = ""


CAPABILITIES = (
    Capability("reference.policy", "前处理分类与规则", "tools/ai/image_preprocessing_policy.py",
               "build_image_preprocessing_policy", ("instruction", "style", "print_settings"), "preprocessing_policy"),
    Capability("reference.prompt", "人像及各类型提示词拼接", "tools/ai/openai_preprocessor.py",
               "build_geometry_reference_prompt_layers", ("instruction", "style", "preprocessing_policy"), "prompt_layers"),
    Capability("design.image", "图像生成设计参考", "tools/ai/design_workflow.py", "DesignWorkflow.preprocess_image",
               ("job", "source_image", "instruction"), "awaiting_confirmation_job", ("paid_image", "write_files"),
               True, "image", route="POST /v1/orcaslicer/model-jobs/image"),
    Capability("design.text", "文字生成设计参考", "tools/ai/design_workflow.py", "DesignWorkflow.preprocess_text",
               ("job", "prompt"), "awaiting_confirmation_job", ("paid_image", "write_files"),
               True, "text", route="POST /v1/orcaslicer/model-jobs/text"),
    Capability("reference.review", "非人像参考诊断", "tools/ai/nonportrait_reference.py", "review_nonportrait_reference",
               ("source_image", "prepared_image", "policy"), "reference_report", ("write_files", "optional_paid_vision")),
    Capability("model.generate", "经确认的参考生成3D", "tools/ai/model_generation_workflow.py", "ModelGenerationWorkflow.generate",
               ("confirmed_job", "prepared_prompt", "paid_authorization"), "model_job", ("paid_3d", "write_files"),
               True, "geometry", route="POST /v1/orcaslicer/model-jobs/{id}/generate"),
    Capability("model.texture", "保留几何重新贴图", "tools/ai/model_generation_workflow.py", "ModelGenerationWorkflow.retexture",
               ("confirmed_reference_job", "source_task", "paid_authorization"), "model_job", ("paid_texture", "write_files"),
               True, "texture", route="POST /v1/orcaslicer/model-jobs/{id}/retexture"),
    Capability("model.check", "已有模型质量检查", "tools/ai/model_quality_workflow.py", "ModelQualityWorkflow.check_saved_model",
               ("registered_asset",), "quality_report", ("write_report",), route="POST /v1/orcaslicer/model-check"),
    Capability("artifact.prepare", "模型转换与归一化", "tools/ai/model_artifact_workflow.py", "ModelArtifactWorkflow.prepare_obj_artifact",
               ("download", "output_directory", "palette"), "obj_artifact", ("write_files",)),
    Capability("mesh.repair", "局部拓扑修复", "tools/ai/model_mesh_repair.py", "_repair_small_obj_topology_defects",
               ("obj", "report_path"), "repair_report", ("change_supplied_file",), True),
    Capability("color.bake", "贴图转顶点色", "tools/ai/model_texture_baking.py", "_bake_obj_texture_to_vertex_colors",
               ("obj", "output", "palette"), "vertex_color_obj", ("write_files",)),
    Capability("color.regions", "局部颜色区域整理", "tools/ai/model_color_regions.py", "_regularize_obj_color_boundaries",
               ("obj", "report_path"), "color_report", ("change_supplied_file",), True),
    Capability("portrait.reference", "旧人像几何参考准备", "tools/ai/portrait_geometry_reference.py", "_geometry_generation_reference",
               ("job",), "geometry_reference", ("write_files",)),
    Capability("portrait.multiview", "显式人像多视图流程", "tools/ai/portrait_multiview_workflow.py", "PortraitMultiviewWorkflow.ensure_portrait_multiview",
               ("job",), "view_references", ("optional_paid_image", "optional_paid_vision", "write_files"), True, "image"),
    Capability("history.restore", "恢复既有任务", "tools/ai/model_job_lifecycle.py", "ModelJobLifecycle.restore_jobs",
               ("job_directory",), "registered_jobs", ("read_history",)),
    Capability("history.reuse", "复用设计参考", "tools/ai/model_job_application.py", "ModelJobApplication.reuse_design",
               ("job_id",), "awaiting_confirmation_job", ("write_files",), route="POST /v1/orcaslicer/model-jobs/{id}/reuse-design"),
    Capability("model.import", "原生模型导入", "src/slic3r/AI/Contracts/IModelArtifactConsumer.hpp", "IModelArtifactConsumer",
               ("ModelImportRequest",), "ModelImportResult", ("change_project",), True, "native", "native_port"),
    Capability("model.prepare", "尺寸与底座提案", "src/slic3r/GUI/AI/Orca/OrcaModelPreparation.hpp", "prepare_model",
               ("model", "preparation_options"), "ModelPreparation", availability="native", isolation="native_port"),
    Capability("model.prepare.apply", "应用尺寸与底座", "src/slic3r/GUI/AI/Orca/OrcaModelPreparation.hpp", "apply_model_preparation",
               ("model", "ModelPreparation"), "prepared_model", ("change_project",), True, "native", "native_adapter"),
    Capability("color.workbench", "分区与人工配色", "src/slic3r/GUI/AI/ModelGeneration/ModelGenerationFeatureHost.hpp", "request_workbench_color_matching",
               ("artifact", "palette"), "colored_artifact", ("change_appearance",), False, "native", "host_bound"),
    Capability("slice.inspect", "打印风险检查", "src/slic3r/AI/SmartSlicing/Application/PrintabilityInspector.hpp", "PrintabilityInspector",
               ("workspace_revision",), "PrintabilityReport", availability="native", isolation="native_port"),
    Capability("slice.trial", "候选试切片与比较", "src/slic3r/AI/SmartSlicing/Application/SmartSlicingCoordinator.hpp", "begin_candidate_trials",
               ("workspace_revision", "proposals"), "candidate_comparison", ("trial_slice",), availability="native", isolation="native_port"),
    Capability("slice.apply", "应用选定切片候选", "src/slic3r/AI/SmartSlicing/Application/SmartSlicingCoordinator.hpp", "apply_selected_candidate",
               ("selected_candidate", "unchanged_revision"), "official_slice", ("change_project", "official_slice"),
               True, "native", "native_port"),
)

RECIPES = (
    {"id": "image_to_model", "title": "图片到模型", "steps": ["design.image", "model.generate"],
     "checkpoints": ["review_design", "confirm_paid_model"], "automatic_execution": False},
    {"id": "text_to_model", "title": "文字到模型", "steps": ["design.text", "model.generate"],
     "checkpoints": ["review_design", "confirm_paid_model"], "automatic_execution": False},
    {"id": "existing_model_to_print", "title": "已有模型到切片候选",
     "steps": ["model.check", "model.import", "model.prepare", "model.prepare.apply", "slice.inspect", "slice.trial", "slice.apply"],
     "checkpoints": ["confirm_import", "confirm_preparation", "compare_and_confirm_candidate"], "automatic_execution": False},
)


def build_catalog(availability: Mapping[str, bool] | None = None) -> dict[str, Any]:
    """Expose source boundaries and routes; native availability belongs to its host."""
    availability = availability or {}
    items = []
    for capability in CAPABILITIES:
        item = asdict(capability)
        item["available"] = True if capability.availability == "local" else availability.get(capability.availability)
        # None is intentional: the Sidecar cannot claim native workspace availability.
        items.append(item)
    return {"schema": SCHEMA, "capabilities": items,
            "recipes": [{**recipe, "steps": list(recipe["steps"]), "checkpoints": list(recipe["checkpoints"])} for recipe in RECIPES]}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--json", action="store_true", help="Print offline catalog JSON without creating work.")
    parser.parse_args()
    print(json.dumps(build_catalog(), ensure_ascii=True, indent=2))


if __name__ == "__main__":
    main()
