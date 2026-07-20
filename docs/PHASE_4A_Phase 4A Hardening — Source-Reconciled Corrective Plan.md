# Phase 4A Hardening — Source-Reconciled Corrective Plan

All symbol names, file locations, line numbers, and lock scopes below are verified against the `phase4a-hardening-audit` branch at SHA `bdb3b34`. No file paths, variable names, or nesting claims are inferred — each was confirmed by exhaustive search.

> [!IMPORTANT]
> Do not modify source until Shawn explicitly approves this plan.

---

## 1. Root Cause: Recursive Dispatch-Mutex Deadlock

### Confirmed Deadlock Call Graph

```text
argus_net_mgr_request_service()                [argus_net_mgr.c:515]
  L525: xSemaphoreTake(s_net_mutex)            ← LOCK #1
  L541: argus_cmd_router_lock_dispatch()        ← LOCK #2 (s_dispatch_mutex)
  L559: argus_net_mgr_orchestrate_service_entry()
    L337: auth_ops->prepare_transition()
      = prod_prepare_transition()               [argus_authority_mgr.c:99]
        = argus_authority_prepare_service_transition() [argus_authority_mgr.c:162]
          L165: argus_cmd_router_lock_dispatch() ← ATTEMPTS LOCK #2 AGAIN → DEADLOCK
```

