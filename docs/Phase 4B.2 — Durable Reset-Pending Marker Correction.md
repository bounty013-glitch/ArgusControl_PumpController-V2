# Phase 4B.2 — Durable Reset-Pending Marker Correction

**Commit:** `0abc6e5` on `phase4b-config-portal`  
**Parent:** `815f417` (pure-test isolation, reset truthfulness)  
**Build:** ESP-IDF 5.5.1 full-clean — 986,733 bytes (6% free in 1MB app partition)  
**Tests:** 78 distinct definitions, 78 RUN_TEST invocations, 3 passes, 234 expected executions

---

## Blocking Finding: Reset-Pending Marker Was Erased During Reset

### Root Cause

The reset-pending marker (`rst_pend`) lived in `ARGUS_NVS_NS_SYS`. `prod_erase_all()` erased both `ARGUS_NVS_NS_CFG` **and** `ARGUS_NVS_NS_SYS`, destroying the marker during factory reset. If power was lost between erase and clearing the marker, the next boot found no marker and skipped recovery.

### Architecture Fix: Dedicated `ARGUS_NVS_NS_RST` Namespace

```
Before:
  argus_cfg → [slot_a, slot_b]
  argus_sys → [active_slot, prov_hwm, rst_pend]  ← erased by factory reset
  
After:
  argus_cfg → [slot_a, slot_b]                    ← erased by factory reset
  argus_sys → [active_slot, prov_hwm]              ← erased by factory reset
  argus_rst → [rst_pend]                           ← NOT erased by factory reset
```

#### Durable Transaction Invariants (all 12 met)

| # | Invariant | How Enforced |
|---|-----------|-------------|
| 1 | `write_reset_pending(true)` completes before erase | Factory reset returns early on failure |
| 2 | Erase does not remove pending marker | `prod_erase_all` erases CFG + SYS, never RST |
| 3 | Failed erase preserves pending=true | Erase error → early return, marker untouched |
| 4 | Failed clear preserves pending=true | Clear error → marker stays in RST namespace |
| 5 | Boot recovery re-erases on pending=true | `argus_nvs_config_init` checks and erases |
| 6 | Recovery-clear only after successful erase | Clear follows erase-success gate |
| 7 | Missing marker = not pending | `ESP_ERR_NVS_NOT_FOUND` → `false`, `ESP_OK` |
| 8 | Real marker-read errors propagated | Other NVS errors → returned, not hidden |
| 9 | Recovery erase errors propagated | `argus_nvs_config_init` returns exact error |
| 10 | Init does not report success after failure | All paths return the real error |
| 11 | All factory-reset scope data erased | CFG (slots) + SYS (selector, HWM) erased |
| 12 | Portal credentials governed by 4B.2 policy | Unchanged |

---

## Supporting Fix: Init-Path Error Hiding Eliminated

### [prod_read_reset_pending()](../main/argus_nvs_config.c#L175-L206)

| NVS Result | Before | After |
|------------|--------|-------|
| `NOT_FOUND` (namespace or key) | `pending=false, ESP_OK` | `pending=false, ESP_OK` (unchanged) |
| Any other error | `pending=false, ESP_OK` | Error propagated |

### [argus_nvs_config_init()](../main/argus_nvs_config.c#L554-L601)

| Operation | Before | After |
|-----------|--------|-------|
| `nvs_flash_erase()` | Return value discarded | Failure → return error |
| `nvs_flash_init()` (retry) | Continued into init | Failure → return error |
| `read_reset_pending()` | Return value discarded | Failure → return error |
| Recovery `erase_all()` | Return value discarded | Failure → return error |
| Recovery `write_reset_pending(false)` | Return value discarded | Failure → return error |

---

## Mock Infrastructure

| Change | Purpose |
|--------|---------|
| `mock_erase_all()` preserves `reset_pending` | Mirrors the production architecture where RST namespace survives erase |
| `reset_pend_read_error` field added | Enables read-failure injection |
| `mock_read_reset_pending()` checks read error | Propagates injected error |
| All error-injection fields preserved across erase | Prevents test interference |

---

## New Tests T71-T78

| Test | Coverage | Key Assertion |
|------|----------|--------------|
| T71 | `write_pending(true)` fails | No erase occurs, original data intact |
| T72 | Erase fails after `pending=true` | Pending survives, erase error returned |
| T73 | Erase OK, `write_pending(false)` fails | Pending remains true, data erased |
| T74 | Boot recovery with `pending=true` | Re-erase runs, pending cleared, core uncommissioned |
| T75 | Pending-read real storage error | Error propagated, not interpreted as false |
| T76 | Missing pending key/namespace | Treated as not pending (normal) |
| T77 | Recovery erase failure | Exact error returned, pending preserved |
| T78 | Recovery pending-clear failure | Exact error returned, pending preserved |

All tests use stack-local `argus_nvs_core_t` and `mock_nvs_store_t`. Zero production singleton calls.

---

## Verification Evidence

| Item | Status | Evidence |
|------|--------|----------|
| Full clean build | **Compiled** | 1089 objects, zero errors, zero warnings |
| Binary size | 986,733 bytes | 6% free (61,728 bytes) in 1MB partition |
| Singleton isolation | **Proven** | `Select-String` — zero production singleton calls in test bodies |
| Production delegation | **Proven** | Lines 581, 676, 725 — `argus_nvs_core_init/commit` calls confirmed |
| Test count | 78 distinct | 78 RUN_TEST calls, 234 expected executions (3 passes) |
| Whitespace | Clean | `git diff --check` — no issues |
| Credentials | Clean | No passwords/tokens in diff |
| Git | Committed | `0abc6e5` on `phase4b-config-portal` |
| Push | Complete | `815f417..0abc6e5` |
| Runtime | **Pending** | Requires operator flash and on-device execution |

---

## Files Changed

| File | Changes |
|------|---------|
| [argus_nvs_config.c](../main/argus_nvs_config.c) | `ARGUS_NVS_NS_RST` namespace, `prod_read/write_reset_pending` → RST namespace, `prod_erase_all` unchanged (CFG+SYS only), `argus_nvs_config_init` error propagation, `nvs_flash_init/erase` error propagation |
| [argus_tests_4a.c](../main/argus_tests_4a.c) | `reset_pend_read_error` field, `mock_erase_all` preserves pending, `mock_read_reset_pending` error check, T68 updated, T71-T78 added, runner count → 78 |
