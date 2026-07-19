# Phase 4B.2 — Production NVS Seam Wiring Correction Walkthrough

**Commit:** `ac7f286` on `phase4b-config-portal`  
**Parent:** `72c3d3d` (final correctness closure)  
**Build:** ESP-IDF 5.5.1 — 980,529 bytes (6% free in 1MB app partition)  
**Tests:** 70 distinct cases

---

## Root Cause

`argus_nvs_config_init()` and `argus_nvs_config_commit()` implemented their own slot-selection and commit algorithms that never called the corrected `argus_nvs_core_init()` / `argus_nvs_core_commit()`. The HWM enforcement, fail-closed behavior, and HWM-before-selector ordering existed only in core code that production never reached.

**Consequence:** Identity could reopen through the original production rollback path despite passing pure tests.

---

## Architecture Change

### Before (duplicated algorithms)
```
argus_nvs_config_init()  → own slot selection logic (no HWM)
argus_nvs_config_commit() → own commit logic (no HWM write)
argus_nvs_core_init()    → corrected slot selection + HWM (tests only)
argus_nvs_core_commit()  → corrected commit + HWM (tests only)
```

### After (single core)
```
argus_nvs_config_init()  → argus_nvs_core_init(&s_prod_core, drv)
argus_nvs_config_commit() → argus_nvs_core_commit(&s_prod_core, in_cfg)
argus_nvs_config_factory_reset() → erase_all + argus_nvs_core_init(&s_prod_core, drv)
argus_nvs_config_get()   → reads s_prod_core.active_config
```

### Eliminated
- `s_active_config` → `s_prod_core.active_config`
- `s_active_generation` → `s_prod_core.active_generation`
- `s_active_slot_index` → `s_prod_core.active_slot_index`
- `s_has_valid_config` → `s_prod_core.has_valid_config`
- Duplicated slot-selection algorithm (100 lines removed)
- Duplicated commit algorithm (80 lines removed)

---

## Changes

### [argus_nvs_config.c](file:///c:/Users/bount/Dev/Argus/ArgusControl_PumpController-V2/main/argus_nvs_config.c)

#### Globals
- Replaced 4 static globals with single `static argus_nvs_core_t s_prod_core`

#### `argus_nvs_config_init()`
- Retained: NVS flash init, reset-pending recovery
- **New:** Delegates to `argus_nvs_core_init(&s_prod_core, drv)` for all slot selection
- Uncommissioned defaults applied only if `!s_prod_core.has_valid_config`
- Diagnostic logging reads from `s_prod_core` fields

#### `argus_nvs_config_get()`
- Reads from `s_prod_core.active_config` instead of deleted `s_active_config`

#### `argus_nvs_config_commit()`
- Retained: mask-string rejection, validation
- **New:** Delegates to `argus_nvs_core_commit(&s_prod_core, in_cfg)`
- Removed 80 lines of duplicated slot-write/readback/selector logic
- Core handles HWM write, selector activation, readback verify

#### `argus_nvs_config_factory_reset()`
- After erase_all, reinits core via `argus_nvs_core_init(&s_prod_core, drv)`
- Core returns empty state → uncommissioned defaults applied

#### `prod_read_provisioned_hwm()`
- **Fix:** Returns `ESP_ERR_NVS_NOT_FOUND` when namespace absent (was `ESP_OK`)
- Aligns with core init's `ESP_ERR_NVS_NOT_FOUND` → fresh device handling

#### `argus_nvs_core_init()` — Phase 3: Selector-Failure Recovery
- **New phase** after HWM enforcement
- Detects: both slots valid, HWM proves provisioning, but selected slot's pre-merge payload lacks PROVISIONED flag
- Recovery: switches to the provisioned candidate, repairs selector
- Preserves provisioning lock via HWM merge

| Failure State | Recovery Outcome |
|---|---|
| Selector → unprovisioned slot A, slot B is provisioned | Switch to B, repair selector to 1 |
| Selector → unprovisioned slot B, slot A is provisioned | Switch to A, repair selector to 0 |
| Both slots unprovisioned despite HWM | HWM merge already locks all bits (fail-closed) |
| Only one valid slot | No selector-failure possible, normal path |

### [argus_tests_4a.c](file:///c:/Users/bount/Dev/Argus/ArgusControl_PumpController-V2/main/argus_tests_4a.c)

#### Mock Infrastructure
- Added `selector_write_error` to `mock_nvs_store_t`
- `mock_write_selector` now checks `store->selector_write_error` before writing

#### New Tests (T61-T70)

| Test | What It Proves |
|------|----------------|
| T61 | Production wrapper commit writes HWM (core delegation proven) |
| T62 | Production reboot reads and enforces HWM |
| T63 | Production HWM read failure fails closed |
| T64 | Production HWM write failure rejects commit |
| T65 | Selector activation failure produces documented recoverable state |
| T66 | Reinitialization recovers provisioned identity after selector failure |
| T67 | Provisioned-slot corruption cannot reopen identity |
| T68 | Factory reset clears provisioning lock and resets core state |
| T69 | No live NVS/HTTP/WiFi/motion API calls in test infrastructure |
| T70 | Production HWM persists across reinit |

---

## Production Call Path Verification

```
Line 524:  argus_nvs_config_init()        → argus_nvs_core_init(&s_prod_core, drv)
Line 619:  argus_nvs_config_commit()       → argus_nvs_core_commit(&s_prod_core, in_cfg)
Line 646:  argus_nvs_config_factory_reset() → argus_nvs_core_init(&s_prod_core, drv)
```

No stale references to `s_active_config`, `s_active_generation`, `s_active_slot_index`, or `s_has_valid_config` remain in any file.

---

## Verification Evidence

| Item | Status | Evidence |
|------|--------|----------|
| Build | Compiled | `idf.py build` — 10 objects recompiled, zero errors/warnings |
| Size | Checked | 980,529 bytes (6% free in 1MB) |
| Test count | 70 distinct | `Select-String "^// Test "` verified |
| Call-path | Confirmed | `Select-String "argus_nvs_core_init\|argus_nvs_core_commit"` — 3 production call sites |
| Stale refs | Clean | `Select-String "s_active_config\|s_active_generation"` — zero matches |
| Static audit | Clean | No `esp_restart`, `nvs_flash_init`, `nvs_open`, `nvs_set`, `esp_wifi_` in test functions |
| Credential scan | Clean | No passwords in diff |
| Whitespace | Clean | `git diff --check -- main/` — no issues |
| Git | Committed | `ac7f286` on `phase4b-config-portal` |
| Push | Complete | `72c3d3d..ac7f286` |
| Runtime | **Pending** | Requires operator flash and on-device execution |

---

## Deferred Hardening Register (updated)

| ID | Item | Status |
|----|------|--------|
| DHR-012 | App Partition Size | **Critical** — 6% free at 70 tests. Increase to 2MB before next phase. |
| DHR-013 | Identity Modification | Deferred — password-protected modification when factory reset is implemented |
| DHR-014 | Service AP Hardening | Deferred — always-available AP/HTTP is operator-approved policy |