`s_dispatch_mutex` is a non-recursive `xSemaphoreCreateMutexStatic` ([argus_cmd_router.c:20](../main/argus_cmd_router.c#L20)). Calling `xSemaphoreTake` on a mutex already held by the same task blocks forever.

**Every runtime call to `argus_net_mgr_request_service()` from [app_main.c](../main/app_main.c) (lines 453, 554, 886) is affected.**

### Fix

Refactor `argus_authority_prepare_service_transition()` to remove its internal `argus_cmd_router_lock_dispatch()` / `unlock_dispatch()` calls. The caller (`argus_net_mgr_request_service`) already holds `s_dispatch_mutex` before calling the orchestrator. The authority function will only perform the mutex-protected authority-core state update (acquire `s_auth_mutex`, set mode, increment generation, release `s_auth_mutex`) and the controlled stop sequence — **without touching dispatch**.

The `prod_prepare_transition` callback will call a **new** internal function `argus_authority_prepare_service_transition_locked()` that assumes dispatch is already held externally.

`argus_authority_request_service()` (standalone bypass) also calls `argus_authority_prepare_service_transition()`. See Finding 2 for its removal.

### Corrected Production Call Graph

```text
argus_net_mgr_request_service(requested_owner) [argus_net_mgr.c]
  ├── xSemaphoreTake(s_net_mutex)
  │   Validates s_net_mode is AP_DISCOVERABLE or UNCOMMISSIONED_AP
  │   Validates s_ap_started == true
  │
  ├── argus_cmd_router_lock_dispatch()   ← s_dispatch_mutex acquired ONCE
  │
  ├── argus_net_mgr_orchestrate_service_entry(...)
  │   ├── auth_ops->prepare_transition()
  │   │   └── prod_prepare_transition()
  │   │       └── argus_authority_prepare_service_transition_locked()
  │   │           ├── argus_authority_mgr_set_mode(SERVICE_TRANSITION, NONE)
  │   │           │   └── [s_auth_mutex: take → set → give]
  │   │           ├── argus_state_mgr_stop_normal()
  │   │           │   └── [s_command_mutex → s_state_mutex → motion ops]
  │   │           └── Poll for HOLDING/UNLOCKED (bounded 5.0s)
  │   │
  │   ├── ops->request_normal_stop()
  │   ├── ops->verify_stopped()          (bounded 5.0s)
  │   ├── ops->stop_broker()
  │   ├── ops->verify_broker_stopped()   (bounded 2.0s)
  │   ├── ops->disconnect_sta()
  │   ├── ops->verify_sta_disconnected() (bounded 2.0s)
  │   ├── ops->verify_sta_ip_released()  (bounded 2.0s)
  │   ├── ops->set_wifi_ap_only()
  │   ├── ops->verify_ap_active()        (bounded 2.0s)
  │   ├── *net_mode = SERVICE_AP_ONLY
  │   └── auth_ops->grant_local(owner)
  │       └── prod_grant_local()
  │           └── argus_authority_grant_local_service(owner)
  │               └── [s_auth_mutex: take → set → give]
  │
  ├── argus_cmd_router_unlock_dispatch()  ← s_dispatch_mutex released (all paths)
  └── xSemaphoreGive(s_net_mutex)         ← s_net_mutex released (all paths)
```

---

## 2. Elimination of Authority-Only Bypass API

### What Exists Now

[argus_authority_mgr.h:101](../main/argus_authority_mgr.h#L101) declares `argus_authority_request_service()`. [argus_authority_mgr.c:242–247](../main/argus_authority_mgr.c#L242-L247) implements it as a simple `prepare + grant` convenience wrapper.

### Verified Scope of Impact

- **app_main.c**: Zero direct call sites (confirmed by search). All three production call sites use `argus_net_mgr_request_service`.
- **argus_tests_4a.c**: Zero direct call sites.

### Change

- **Delete** the declaration from [argus_authority_mgr.h](../main/argus_authority_mgr.h).
- **Delete** the definition from [argus_authority_mgr.c](../main/argus_authority_mgr.c).
- **Delete** the old `argus_authority_prepare_service_transition()` public API (line 77 in header, line 162 in source).
- **Replace** with `argus_authority_prepare_service_transition_locked()` — a function that does NOT acquire/release `s_dispatch_mutex`. Keep it accessible via the `prod_prepare_transition` callback in `argus_service_authority_ops_t`.

Governance: `argus_net_mgr_request_service()` is the **sole** production entry point for coordinated local-service acquisition.

---

## 3. Source-Reconciled Lock Hierarchy

### Verified Complete Inventory (9 Mutexes + 1 Spinlock)

| # | Lock Name | Actual File | Line | Type | Scope |
|:---:|:---|:---|:---:|:---|:---|
| 1 | `s_net_mutex` | [argus_net_mgr.c](../main/argus_net_mgr.c#L30) | 30 | Static Mutex | Network mode transitions, coherent state |
| 2 | `s_dispatch_mutex` | [argus_cmd_router.c](../main/argus_cmd_router.c#L13) | 13 | Static Mutex | Command envelope dispatch serialization |
| 3 | `s_command_mutex` | [argus_state_mgr.c](../main/argus_state_mgr.c#L41) | 41 | Dynamic Mutex | State-manager command serialization |
| 4 | `s_auth_mutex` | [argus_authority_mgr.c](../main/argus_authority_mgr.c#L21) | 21 | Static Mutex | Authority core read/write |
| 5 | `s_state_mutex` | [argus_state_mgr.c](../main/argus_state_mgr.c#L42) | 42 | Dynamic Mutex | State machine transitions, fault latches |
| 6 | `s_traj_mutex` | [argus_trajectory.c](../main/argus_trajectory.c#L13) | 13 | Dynamic Mutex | Trajectory ramps, velocity targets |
| 7 | `s_lifecycle_mutex` | [argus_step_gen.c](../main/argus_step_gen.c#L25) | 25 | Dynamic Mutex | Step generator lifecycle (arm, start, stop, direction) |
| 8 | `s_timing_mux` | [argus_step_gen.c](../main/argus_step_gen.c#L32) | 32 | portMUX_TYPE (spinlock) | GPTimer pulse timing (ISR-safe) |
| 9 | `s_broker.lock` | [argus_mqtt_broker.c](../main/argus_mqtt_broker.c#L44) | 44 | Dynamic Mutex | Broker client slot allocation, publish, stop |
| 10 | `s_ctx.lock` | [argus_stepper.c](../main/argus_stepper.c#L31) | 31 | Dynamic Mutex | Legacy stepper driver (independent, not in V2 motion stack) |

### Verified Nesting Order (Maximum Depth Path)

```text
s_net_mutex                          [argus_net_mgr.c]
 └→ s_dispatch_mutex                 [argus_cmd_router.c]
     └→ s_auth_mutex (brief)         [argus_authority_mgr.c]
     └→ s_command_mutex              [argus_state_mgr.c]
         └→ s_state_mutex            [argus_state_mgr.c]
             └→ [motion ops]
                 └→ s_traj_mutex     [argus_trajectory.c]
                     └→ s_lifecycle_mutex [argus_step_gen.c]
                         └→ s_timing_mux (spinlock) [argus_step_gen.c]
```

### Hierarchy Rules

1. **Strict downward-only**: Lower locks never call APIs that acquire higher locks.
2. **E-stop bypass**: `argus_state_mgr_estop()` ([argus_state_mgr.c:480–487](../main/argus_state_mgr.c#L480-L487)) intentionally bypasses `s_command_mutex` — acquires only `s_state_mutex` for immediate preemption.
3. **E-stop dispatch bypass**: `argus_cmd_router_dispatch()` ([argus_cmd_router.c:44–47](../main/argus_cmd_router.c#L44-L47)) with `ARGUS_CMD_SRC_INTERNAL_SAFETY` or `ARGUS_CMD_TYPE_ESTOP` bypasses `s_dispatch_mutex` entirely and calls `argus_state_mgr_estop()` directly.
4. **Broker isolation**: `s_broker.lock` is independent of the authority/state hierarchy. The `on_message` callback is invoked **after** the lock is released.
5. **Network event callbacks**: ESP-IDF Wi-Fi and IP event callbacks execute in the system event-loop task, **not ISR context**. They modify `volatile bool` flags (`s_sta_connected`, `s_sta_ip_acquired`, `s_ap_started`) without acquiring `s_net_mutex`. The proposed event-group additions (`xEventGroupSetBits`/`ClearBits`) will be called from these callbacks. Network event callbacks do not acquire `s_net_mutex`; therefore the proposed event-bit waits introduce no `s_net_mutex` dependency in that callback path.

### Confirmations

- Software E-stop does **not** require `s_dispatch_mutex` — confirmed at [argus_cmd_router.c:44–47](../main/argus_cmd_router.c#L44-L47).
- The broker server task does **not** acquire `s_net_mutex` — confirmed by exhaustive search of [argus_mqtt_broker.c](../main/argus_mqtt_broker.c).
- `s_auth_mutex` is a **leaf lock** — confirmed: only `get_snapshot` and `set_mode` acquire it, neither calls any function that acquires another lock.

---

## 4. MQTT Broker Lifecycle Hardening

### Current State (Defects Found)

Source: [argus_mqtt_broker.c](../main/argus_mqtt_broker.c) (688 lines)

| # | Defect | Evidence |
|---|--------|----------|
| 1 | No task handle stored — `xTaskCreate` at L631 passes `NULL` for handle | Cannot wait for task exit |
| 2 | `stop()` closes sockets (L663–682) but does not wait for server/client tasks to terminate | Race on start-after-stop |
| 3 | `start()` does `memset(&s_broker, 0, ...)` at L595, leaking the old mutex (never calls `vSemaphoreDelete`) | Semaphore leak |
| 4 | `is_running()` at L653 reads `s_broker.started` without the lock | Formally unsynchronized |
| 5 | State tracked by a single `bool started` — no intermediate STARTING/STOPPING states | Cannot distinguish startup failure from not-started |

### Proposed Changes

1. **Add broker state enum**: `BROKER_STOPPED`, `BROKER_STARTING`, `BROKER_RUNNING`, `BROKER_STOPPING`.
2. **Store server task handle**: Save the `TaskHandle_t` from `xTaskCreate`.
3. **Add event group `s_broker_event_group`** with bits `BROKER_EVT_STARTED` and `BROKER_EVT_STOPPED`.
4. **Server task signals on lifecycle transitions**: Sets `BROKER_EVT_STARTED` on successful bind+listen, or `BROKER_EVT_STOPPED` on failure/exit.
5. **`start()` waits** bounded (2000ms) on `xEventGroupWaitBits` for either bit.
6. **`stop()` waits** bounded (2000ms) on `BROKER_EVT_STOPPED` after closing sockets.
7. **`stop()` calls `vSemaphoreDelete`** after confirmed task exit, before returning.
8. **`start()` does NOT `memset` while old mutex is live** — clean up properly first.

### `is_running()` Behavior

Returns `true` only when broker state == `BROKER_RUNNING`. Reads state under `s_broker.lock`.

---

## 5. NVS Observation Architecture

### Current API

[argus_nvs_config.h:143](../main/argus_nvs_config.h#L143):
```c
esp_err_t argus_nvs_config_get_observation_snapshot(uint8_t *out_selector,
    argus_cfg_slot_t *out_slot_a, argus_cfg_slot_t *out_slot_b);
```

[Current implementation](../main/argus_nvs_config.c#L569-L583) always returns `ESP_OK` and does not report whether NVS reads actually succeeded or whether the device is uncommissioned (NVS absent).

### Change

Add a richer observation struct:

```c
typedef struct {
    esp_err_t selector_status;   // ESP_OK, ESP_ERR_NOT_FOUND, or actual error
    bool      selector_present;  // true if selector_status == ESP_OK
    uint8_t   selector;          // valid only if selector_present

    esp_err_t slot_a_status;
    bool      slot_a_present;
    bool      slot_a_valid;      // valid_marker matches AND CRC passes
    argus_cfg_slot_t slot_a;

    esp_err_t slot_b_status;
    bool      slot_b_present;
    bool      slot_b_valid;
    argus_cfg_slot_t slot_b;
} argus_nvs_observation_t;
```

Update `argus_nvs_config_get_observation_snapshot` signature to accept `argus_nvs_observation_t *out_obs` and return `esp_err_t`. Return `ESP_OK` if all reads returned either `ESP_OK` or `ESP_ERR_NOT_FOUND`. Return the first unexpected error otherwise.

The production snapshot (`capture_prod_snapshot`) will use this struct and propagate any unexpected NVS error to the test runner.

---

## 6. Pure-Test Isolation (Test Suite Defects)

### Current State

[argus_tests_4a.c](../main/argus_tests_4a.c) (848 lines) contains production API calls that violate the pure-test constraint:

| Line | Production Call | Violation |
|:---:|:---|:---|
| 536 | `argus_cmd_router_lock_dispatch()` | Acquires production `s_dispatch_mutex` |
| 543 | `argus_cmd_router_dispatch(&estop_env)` | Invokes production dispatch (E-stop) |
| 544 | `argus_cmd_router_unlock_dispatch()` | Releases production `s_dispatch_mutex` |

These occur in Test 16 (`test_network_truthfulness_and_broker_ordering`). The test takes the production dispatch mutex, then dispatches a real E-stop command through the production router.

> [!WARNING]
> This is a production state mutation from a "pure" test. The E-stop fires through `argus_state_mgr_estop()` which writes to `s_prod_core` under `s_state_mutex`, potentially latching `estop_latched = true` in production state.

### Fix

Replace the live dispatch/E-stop calls with a **stack-local mock simulation**:

1. Remove `argus_cmd_router_lock_dispatch()`, `argus_cmd_router_dispatch()`, `argus_cmd_router_unlock_dispatch()` calls from Test 16.
2. Instead, use a stack-local `mock_net_ops_ctx_t` with `estop_during_stop = true` and `fail_stage = 5`, exactly like the existing E-stop-during-deceleration test path at line 625.
3. Assert:
   - Return is `ESP_ERR_INVALID_STATE`
   - Stack-local authority ends at `NONE / NONE`
   - Stack-local network mode ends at `NETWORK_FAULT`
   - `grant_local` was never called
   - No production APIs were invoked

### Production Snapshot Guard (Retained)

The `run_all` function's before/after `capture_prod_snapshot` + `compare_prod_snapshots` mechanism ([argus_tests_4a.c:776–814](../main/argus_tests_4a.c#L776-L814)) is a **read-only observation**, not a test. It reads 11 production APIs (lines 705–715) and compares 31 fields (lines 730–760). This mechanism is retained as-is — it does not mutate production state.

---

## 7. Failure Injection Matrix Corrections

### Current Stage 3 Gap

The failure injection loop ([argus_tests_4a.c:592–614](../main/argus_tests_4a.c#L592-L614)) iterates stages 2–13. Stage 3 injects **no error** — none of the mock callbacks check `fail_stage == 3`. The orchestrator's internal step 3 maps to no distinct injectable callback. The test asserts `res != ESP_OK` for all stages, but stage 3 may silently pass.

### Fix

Either:
- **(A)** Remove stage 3 from the loop (iterate 2, then 4–13), since there is no distinct stage-3 callback to inject, OR
- **(B)** Map stage 3 to a second prepare-transition failure variant (e.g., return `ESP_FAIL` instead of `ESP_ERR_INVALID_STATE`) to distinguish from stage 2.

Prefer **(A)** for honesty. The orchestrator has exactly 11 injectable callbacks (stages 2, 4–13). Stage 3 is a phantom.

### Failure Cutoff Table (Reconciled)

| Stage | Injected Callback | Injected Error | Expected Call Count |
|:---:|:---|:---|:---:|
| 2 | `prepare_transition` | `ESP_ERR_INVALID_STATE` | 0 transition ops |
| 4 | `request_normal_stop` | `ESP_ERR_TIMEOUT` | 1 |
| 5 | `verify_stopped` | `ESP_ERR_INVALID_STATE` | 2 |
| 6 | `stop_broker` | `ESP_ERR_INVALID_STATE` | 3 |
| 7 | `verify_broker_stopped` | `ESP_ERR_TIMEOUT` | 4 |
| 8 | `disconnect_sta` | `ESP_ERR_INVALID_STATE` | 5 |
| 9 | `verify_sta_disconnected` | `ESP_ERR_TIMEOUT` | 6 |
| 10 | `verify_sta_ip_released` | `ESP_ERR_TIMEOUT` | 7 |
| 11 | `set_wifi_ap_only` | `ESP_ERR_INVALID_STATE` | 8 |
| 12 | `verify_ap_active` | `ESP_ERR_TIMEOUT` | 9 |
| 13 | `grant_local` | `ESP_ERR_INVALID_ARG` | 9 (+1 failed grant) |

All stages assert: `abort_transition` called once, `net_mode == NETWORK_FAULT`, authority `NONE/NONE`, error code preserved through orchestrator.

---

## 8. Network Event Group Introduction

### Current State

No FreeRTOS Event Groups exist in the codebase. The `volatile bool` flags `s_sta_connected`, `s_sta_ip_acquired`, `s_ap_started` in [argus_net_mgr.c:55–58](../main/argus_net_mgr.c#L55-L58) are set directly in Wi-Fi/IP event callbacks and polled via spin-loops in the verification callbacks.

### Change

Add `s_net_event_group` with defined bits:
- `NET_BIT_STA_CONNECTED`
- `NET_BIT_STA_IP_ACQUIRED`
- `NET_BIT_AP_STARTED`

Event callbacks call `xEventGroupSetBits` / `xEventGroupClearBits` (NOT the `FromISR` variants — Wi-Fi and IP callbacks execute in the system event-loop task, not ISR context).

Verification callbacks (`prod_verify_sta_disconnected`, `prod_verify_sta_ip_released`, `prod_verify_ap_active`) use bounded `xEventGroupWaitBits` instead of polling loops.

The `volatile bool` flags are retained as secondary reads for `argus_net_mgr_is_*` public getters that don't need blocking waits.

---

## 9. Files to Change

| # | File | Changes |
|:---:|:---|:---|
| 1 | [argus_authority_mgr.h](../main/argus_authority_mgr.h) | Delete `argus_authority_request_service()` (L101). Delete `argus_authority_prepare_service_transition()` (L77). Add `argus_authority_prepare_service_transition_locked()` declaration. |
| 2 | [argus_authority_mgr.c](../main/argus_authority_mgr.c) | Delete `argus_authority_request_service()` (L242–247). Refactor `argus_authority_prepare_service_transition()` into `_locked()` variant that does NOT touch `s_dispatch_mutex`. Update `prod_prepare_transition` callback to call the `_locked()` variant. Delete `argus_authority_request_exit()`'s `lock_dispatch` / `unlock_dispatch` pair (L252, L258) — it must also be called from within an already-held dispatch context, or refactored to acquire dispatch itself as the sole entry point for exit. |
| 3 | [argus_net_mgr.c](../main/argus_net_mgr.c) | Add `s_net_event_group`. Replace volatile-polling verification callbacks with `xEventGroupWaitBits`. Add `#include "freertos/event_groups.h"`. Add `argus_net_mgr_get_snapshot()` for coherent read under `s_net_mutex`. |
| 4 | [argus_net_mgr.h](../main/argus_net_mgr.h) | Declare `argus_net_snapshot_t` and `argus_net_mgr_get_snapshot()`. |
| 5 | [argus_mqtt_broker.c](../main/argus_mqtt_broker.c) | Add broker state enum. Store `TaskHandle_t`. Add `s_broker_event_group`. Implement lifecycle signal handshake in server task. Implement bounded `start()`/`stop()` with event-group waits. Fix semaphore leak on restart. |
| 6 | [argus_mqtt_broker.h](../main/argus_mqtt_broker.h) | Declare any new lifecycle queries if needed. |
| 7 | [argus_nvs_config.h](../main/argus_nvs_config.h) | Add `argus_nvs_observation_t` struct. Update `argus_nvs_config_get_observation_snapshot` signature. |
| 8 | [argus_nvs_config.c](../main/argus_nvs_config.c) | Implement error-checked observation snapshot with status/presence/validity per slot. |
| 9 | [argus_tests_4a.c](../main/argus_tests_4a.c) | Remove live dispatch/E-stop calls (L536, 543, 544). Replace with stack-local mock E-stop simulation. Fix stage 3 gap. Update `argus_prod_snapshot_t` to use `argus_nvs_observation_t`. |
| 10 | [app_main.c](../main/app_main.c) | No changes needed — all three service-entry call sites already route through `argus_net_mgr_request_service`. |
| 11 | [docs/PHASE_4A_RUNTIME_ACCEPTANCE.md](../docs/PHASE_4A_RUNTIME_ACCEPTANCE.md) | Update documentation record after implementation. |

> [!WARNING]
> `argus_authority_request_exit()` ([argus_authority_mgr.c:249–297](../main/argus_authority_mgr.c#L249-L297)) also calls `argus_cmd_router_lock_dispatch()` at L252. This exit path is called from `net_mgr_task()` SERVICE_EXIT handler while `s_net_mutex` is already held ([argus_net_mgr.c:131–134](../main/argus_net_mgr.c#L131-L134)). This is not a recursive deadlock (it's `s_net_mutex → s_dispatch_mutex`, which is the correct ordering), but it uses the same `lock_dispatch` pattern internally. Refactoring `request_exit` for consistency is recommended but not blocking — it follows the correct hierarchy order. Document this path explicitly in the lock doctrine.

## Open Questions

> [!IMPORTANT]
> **Q1: `argus_authority_request_exit()` refactoring scope.**
> Should the exit path also be refactored to use a `_locked()` variant called from within `argus_net_mgr` (mirroring the entry path)? Or is the current `s_net_mutex → s_dispatch_mutex` (correct order) acceptable as-is?

> [!IMPORTANT]
> **Q2: Stage 3 failure injection.**
> Prefer option (A): Remove stage 3 from the loop entirely since there is no injectable callback at that point. Confirm.

> [!IMPORTANT]
> **Q3: `s_ctx.lock` (legacy stepper, [argus_stepper.c](../main/argus_stepper.c)).**
> This is a legacy stepper driver with its own independent mutex. It is not used in the V2 motion stack. Is it safe to exclude it from the lock hierarchy table? Confirm it is not called from any V2 production path.

---

## 10. Verification Plan

### Build Verification

```powershell
. C:\Espressif\tools\Microsoft.v5.5.3.PowerShell_profile.ps1
idf.py fullclean
idf.py build
idf.py size
git diff --check
git status --short
```

### Static Verification

After implementation, run:
```powershell
Select-String -Path "main\*.c","main\*.h" -Pattern "argus_authority_request_service" | ForEach-Object { $_.Filename + ":" + $_.LineNumber }
```
Expect: zero results.

### Physical Verification

- Physical on-device execution: **PENDING SHAWN'S RUNTIME TEST**
- Hardware flashing: **PENDING OPERATOR**
- Main branch merge / push / tagging: **STOPPED / PENDING SHAWN'S REVIEW**

```text
Deadlock fix:                         PLANNED
Bypass API removal:                   PLANNED
MQTT broker lifecycle hardening:      PLANNED
NVS observation upgrade:              PLANNED
Pure-test isolation fix:              PLANNED
Event group introduction:             PLANNED
Failure matrix stage 3 fix:           PLANNED
Final binary evidence:                PENDING IMPLEMENTATION
```
