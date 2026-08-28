# AI Architecture Baseline Guardrails Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 将当前 AI 架构耦合量和发布晋级不变量变成可在本地与 CI 自动验证的集成闸门。

**Architecture:** 在现有 `ai-integration-lock.json` 和 `verify_ai_integration.py` 上扩展 lock v3，不创建第二套检查器。文档校验负责结构与固定值，仓库校验负责文件行数和相对锁定上游的共享触点 diff；所有运行时行为保持不变。

**Tech Stack:** Python 3.12、unittest、Git numstat、JSON、GitHub Actions、C++17/CMake（本阶段不改运行时代码）

---

### Task 1: Record the accepted design

**Files:**
- Create: `docs/plans/2026-08-28-ai-modular-integration-release-design.md`
- Create: `docs/architecture/ADR-005-guarded-incremental-ai-decomposition.md`
- Create: `docs/plans/2026-08-28-phase1-architecture-baseline-implementation-plan.md`

**Step 1: Verify the documents are present**

Run: `git status --short -- docs/architecture docs/plans`
Expected: only the three intended documents are new.

**Step 2: Check whitespace**

Run: `git diff --check -- docs/architecture docs/plans`
Expected: exit 0 with no output.

### Task 2: Add failing architecture-contract tests

**Files:**
- Modify: `tools/ai/test_integration_guardrails.py`

**Step 1: Add schema and budget tests**

Add tests that require:

- lock schema `orcaslicer.ai-integration-lock/v3`;
- pattern `desktop_modular_monolith` and target contract root `src/slic3r/AI/Contracts`;
- current repository line/diff budgets to pass;
- a one-line-over-limit fixture to return `architecture.line_budget`;
- a reduced MainFrame budget to return `architecture.diff_budget`;
- internal fast package to remain non-promotable and production rebuild to remain forbidden.

**Step 2: Run the tests and verify failure**

Run: `python -m unittest tools.ai.test_integration_guardrails -q`
Expected: FAIL because lock v2 and architecture validation do not yet exist.

### Task 3: Extend the lock to v3

**Files:**
- Modify: `docs/architecture/ai-integration-lock.json`

**Step 1: Add `architecture_contract`**

Record:

- modular-monolith pattern and `baseline_guarded` migration phase;
- target neutral-contract root;
- MainFrame, Plater and CMake composition roots;
- current line budgets for `ModelGenerationPanel.cpp` and `orca_ai_sidecar.py`;
- current per-file numstat budgets relative to locked upstream;
- immutable release-promotion booleans.

**Step 2: Validate JSON**

Run: `python -m json.tool docs/architecture/ai-integration-lock.json > $null`
Expected: exit 0.

### Task 4: Implement minimal validation

**Files:**
- Modify: `scripts/verify_ai_integration.py`

**Step 1: Validate architecture schema**

Add exact keys, normalized paths, positive integer budgets and fixed release-invariant checks.

**Step 2: Validate repository budgets**

Count physical lines with `splitlines()`. Run `git diff --numstat <locked-upstream>..HEAD -- <composition-roots>`, parse integer records, treat binary records as errors and report only paths/counts.

**Step 3: Connect validation**

Call the budget validation after document/source/dependency checks and before Git receipt validation so local `--skip-git` still checks filesystem architecture, while the shared-diff check uses the local repository.

**Step 4: Run focused tests**

Run: `python -m unittest tools.ai.test_integration_guardrails -q`
Expected: all tests pass.

### Task 5: Make CI cover architecture governance

**Files:**
- Modify: `.github/CODEOWNERS`
- Modify: `.github/workflows/ai-integration-guardrails.yml`

**Step 1: Expand path trigger**

Replace the single lock-file path with `docs/architecture/**` so ADR and readiness-control changes run the same guardrail.

Add explicit integration ownership for `docs/architecture/` and `src/slic3r/AI/Contracts/`.

**Step 2: Run YAML/static checks**

Run: `python scripts/verify_ai_integration.py --json`
Expected: `"ok": true`.

### Task 6: Complete phase verification and commit

**Files:**
- Verify all files above.

**Step 1: Run focused guardrail suite**

Run: `python -m unittest tools.ai.test_integration_guardrails -q`
Expected: all tests pass.

**Step 2: Run all AI Python tests**

Run: `python -m unittest discover -s tools/ai -p 'test_*.py' -q`
Expected: all tests pass with no paid provider request.

**Step 3: Run integration verifier and diff checks**

Run: `python scripts/verify_ai_integration.py`
Expected: `AI integration guardrails: PASS`.

Run: `git diff --check`
Expected: exit 0 with no output.

**Step 4: Commit one phase**

Run:

```powershell
git add .github/CODEOWNERS `
  .github/workflows/ai-integration-guardrails.yml `
  docs/architecture/ADR-005-guarded-incremental-ai-decomposition.md `
  docs/architecture/ai-integration-lock.json `
  docs/plans/2026-08-28-ai-modular-integration-release-design.md `
  docs/plans/2026-08-28-phase1-architecture-baseline-implementation-plan.md `
  scripts/verify_ai_integration.py `
  tools/ai/test_integration_guardrails.py
git commit -m "arch(ai): guard modular decomposition budgets"
```

Expected: one new commit after current HEAD; no history rewrite and no remote push.
