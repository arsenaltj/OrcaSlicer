# Phase 80 Quality-guided Regeneration Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Convert persisted model-quality evidence into bounded, provider-neutral regeneration advice that users can explicitly apply to the next model input without triggering a paid call.

**Architecture:** A new pure Python module maps known structural and visual warning codes to deterministic advice. The Sidecar exposes the optional advice in its public job DTO; the native client parses it defensively, and the model-generation panel displays and applies the bounded prompt suffix while preserving all existing paid confirmations.

**Tech Stack:** Python 3 standard library and `unittest`; C++17; nlohmann/json; wxWidgets; existing loopback Sidecar and Catch2/Release verification workflow.

---

### Task 1: Pure refinement-advice domain module

**Files:**
- Create: `tools/ai/model_refinement.py`
- Create: `tools/ai/test_model_refinement.py`

**Step 1: Write failing mapping tests**

Add tests that call:

```python
advice = build_model_refinement_advice(
    {"status": "review", "warnings": ["thin_local_wall_regions", "localized_overhang_regions"]},
    {},
)
```

Assert `available`, stable issue ordering, Chinese titles, and a bounded prompt suffix.

**Step 2: Add failing edge-case tests**

Cover passing reports, malformed input, unknown codes, duplicate category mappings,
visual warning mappings, maximum six issues, and byte limits.

**Step 3: Run the red tests**

Run:

```powershell
python -m unittest tools.ai.test_model_refinement -v
```

Expected: FAIL because `model_refinement` does not exist.

**Step 4: Implement the minimal pure module**

Define immutable rule data and:

```python
def build_model_refinement_advice(
    model_quality: Mapping[str, Any] | None,
    visual_quality: Mapping[str, Any] | None,
) -> dict[str, Any]:
    ...
```

Use only allow-listed codes. Deduplicate by category/action key, cap at six issues,
and enforce bounded UTF-8 output without splitting a code point.

**Step 5: Run the tests green**

Run the same unittest command. Expected: all tests PASS.

**Step 6: Commit**

```powershell
git add tools/ai/model_refinement.py tools/ai/test_model_refinement.py
git commit -m "feat(ai): derive model refinement advice"
```

### Task 2: Expose advice through the Sidecar job contract

**Files:**
- Modify: `tools/ai/orca_ai_sidecar.py`
- Modify: `tools/ai/test_sidecar_contract.py`
- Modify if packaged module lists require it: `scripts/package_windows_ai_test.ps1`
- Modify if packaged module lists require it: `packaging/windows-ai-test/setup/Check-Environment.ps1`

**Step 1: Write failing contract tests**

Persist representative `model-quality.json` and `visual-quality.json` files in a
job directory. Assert the public job contains:

```python
self.assertTrue(public["refinement"]["available"])
self.assertEqual(public["refinement"]["issues"][0]["code"], "thin_local_wall_regions")
```

Add a passing-report case that returns an unavailable empty advice object.

**Step 2: Run the red contract tests**

Run:

```powershell
python -m unittest tools.ai.test_sidecar_contract.SidecarContractTests.test_public_job_exposes_model_refinement_advice -v
```

Expected: FAIL because `refinement` is absent.

**Step 3: Integrate the pure builder**

Import `build_model_refinement_advice` and add the computed object to `_public_job`.
Do not persist a second source of truth and do not add a network route.

**Step 4: Update packaged runtime manifests only if explicit Python file lists are used**

Add one module entry beside the other `tools/ai` quality modules. Do not alter
dependencies, ports, environment variables, or installation defaults.

**Step 5: Run contract and module tests**

Run:

```powershell
python -m unittest tools.ai.test_model_refinement tools.ai.test_sidecar_contract -v
```

Expected: PASS.

**Step 6: Commit**

```powershell
git add tools/ai/orca_ai_sidecar.py tools/ai/test_sidecar_contract.py scripts/package_windows_ai_test.ps1 packaging/windows-ai-test/setup/Check-Environment.ps1
git commit -m "feat(ai): expose regeneration advice"
```

