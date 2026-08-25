# Model Provider Gateway Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Extract paid Tripo model-task orchestration from the Sidecar into a provider-neutral gateway with explicit one-shot authorization, safe remote-ID reuse, structured errors, and no automatic fallback.

**Architecture:** Add a pure Python `ModelProviderGateway` that adapts `tripo_client` behind provider-neutral request/result types. The Sidecar remains the job owner and artifact post-processor; it validates user intent, persists an attempt intent before the remote call, delegates provider operations, and persists returned IDs immediately.

**Tech Stack:** Python 3.12 standard library, `unittest`, existing Tripo adapter, existing Sidecar HTTP contract, PowerShell Windows packaging checks.

---

### Task 1: Provider gateway domain contract

**Files:**
- Create: `tools/ai/model_provider_gateway.py`
- Create: `tools/ai/test_model_provider_gateway.py`

**Step 1: Write failing policy and authorization tests**

Add tests that assert:

```python
policy = provider_policy()
self.assertEqual(policy.design_providers, ("gpt", "image2"))
self.assertEqual(policy.geometry_provider, "tripo")
self.assertFalse(policy.automatic_fallback)
self.assertEqual(policy.max_paid_model_tasks_per_confirmation, 1)

authorization = PaidTaskAuthorization.confirmed("job-1:model:1")
self.assertTrue(authorization.consume("tripo", "model_generation"))
with self.assertRaises(ProviderGatewayError) as raised:
    authorization.consume("tripo", "model_generation")
self.assertEqual(raised.exception.code, "authorization_consumed")
```

Also cover blank request IDs and wrong provider/operation.

**Step 2: Run the test and verify RED**

Run:

```powershell
python -m unittest tools.ai.test_model_provider_gateway -v
```

Expected: import failure because `model_provider_gateway.py` does not exist.

**Step 3: Implement the minimal domain types**

Implement frozen `ProviderPolicy`, `ModelTaskRequest`, `ProviderTaskRef`, structured `ProviderGatewayError`, and mutable one-shot `PaidTaskAuthorization`. Keep the module independent of Sidecar `Job` and HTTP types.

**Step 4: Run tests and verify GREEN**

Run the same unit-test command. Expected: policy and authorization tests pass.

**Step 5: Commit**

```powershell
git add -- tools/ai/model_provider_gateway.py tools/ai/test_model_provider_gateway.py
git commit -m "feat(ai): define model provider gateway"
```

### Task 2: Model task creation, reuse, and error classification

**Files:**
- Modify: `tools/ai/model_provider_gateway.py`
- Modify: `tools/ai/test_model_provider_gateway.py`

**Step 1: Write failing task orchestration tests**

Cover:

- existing task ID returns `reused=True` without consuming authorization or calling a provider;
- text requests call injected `create_text_task(prompt, face_limit)` once;
- image requests call injected `upload_image(path)` then `create_image_task(token, face_limit)` once;
- missing authorization invokes no provider;
- the same authorization cannot create a second task;
- no failure triggers a text/image or alternate-provider fallback;
- Tripo errors map to stable codes and retain safe messages;
- creation connection failures are `ambiguous=True` and never retried by Gateway.

Use injected callables so every test is offline.

**Step 2: Run focused tests and verify RED**

```powershell
python -m unittest tools.ai.test_model_provider_gateway -v
```

Expected: missing `ModelProviderGateway` behavior.

**Step 3: Implement `ModelProviderGateway`**

Constructor defaults adapt existing `tripo_client` functions but accept injected callables. Implement:

```python
def model_generation_available(self) -> bool: ...
def start_or_reuse_model_task(
    self,
    request: ModelTaskRequest,
    *,
    existing_task_id: str = "",
    authorization: PaidTaskAuthorization | None = None,
) -> ProviderTaskRef: ...
```

Validate source, prompt/image, face limit and task IDs before provider calls. Catch `TripoError`, classify it, and raise `ProviderGatewayError` without retrying.

**Step 4: Run tests and verify GREEN**

Run the focused suite. Expected: all Gateway creation/reuse/error tests pass.

**Step 5: Commit**

```powershell
git add -- tools/ai/model_provider_gateway.py tools/ai/test_model_provider_gateway.py
git commit -m "feat(ai): orchestrate paid model tasks"
```

### Task 3: Conversion, polling, and artifact transport

**Files:**
- Modify: `tools/ai/model_provider_gateway.py`
- Modify: `tools/ai/test_model_provider_gateway.py`

**Step 1: Write failing transport tests**

Test that the Gateway:

- reuses an existing conversion task without creating another;
- creates one conversion when explicitly allowed and no ID exists;
- refuses implicit conversion creation when `allow_create=False`;
- delegates polling and progress/cancellation unchanged;
- delegates bounded artifact download;
- classifies unsafe artifact, rate limit, timeout, cancellation and unavailable errors;
- never automatically creates a new model task while handling conversion/download errors.

**Step 2: Verify RED**

Run the focused Gateway tests and confirm the new methods are absent.

**Step 3: Implement transport methods**

Add provider-neutral methods:

```python
def start_or_reuse_conversion(..., allow_create: bool) -> ProviderTaskRef: ...
def wait_for_task(...): ...
def download_artifact(...): ...
```

Do not add retry loops. Retain the underlying Tripo client’s existing read-only polling behavior.

