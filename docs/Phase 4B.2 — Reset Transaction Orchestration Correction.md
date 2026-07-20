# Phase 4B.2 — Reset Transaction Orchestration Correction

**Branch:** `phase4b-config-portal`  
**Supersedes:** commit `0abc6e5` (durable marker) and `815f417` (pure-test isolation)

---

## Summary

Three corrections applied to the Phase 4B.2 reset transaction system:

1. **Testable core helpers** — Factory-reset and boot-recovery orchestration
   extracted into pure, caller-owned helpers that production wrappers delegate to.
2. **Initialization flag truthfulness** — `s_initialized` is now false on all
   failure paths; set true only after successful completion.
3. **Repository documentation** — Stale closure document updated with superseded
   notice; this consolidated document added.

---

## Architecture: Production-Used Core Helpers

### `argus_nvs_core_factory_reset(core, drv)`

Pure factory-reset transaction operating on caller-owned core and injected driver:

1. `write_reset_pending(true)` — returns immediately on failure
2. `erase_all()` — returns immediately on failure; marker preserved
3. `write_reset_pending(false)` — non-fatal if fails (documented production policy)
4. `argus_nvs_core_init(core, drv)` — reinitialize to match erased state

**Production policy:** If clearing the pending marker fails after a successful
erase, the core is still reinitialized. The marker remains true and triggers an
idempotent recovery erase on next boot. The clear error is returned.

### `argus_nvs_core_recovery_check(drv)`

Pure boot-recovery transaction operating on injected driver:

1. `read_reset_pending()` — missing key/namespace = not pending (normal)
2. If not pending, return ESP_OK (no erase)
3. If pending, `erase_all()` — return exact error on failure; marker preserved
4. `write_reset_pending(false)` — return exact error on failure; marker preserved

### Delegation Proof

| Production Wrapper | Delegates To |
|---|---|
| `argus_nvs_config_init()` | `argus_nvs_core_recovery_check(drv)` |
| `argus_nvs_config_factory_reset()` | `argus_nvs_core_factory_reset(&s_prod_core, drv)` |

Tests T68, T69, T71-T78 call the same helpers.

---

## Initialization Flag Truthfulness

| Path | Before | After |
|------|--------|-------|
| `nvs_flash_erase()` failure | `s_initialized = true` | `s_initialized = false` (unchanged from reset) |
| `nvs_flash_init()` retry failure | `s_initialized = true` | `s_initialized = false` |
| `recovery_check()` failure | `s_initialized = true` | `s_initialized = false` |
| `core_init()` failure | `s_initialized = true` | `s_initialized = false` |
| Successful completion | `s_initialized = true` | `s_initialized = true` (only here) |

---

## Test Coverage

### Factory-Reset Transaction (via `argus_nvs_core_factory_reset`)

| Test | Scenario | Key Assertions |
|------|----------|---------------|
| T68 | Successful factory reset | Full erasure, marker cleared, core uncommissioned |
| T69 | Erase failure | Exact error, pending preserved, data intact |
| T71 | Pending-write failure | No erase, data intact, exact error |
| T72 | Erase failure (via helper) | Pending survives, exact error |
| T73 | Pending-clear failure | Pending survives, data erased, core reinitializable |

### Boot-Recovery Transaction (via `argus_nvs_core_recovery_check`)

| Test | Scenario | Key Assertions |
|------|----------|---------------|
| T74 | Successful recovery | Full erasure, marker cleared, core uncommissioned |
| T75 | Pending-read error | Exact error propagated, not hidden |
| T76 | Missing marker | No erase, success |
| T77 | Recovery erase failure | Exact error, pending preserved |
| T78 | Recovery clear failure | Exact error, pending preserved, data erased |

All tests use stack-local `argus_nvs_core_t` and `mock_nvs_store_t`. Zero
production singleton calls.

---

## Files Changed

| File | Changes |
|------|---------|
| `argus_nvs_config.h` | Declared `argus_nvs_core_recovery_check`, `argus_nvs_core_factory_reset` |
| `argus_nvs_config.c` | Added two core helpers; `argus_nvs_config_init` delegates recovery; `argus_nvs_config_factory_reset` delegates transaction; `s_initialized` truthful on all paths |
| `argus_tests_4a.c` | T68-T78 rewritten to call production-used helpers |
| Stale closure doc | Superseded notice added |
| This document | Added |

---

## Verification Status

| Item | Status |
|------|--------|
| Full clean build | Pending this commit |
| Binary size | Pending this commit |
| Singleton isolation | Pending verification |
| Production delegation proof | Pending verification |
| Runtime | Pending operator flash |