### Task 3: Parse the optional advice in the native client

**Files:**
- Modify: `src/slic3r/GUI/AIModelGenerationClient.hpp`
- Modify: `src/slic3r/GUI/AIModelGenerationClient.cpp`

**Step 1: Add the bounded DTO**

Add:

```cpp
struct ModelRefinementAdvice {
    struct Issue {
        std::string code;
        std::string category;
        std::string title;
        std::string instruction;
    };
    bool available { false };
    std::string summary;
    std::string prompt_suffix;
    std::vector<Issue> issues;
};
```

Store it on `JobStatus`.

**Step 2: Parse defensively**

Accept at most six issues and bounded strings. Ignore malformed objects, unknown
shape, missing text, or unavailable advice. Never interpret advice as a command.

**Step 3: Compile the native target**

Run:

```powershell
cmake --build build --config Release --target OrcaSlicer -- -m
```

Expected: Release build succeeds.

**Step 4: Commit**

```powershell
git add src/slic3r/GUI/AIModelGenerationClient.hpp src/slic3r/GUI/AIModelGenerationClient.cpp
git commit -m "feat(ai): read model refinement advice"
```

### Task 4: Add an explicit, no-network GUI application step

**Files:**
- Modify: `src/slic3r/GUI/ModelGenerationPanel.hpp`
- Modify: `src/slic3r/GUI/ModelGenerationPanel.cpp`

**Step 1: Add quality-card controls and state**

Add a summary label and `应用到下一次生成` button below visual review. Keep the
section hidden when advice is unavailable.

**Step 2: Apply advice locally**

The handler must:

```cpp
// Pseudocode
if (suffix.empty() || prompt already contains suffix) return;
if (UTF8 byte count would exceed 2000) show a local message and return;
m_prompt->SetValue(current_prompt + separator + suffix);
m_prompt->SetFocus();
```

It must not call `preprocess_*`, `generate`, `recommend_*`, or another client API.
Keep the current model and reports visible for comparison.

**Step 3: Wire recovery and state refresh**

Copy advice from every job status and clear it on reset or when no report is
available. Enable the button only while idle and when the suffix is not already
present.

**Step 4: Build Release**

Run the Release command from Task 3. Expected: PASS.

**Step 5: Commit**

```powershell
git add src/slic3r/GUI/ModelGenerationPanel.hpp src/slic3r/GUI/ModelGenerationPanel.cpp
git commit -m "feat(ai): apply regeneration advice locally"
```

### Task 5: Full verification and review record

**Files:**
- Create: `Docs/architecture/2026-08-25-phase80-quality-guided-regeneration-review.md`

**Step 1: Run Python verification**

Run module tests, Sidecar contracts, full `tools/ai` offline discovery, and
`python -m py_compile` for changed Python files. Expected: all PASS.

**Step 2: Run native verification**

Run the model-generation C++ regression target available in the current build,
then build the Windows Release `OrcaSlicer` target. Expected: all PASS.

**Step 3: Verify the repository-local GUI**

Launch only:

```text
D:\Workspace\06_3DDY_claude\build\src\Release\orca-slicer.exe
```

Load a local job carrying refinement advice. Verify the advice section, apply it,
confirm the input changes once, confirm the current model remains loaded, and
confirm Sidecar logs contain no preview/generation/provider action.

**Step 4: Run repository checks**

Run `git diff --check` and inspect the changed-file list. Do not stage or modify
the unrelated untracked `.tmp/` directory.

**Step 5: Write and commit the review**

Document behavior, tests, Release/GUI evidence, shared-file inventory, paid-call
count, and compatibility state.

```powershell
git add Docs/architecture/2026-08-25-phase80-quality-guided-regeneration-review.md
git commit -m "docs(ai): verify quality guided regeneration"
```