**Step 4: Verify GREEN and compile**

```powershell
python -m unittest tools.ai.test_model_provider_gateway -v
python -m py_compile tools/ai/model_provider_gateway.py tools/ai/test_model_provider_gateway.py
```

**Step 5: Commit**

```powershell
git add -- tools/ai/model_provider_gateway.py tools/ai/test_model_provider_gateway.py
git commit -m "feat(ai): isolate provider artifact transport"
```

### Task 4: Sidecar integration with persisted intent

**Files:**
- Modify: `tools/ai/orca_ai_sidecar.py`
- Modify: `tools/ai/test_obj_generation.py`
- Modify: `tools/ai/test_sidecar_contract.py`

**Step 1: Write failing Sidecar tests**

Add or update tests proving:

- `/generate` creates a one-shot authorization only after all job/palette/prompt/config checks pass;
- `_generate_job` records `provider_request_id`, provider, operation and `creating` before remote creation;
- successful creation immediately records `generation_task_id`;
- resume passes the existing ID and no authorization, producing zero paid task creation calls;
- ambiguous creation failure records structured error metadata and does not retry;
- conversion reuses persisted IDs during resume;
- recheck and visual review do not invoke any Gateway provider method.

**Step 2: Run focused tests and verify RED**

```powershell
python -m unittest tools.ai.test_obj_generation tools.ai.test_sidecar_contract -v
```

Expected: old direct-call expectations fail.

**Step 3: Integrate the Gateway**

- Replace direct Tripo function imports with a module-level `_MODEL_PROVIDER_GATEWAY`.
- Use `model_generation_available()` for health and `/generate` availability.
- Create `PaidTaskAuthorization.confirmed(f"{job.id}:model:{attempt_number}")` in the validated request flow and pass it to `_generate_job`.
- Persist the creation intent before calling the Gateway.
- Route task creation/reuse, polling, conversion and download through the Gateway.
- Catch `ProviderGatewayError` beside local `TripoError`; persist `provider_error_code`, category, retryable and ambiguous flags in the attempt.
- Preserve existing messages, states, progress and HTTP status codes.

**Step 4: Run focused tests and verify GREEN**

Run the same focused test command. Expected: all existing and new Sidecar tests pass.

**Step 5: Commit**

```powershell
git add -- tools/ai/orca_ai_sidecar.py tools/ai/test_obj_generation.py tools/ai/test_sidecar_contract.py
git commit -m "refactor(ai): route model tasks through gateway"
```

### Task 5: Capability contract and Windows package

**Files:**
- Modify: `tools/ai/orca_ai_sidecar.py`
- Modify: `tools/ai/test_sidecar_contract.py`
- Modify: `scripts/package_windows_ai_test.ps1`
- Modify: `packaging/windows-ai-test/setup/Check-Environment.ps1`

**Step 1: Write failing capability/package tests**

Assert health includes an additive block:

```json
{
  "provider_policy": {
    "design_providers": ["gpt", "image2"],
    "geometry_provider": "tripo",
    "automatic_fallback": false,
    "max_paid_model_tasks_per_confirmation": 1
  }
}
```

Assert Windows package manifests include `model_provider_gateway.py`.

**Step 2: Verify RED**

Run Sidecar contract tests and manifest checks. Expected: missing policy/module entries.

**Step 3: Implement additive contract and packaging entries**

Generate the health policy from `provider_policy()` and add the new Python module beside existing Sidecar runtime files.

**Step 4: Verify GREEN**

```powershell
python -m unittest tools.ai.test_model_provider_gateway tools.ai.test_sidecar_contract -v
git diff --check
```

**Step 5: Commit**

```powershell
git add -- tools/ai/orca_ai_sidecar.py tools/ai/test_sidecar_contract.py scripts/package_windows_ai_test.ps1 packaging/windows-ai-test/setup/Check-Environment.ps1
git commit -m "build(ai): package model provider gateway"
```

### Task 6: Full verification and review

**Files:**
- Create: `Docs/architecture/2026-08-26-phase81-model-provider-gateway-review.md`

**Step 1: Run complete offline tests**

```powershell
python -m unittest discover -s tools/ai -p "test_*.py"
python -m py_compile tools/ai/model_provider_gateway.py tools/ai/orca_ai_sidecar.py
```

Expected: all tests pass with no real Provider calls.

**Step 2: Run Release build**

```powershell
& 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' --build build --config Release --target OrcaSlicer -- -m
```

Expected: Release target links successfully; existing `LNK4098` warning is acceptable.

**Step 3: Run local contract smoke**

Start the repository Sidecar with a temporary output directory and no real credentials, verify `/health` exposes policy and unavailable generation safely, then stop it and confirm the temporary port is released. Do not issue generation endpoints.

**Step 4: Write the review record**

Document architecture, error/idempotency behavior, test counts, Release result, no paid calls, changed shared files, package/config/port/output/3MF/profile impact, and the exact repository-local verification boundary.

**Step 5: Commit and inspect status**

```powershell
git add -- Docs/architecture/2026-08-26-phase81-model-provider-gateway-review.md
git commit -m "docs(ai): verify model provider gateway"
git status --short
git diff --check
```

Expected: all model-generation changes committed; only the pre-existing unrelated `.tmp/` may remain untracked.

