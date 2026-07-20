# Implementation Plan - Phase 4A Pure-Test Isolation Correction

## Goal Description
Correct the Phase 4A pure non-motion unit test suite (`t`) so that it executes 100% stack-local, pure unit tests with **zero** mutations to live production singletons, **zero** real MQTT broker operations, **zero** live network manager event posting, **zero** ESP32 NVS flash/driver operations, **zero** FreeRTOS task creation/deletion, and **zero** asynchronous work.

---

## User Review Required

> [!IMPORTANT]
> **Governing Rule: The Production Controller Is Off-Limits During `t`**
> During `t`, tests perform zero production authority mutations, zero authority generation increments, zero network mode mutations, zero Wi-Fi driver calls, zero real MQTT broker start/stops, zero production NVS operations, and zero production singleton replacement or restoration.
>
> Tests do not borrow the production singletons and put them back afterward—they never touch them.

---

## Proposed Changes

### Component 1: Stack-Local Authority Core

#### [MODIFY] [argus_authority_mgr.h](../main/argus_authority_mgr.h)
#### [MODIFY] [argus_authority_mgr.c](../main/argus_authority_mgr.c)

- Extract pure, caller-provided authority state transition logic (`argus_authority_core_*`) operating on stack-local `argus_authority_snapshot_t` instances.
- Re-implement production `argus_authority_mgr` functions as wrappers around `&s_authority`.
- Allow unit tests to test `SERVICE_TRANSITION`, `LOCAL_SERVICE`, aborts, and permission checks purely on stack-local snapshots without incrementing production `s_authority.generation`.

---

### Component 2: Injected Mock Operations Seam for Network & Service Entry

#### [MODIFY] [argus_net_mgr.h](../main/argus_net_mgr.h)
#### [MODIFY] [argus_net_mgr.c](../main/argus_net_mgr.c)

- Define `argus_service_transition_ops_t` containing function pointers for `stop_broker`, `disconnect_sta`, `set_wifi_ap_only`, `verify_broker_stopped`, `verify_sta_disconnected`, and `verify_ap_active`.
- Extract pure service entry orchestration function `argus_net_mgr_orchestrate_service_entry()` accepting stack-local authority snapshot, network mode, and `const argus_service_transition_ops_t *ops`.
- Production `argus_net_mgr_request_service()` calls this orchestration with real production function pointers.
- Unit tests pass stack-local mock operation structs that record invocation order, inputs, and injected failures without touching the real broker, Wi-Fi driver, or event queue.

---

### Component 3: Phase 4A Test Suite & Non-Mutation Proof

#### [MODIFY] [argus_tests_4a.c](../main/argus_tests_4a.c)

- Update all 18 test cases to operate 100% synchronously on stack-local memory structures, stack-local mock NVS drivers, stack-local authority snapshots, and stack-local mock transition operations.
- Completely remove all calls to live `nvs_flash_init`, `argus_net_mgr_post_event`, real `argus_authority_prepare_service_transition`, and production NVS driver re-initialization.
- Update `capture_prod_snapshot()` to record:
  - Full machine state snapshot
  - Configured target, applied target, trajectory target, generated step count
  - Driver enabled state, E-stop latch, fault code
  - Network mode, last network error
  - Authority mode, authority owner, authority generation
  - Active NVS selector, Slot A/B metadata/CRC/validity
  - FreeRTOS task count
- Remove `restore_prod_snapshot()`. Production state must be 100% UNCHANGED naturally.
- Format summary report:
  ```text
  Phase 3B Pure Tests: PASSED

  Phase 4A Pure Tests:
    Distinct Test Cases : 18
    Repeat Passes       : 3
    Total Executions    : 54
    Failed Executions   : 0

  Production Isolation:
    Authority Generation : UNCHANGED
    Network State         : UNCHANGED
    MQTT Broker State     : UNCHANGED
    NVS State             : UNCHANGED
    Machine State         : UNCHANGED
    Task Count            : UNCHANGED

  Production Non-Mutation Proof: PASSED
  ```

---

### Component 4: Documentation

#### [MODIFY] [PHASE_4A_RUNTIME_ACCEPTANCE.md](../docs/PHASE_4A_RUNTIME_ACCEPTANCE.md)

- Update documentation to record that Scenario C is `COMPLETE — PHYSICALLY VERIFIED`, the pure-test isolation correction is implemented and compiled, and Scenario D is pending Shawn's physical testing.

---

## Verification Plan

### Automated Static Audit
Run PowerShell static audit script proving zero production calls in `main/argus_tests_4a.c`:
```powershell
Get-ChildItem -Path main\argus_tests_4a.c | Select-String -Pattern "argus_mqtt_broker_(start|stop)|nvs_flash_|nvs_open|esp_wifi_|xTaskCreate|vTaskDelete"
```

### Clean Build Verification
Execute full-clean build:
```powershell
. C:\Espressif\tools\Microsoft.v5.5.3.PowerShell_profile.ps1
idf.py fullclean
idf.py build
idf.py size
git diff --check
git status --short
```

### Stop Gate
Do not flash hardware, do not run physical tests, do not commit/push/tag. Output final evidence report.
