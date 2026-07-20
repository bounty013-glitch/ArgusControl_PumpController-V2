# Implementation Plan - Phase 4A Pure-Test and Service-Orchestration Hardening

## Goal Description
Implement comprehensive pure-test and service-orchestration hardening for Argus Pump Controller V2:
1. Separate `argus_authority_core_t` (mutable core state) from `argus_authority_snapshot_t` (read-only DTO).
2. Expand `argus_service_transition_ops_t` to a complete 14-step synchronous orchestrator with strict action -> verification ordering used identically in production and unit tests.
3. Fail closed on any transition step failure, returning authority to `NONE/NONE` and network mode to `NETWORK_FAULT`.
4. Capture and compare all 23 production isolation fields before/after `t`.
5. Make Mock NVS storage 100% caller-owned on the stack, and update `argus_nvs_core_commit()` from verified readback payload `verify_slot.payload`.
6. Convert all 18 unit tests in `argus_tests_4a.c` to be 100% stack-local, with zero calls to EFUSE MAC, live authority singletons, or live console statics.

---

## User Review Required

> [!IMPORTANT]
> **Hardening Principles**
> - Production and pure unit tests execute the exact same 14-step service-entry ordering.
> - All mock NVS storage is caller-owned on the stack (no file-static mock storage).
> - Production snapshot comparison covers all 23 required fields with explicit diff logging.
> - All 18 unit tests execute on stack-local instances with zero production singleton access.

---

## Proposed Changes

### Component 1: Authority Core & Snapshot Separation

#### [MODIFY] [argus_authority_mgr.h](../main/argus_authority_mgr.h)
#### [MODIFY] [argus_authority_mgr.c](../main/argus_authority_mgr.c)

- Define `argus_authority_core_t` struct containing `mode`, `owner`, `generation`, and `last_error`.
- Keep `argus_authority_snapshot_t` as a read-only observation DTO.
- Protect production `s_authority_core` with `s_auth_mutex`.
- Re-implement `argus_authority_core_*` pure functions operating on caller-provided `argus_authority_core_t *core`.

---

### Component 2: Complete Service Transition Orchestration Seam

#### [MODIFY] [argus_net_mgr.h](../main/argus_net_mgr.h)
#### [MODIFY] [argus_net_mgr.c](../main/argus_net_mgr.c)

- Expand `argus_service_transition_ops_t` to include mandatory function pointers:
  - `request_normal_stop`
  - `verify_stopped`
  - `stop_broker`
  - `verify_broker_stopped`
  - `disconnect_sta`
  - `verify_sta_disconnected`
  - `verify_sta_ip_released`
  - `set_wifi_ap_only`
  - `verify_ap_active`
- Implement 14-step strict action -> verification ordering in `argus_net_mgr_orchestrate_service_entry()`.
- Validate all function pointers up front; return `ESP_ERR_INVALID_ARG` if any callback is missing.
- Re-implement `argus_net_mgr_request_service()` to populate `prod_ops` with real callbacks and pass `&s_authority_core` to `argus_net_mgr_orchestrate_service_entry()`.

---

### Component 3: In-Memory NVS Core & Caller-Owned Mock Storage

#### [MODIFY] [argus_nvs_config.h](../main/argus_nvs_config.h)
#### [MODIFY] [argus_nvs_config.c](../main/argus_nvs_config.c)

- Ensure `argus_nvs_core_commit()` updates `core->active_config` from `verify_slot.payload` upon verified readback.
- Support caller-owned void context in driver read/write callbacks for 100% stack-local mock storage.

---

### Component 4: Pure Unit Test Suite & Complete 23-Field Snapshot Proof

#### [MODIFY] [argus_tests_4a.c](../main/argus_tests_4a.c)

- Replace file-static `s_mock_nvs` with caller-owned stack-local `mock_nvs_store_t local_mock` in each test.
- Refactor all 18 test cases:
  - MAC derivation: pure stack-local MAC injection.
  - Command router gate: stack-local authority core.
  - Console verbosity: pure policy evaluation on stack-local struct.
  - One-shot status: pure snapshot status formatting function.
  - Add stage-by-stage mock failure injection tests for all 14 orchestrator stages.
- Expand `argus_prod_snapshot_t` and `compare_prod_snapshots()` to capture and compare all 23 fields.
- Make suite pass/fail reporting explicit (`PHASE 4A PURE UNIT TEST SUITE: PASSED` vs `FAILED`).

---

### Component 5: Documentation

#### [MODIFY] [PHASE_4A_RUNTIME_ACCEPTANCE.md](../docs/PHASE_4A_RUNTIME_ACCEPTANCE.md)

- Update documentation to reflect operator-supplied prior runtime results (17/18 cases passed, line 178 failure) and document current hardening status as `IMPLEMENTED AND COMPILED — PENDING SHAWN'S RUNTIME TEST`.

---

## Verification Plan

### Automated Static Call-Graph Audit
Run PowerShell static audit script proving zero production calls in `main/argus_tests_4a.c`:
```powershell
Get-ChildItem -Path main\argus_tests_4a.c | Select-String -Pattern "argus_mqtt_broker_|nvs_flash_|nvs_open|esp_wifi_|xTaskCreate|vTaskDelete|esp_efuse_"
```

### Uncontested Clean Build Verification
Execute full-clean build and diff checks:
```powershell
. C:\Espressif\tools\Microsoft.v5.5.3.PowerShell_profile.ps1
idf.py fullclean
idf.py build
idf.py size
git diff --check
git status --short
```

### Stop Gate
Do not flash hardware, do not run physical tests, do not commit/push/tag. Output final evidence report ending with required status block.
