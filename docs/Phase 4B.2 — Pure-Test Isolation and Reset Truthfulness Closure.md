# Phase 4B.2 — Pure-Test Isolation and Reset Truthfulness Closure

**Commit:** `815f417` on `phase4b-config-portal`  
**Parent:** `ac7f286` (production NVS seam wiring)  
**Build:** ESP-IDF 5.5.1 full-clean — 981,605 bytes (6% free in 1MB app partition)  
**Tests:** 70 distinct cases

---

## Issue 1: Production-Singleton Mutation from Test Functions

### Root Cause

Tests T61–T68 and T70 called production singleton APIs (`argus_nvs_config_init`, `_commit`, `_get`, `_factory_reset`), which mutated global `s_custom_driver` to point at stack-local mock drivers. After each test returned, `s_custom_driver` pointed to an expired stack frame. The final production observation snapshot called `get_driver()` through this dangling pointer.

### Fix: Stack-Local Core Contexts

All T61-T70 now use only `argus_nvs_core_*` with caller-owned `argus_nvs_core_t` instances:

| Test | Before | After |
|------|--------|-------|
| T61 | `argus_nvs_config_init/commit` | `argus_nvs_core_init/commit` |
| T62 | `argus_nvs_config_init/commit/get` | `argus_nvs_core_init/commit/get` |
| T63 | `argus_nvs_config_init/commit/get` | `argus_nvs_core_init/commit/get` |
| T64 | `argus_nvs_config_init/commit` | `argus_nvs_core_init/commit` |
| T65-T67 | Already pure | Unchanged |
| T68 | `argus_nvs_config_init/commit/factory_reset/get` | `argus_nvs_core_init/commit` + driver erase + reinit |
| T69 | Mock driver round-trip | Erase-failure propagation test (NEW) |
| T70 | `argus_nvs_config_init/commit/get` | `argus_nvs_core_init/commit/get` + second-commit lock test |

### Static Proof — Zero Production Singleton Calls in Tests

```
Select-String "argus_nvs_config_init|argus_nvs_config_commit|
               argus_nvs_config_get|argus_nvs_config_factory_reset"
```

Matches:
- Line 887: `argus_nvs_config_get_observation_snapshot` — production snapshot in runner harness (correct)
- Lines 2319-2321, 2572: Comments documenting static proof

No test function calls any production singleton.

### Production Wrapper Delegation — Static Proof

| Line | Production Wrapper | Core Call |
|------|-------------------|-----------|
| 531 | `argus_nvs_config_init()` | `argus_nvs_core_init(&s_prod_core, drv)` |
| 626 | `argus_nvs_config_commit()` | `argus_nvs_core_commit(&s_prod_core, in_cfg)` |
| 675 | `argus_nvs_config_factory_reset()` | `argus_nvs_core_init(&s_prod_core, drv)` |

Successful compilation proves these calls are type-correct and linked.

---

## Issue 2: Factory-Reset Return-Value Truthfulness

### Root Cause

- `prod_erase_all()` returned `ESP_OK` even when `nvs_open`, `nvs_erase_all`, or `nvs_commit` failed
- `argus_nvs_config_factory_reset()` ignored return values from all four operations

### Fix

#### [prod_erase_all()](file:///c:/Users/bount/Dev/Argus/ArgusControl_PumpController-V2/main/argus_nvs_config.c#L236-L267)

Returns the first real storage error. `ESP_ERR_NVS_NOT_FOUND` on namespace open is non-fatal (namespace may not exist on fresh device).

#### [argus_nvs_config_factory_reset()](file:///c:/Users/bount/Dev/Argus/ArgusControl_PumpController-V2/main/argus_nvs_config.c#L632-L689)

| Step | Failure Behavior |
|------|-----------------|
| `write_reset_pending(true)` | Early return — no destructive operation attempted |
| `erase_all()` | Early return — `reset_pending` preserved for boot recovery |
| `write_reset_pending(false)` | Non-fatal — erase succeeded, next boot re-erases (idempotent) |
| `argus_nvs_core_init()` | Propagated — `s_prod_core` not updated to unjustified state |
| Final return | `clear_err` (captures non-fatal pending-clear failure) |

#### Selector Repair — Best-Effort Documentation

```diff
-            /* Repair selector */
-            if (driver->write_selector) {
-                driver->write_selector(driver->ctx, 1);
+            /* Best-effort selector repair: failure is non-fatal.
+             * In-memory state is correct; next commit repairs durably. */
+            if (driver->write_selector) {
+                (void)driver->write_selector(driver->ctx, 1);
```

Dead code block (lines 356-363 of previous commit) removed.

---

## Mock Infrastructure

| Field | Purpose |
|-------|---------|
| `erase_all_error` | Injects failure into `mock_erase_all` |
| `reset_pend_write_error` | Injects failure into `mock_write_reset_pending` |

`mock_erase_all` now preserves error-injection fields across erase (saves/restores them around `memset`).

---

## Verification Evidence

| Item | Status | Evidence |
|------|--------|----------|
| Full clean build | Compiled | 1089 objects, zero errors/warnings |
| Size | 981,605 bytes | 6% free (66,848 bytes) in 1MB partition |
| Singleton isolation | **Proven** | `Select-String` — zero production singleton calls in test functions |
| Production delegation | **Proven** | Lines 531, 626, 675 confirmed |
| Test count | 70 distinct | 70 RUN_TEST calls, 69 definitions + Test 19 gap |
| Whitespace | Clean | `git diff --check` — no issues |
| Credentials | Clean | No passwords in diff |
| Git | Committed | `815f417` on `phase4b-config-portal` |
| Push | Complete | `ac7f286..815f417` |
| Runtime | **Pending** | Requires operator flash and on-device execution |
