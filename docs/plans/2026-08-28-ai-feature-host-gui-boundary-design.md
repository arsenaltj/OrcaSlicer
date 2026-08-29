# AI FeatureHost and Thin GUI Boundary Design

## Goal

Reduce Orca upstream merge pressure by moving AI lifecycle and workflow orchestration out of `MainFrame` and `Plater`, while preserving the accepted GUI, Sidecar protocol, project/profile formats, and offline fallback behavior.

## Current problem

`MainFrame` directly owns Sidecar discovery, retry state, the model-generation workspace adapter, and the model-generation panel. `Plater` directly owns seven smart-slicing services and contains candidate validation, transactional mutation, slice execution, presenter wiring, and sidebar rendering. These shared Orca files are therefore both integration hotspots and feature implementation files. `ModelGenerationPanel.cpp` also combines presentation helpers, an OpenGL preview widget, wx event handling, job recovery, library persistence, and import orchestration in 5,367 lines.

## Considered approaches

### A. Keep orchestration in shared files and add comments

This has minimal code movement, but leaves upstream conflicts and ownership ambiguity unchanged. Guardrails can limit growth but cannot make feature lifecycle independently reviewable, so this does not meet the phase goal.

### B. Move every panel and Orca operation behind new abstract ports now

This would create the strongest formal separation, but it would require a broad Plater API redesign and a simultaneous rewrite of wx ownership, OpenGL preview, menu registration, and slicing callbacks. The regression surface is too large for a behavior-preserving checkpoint.

### C. Add FeatureHosts and extract cohesive presentation units incrementally

Chosen. A shared desktop host owns service discovery and delegates model-generation lifecycle to a model FeatureHost. A smart-slicing FeatureHost owns its adapters, coordinator, presenter, panel, candidate mutation workflow, pane state, and legacy sidebar projection. `MainFrame` and `Plater` retain only construction, navigation, one narrow official-slice bridge, and completion forwarding. The model preview widget and presentation helpers move out of the oversized panel implementation without changing the panel public API.

## Target structure

```text
GUI/AI/AIDesktopFeatureHost
├── AIServiceManager + retry lifecycle
├── ModelGeneration/ModelGenerationFeatureHost
│   ├── OrcaWorkspaceAdapter
│   └── ModelGenerationPanel
└── config-proposal capability callback -> MainFrame menu hook

GUI/AI/SmartSlicing/SmartSlicingFeatureHost
├── Orca smart-slicing adapters
├── coordinator + presenter + panel
├── candidate validation/application transaction
├── AUI pane lifecycle
└── legacy Sidebar projection

MainFrame / Plater
└── create host, mount UI, forward navigation and terminal slice events
```

The feature hosts are presentation/composition boundaries, so they may depend on Orca GUI adapters. Domain and Application code remains wx-free and continues to depend only on `AI/Contracts`. A separate GUI static library is intentionally deferred because the hosts call back into `Plater`, which is still compiled by `libslic3r_gui`; forcing a new static target now would create a circular link boundary rather than an independent module.

## Lifecycle and failure behavior

The desktop host starts discovery only in editor mode, keeps the existing bounded retry policy, and shuts down asynchronous discovery before wx child destruction. Model generation receives the same availability message and retry callback. Smart slicing is registered only after a compatible Sidecar advertises config proposals. Provider, Sidecar, or discovery failure disables only the affected AI action; Orca editing, manual slicing, Chinese UI, and first-run configuration remain unchanged.

Smart-slicing candidate application remains transactional. Validation runs before mutation, changed objects and plate parameters are applied within the existing undo snapshot, and exceptions trigger the existing rollback path. Official slice completion is forwarded from the same Plater completion point, preserving cancellation and internal-restart semantics.

## Model-generation panel split

The public `ModelGenerationPanel` class and all event signatures remain unchanged. The OpenGL model preview becomes a dedicated `ModelPreview3D` presentation unit. Formatting, progress mapping, palette helpers, file validation, and model-library path helpers become `ModelGenerationPresentation`. This reduces the high-churn panel implementation while keeping wx event ownership, task sequencing, output directories, and metadata behavior intact.

## Enforcement and verification

The integration lock advances to the `gui_feature_hosts` migration phase and records the three host paths. Guardrails require the hosts, reject direct AI service/model orchestration in `MainFrame`, reject direct smart-slicing service ownership in `Plater`, require the extracted preview unit, and lower the model panel line budget. Verification includes focused and full Python guardrails, native presentation/core tests, Windows Release compilation, GUI startup with Chinese labels and first-run behavior, and offline safety checks. No paid provider task is used.
