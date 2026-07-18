# Phase 4A Hardening Audit Corrective Plan (Amended)

This implementation plan corrects all eight blocking architectural findings identified during the independent audit of branch `phase4a-hardening-audit`. It eliminates lock deadlocks, bypass routes, race conditions, unverified MQTT task exits, production test contamination, and incomplete snapshot observation errors.

---

## 1. Root Cause Analysis & Corrected Call Graph

### Root Cause of Blocking Finding 1 (Deadlock)
`argus_net_mgr_request_service()` acquired `s_net_mutex` and `s_dispatch_mutex`. In `orchestrate_service_entry()`, it called `auth_ops->prepare_transition()`, which invoked `argus_authority_prepare_service_transition()`. That function attempted to call `argus_cmd_router_lock_dispatch()` a second time. Because `s_dispatch_mutex` is a non-recursive FreeRTOS mutex, the calling task deadlocked on itself.

### Corrected Call Graph for Coordinated Service Entry

```text
User / Menu / Web API
  │
  ▼
argus_net_mgr_request_service(requested_owner)
  │
  ├─► Acquire s_net_mutex
  │     (Validates net_mode is AP_DISCOVERABLE/UNCOMMISSIONED_AP and s_ap_started)
  │
  ├─► Acquire s_dispatch_mutex via argus_cmd_router_lock_dispatch()
  │     (Waits for any in-flight normal command to exit dispatch gate)
  │
  ├─► argus_net_mgr_orchestrate_service_entry(...)
  │     │
  │     ├─► auth_ops->prepare_transition(auth_ops->ctx)
  │     │     └─► Mutex-protected update inside argus_authority_mgr.c:
  │     │           s_authority.mode = ARGUS_AUTHORITY_SERVICE_TRANSITION;
  │     │           s_authority.owner = ARGUS_AUTH_OWNER_NONE;
  │     │           s_authority.generation++;
  │     │           (DOES NOT LOCK DISPATCH MUTEX AGAIN)
  │     │
  │     ├─► ops->request_normal_stop(ops->ctx)
  │     │     └─► Issues controlled decelerating stop
  │     │
  │     ├─► ops->verify_stopped(ops->ctx)
  │     │     └─► Bounded poll (5.0s) for machine state HOLDING or UNLOCKED
  │     │           (Aborts immediately if E-stop latched or FAULTED)
  │     │
  │     ├─► ops->stop_broker(ops->ctx)
  │     │     └─► Sets BROKER_STATE_STOPPING & closes listener socket
  │     │
  │     ├─► ops->verify_broker_stopped(ops->ctx)
  │     │     └─► Bounded poll (2.0s) for MQTT server task exit signal
  │     │
  │     ├─► ops->disconnect_sta(ops->ctx)
  │     │     └─► Calls esp_wifi_disconnect()
  │     │
  │     ├─► ops->verify_sta_disconnected(ops->ctx)
  │     │     └─► Bounded event wait (2.0s) for NET_BIT_STA_CONNECTED cleared
  │     │
  │     ├─► ops->verify_sta_ip_released(ops->ctx)
  │     │     └─► Bounded event wait (2.0s) for NET_BIT_STA_IP_ACQUIRED cleared
  │     │
  │     ├─► ops->set_wifi_ap_only(ops->ctx)
  │     │     └─► Calls esp_wifi_set_mode(WIFI_MODE_AP)
  │     │
  │     ├─► ops->verify_ap_active(ops->ctx)
  │     │     └─► Bounded check (2.0s) for WIFI_MODE_AP && NET_BIT_AP_STARTED
  │     │
  │     ├─► Set net_mode = SERVICE_AP_ONLY
  │     │
  │     └─► auth_ops->grant_local(auth_ops->ctx, requested_owner)
  │           └─► Mutex-protected update inside argus_authority_mgr.c:
  │                 s_authority.mode = ARGUS_AUTHORITY_LOCAL_SERVICE;
  │                 s_authority.owner = requested_owner;
  │
  ├─► Release s_dispatch_mutex via argus_cmd_router_unlock_dispatch() [ON ALL PATHS]
  │
  └─► Release s_net_mutex [ON ALL PATHS]
```

---

## 2. Lock Ownership & Network Synchronization Architecture

### Lock Ownership Table

| Lock Name | Declared Location | Owned Scope | Mutex Type | Nested Locks Allowed | Holding During Async/Net Waits |
| :--- | :--- | :--- | :--- | :--- | :--- |
| `s_net_mutex` | `main/argus_net_mgr.c` | Network mode transitions & coherent state snapshots | Non-recursive FreeRTOS Mutex | May call `s_dispatch_mutex` & `s_auth_mutex` | Yes (Protected by lock-free event bits) |
| `s_dispatch_mutex` | `main/argus_cmd_router.c` | Serializes command envelope execution & authority mode transitions | Non-recursive FreeRTOS Mutex | May call `s_auth_mutex` & `s_state_mutex` | Yes (Bounded transition sequence) |
| `s_auth_mutex` | `main/argus_authority_mgr.c` | Atomic read/write of private `s_authority` core state | Non-recursive FreeRTOS Mutex | NONE (Leaf lock) | NO (Microsecond atomic updates only) |
| `s_state_mutex` | `main/argus_state_mgr.c` | Atomic state machine transitions & trajectory updates | Non-recursive FreeRTOS Mutex | NONE (Leaf lock) | NO (Microsecond atomic updates only) |

### Event-Driven Network Lifecycle Synchronization

Network lifecycle bits are managed via a FreeRTOS Event Group (`s_net_event_group`):
- `NET_BIT_STA_STARTED` (`1 << 0`)
- `NET_BIT_STA_CONNECTED` (`1 << 1`)
- `NET_BIT_STA_IP_ACQUIRED` (`1 << 2`)
- `NET_BIT_AP_STARTED` (`1 << 3`)

ESP-IDF Wi-Fi and IP event callbacks run in the system event-loop task context (`sys_evt`). Callbacks update bits using standard task-context APIs:
- `xEventGroupSetBits(s_net_event_group, bits)`
- `xEventGroupClearBits(s_net_event_group, bits)`

> Network event callbacks do not acquire `s_net_mutex`; therefore the proposed event-bit waits introduce no `s_net_mutex` dependency in that callback path.

### Coherent Network Snapshot API & Private Helpers

To prevent recursive locking on `s_net_mutex` when internal network functions operate while already holding `s_net_mutex`:
1. **Private `_locked` Helpers**: Functions internal to `argus_net_mgr.c` (such as `set_net_mode_locked()` and `get_net_mode_locked()`) operate on state variables while `s_net_mutex` is already held by the caller.
2. **Coherent Public Snapshot API**: Public callers (including the production snapshot guard) call `argus_net_mgr_get_snapshot()`, which acquires `s_net_mutex` once and populates a coherent `argus_net_snapshot_t`:

```c
typedef struct {
    argus_network_mode_t net_mode;
    argus_net_err_t last_net_err;
    wifi_mode_t wifi_driver_mode;
    bool sta_started;
    bool sta_connected;
    bool sta_ip_acquired;
    bool ap_started;
} argus_net_snapshot_t;

esp_err_t argus_net_mgr_get_snapshot(argus_net_snapshot_t *out);
```

---

## 3. MQTT Broker Task Lifecycle & Event Group Synchronization (Finding 4)

The MQTT broker lifecycle is governed by an explicit state machine and synchronized via a dedicated FreeRTOS Event Group (`s_broker_event_group`):

- `BROKER_EVT_STARTED` (`1 << 0`)
- `BROKER_EVT_STOPPED` (`1 << 1`)

### Broker Lifecycle State Table

| Current State | Requested Action | Target State | Operation / Synchronization | Return Result |
| :--- | :--- | :--- | :--- | :--- |
| `STOPPED` | `start` | `STARTING` -> `RUNNING` | Clears `BROKER_EVT_STOPPED`, spawns `argus_mqtt_server_task`. On listener bind success, sets `BROKER_EVT_STARTED` & state `RUNNING`. On failure, cleans up and reverts to `STOPPED`. | `ESP_OK` / `ESP_FAIL` |
| `STOPPED` | `stop` | `STOPPED` | Idempotent no-op. | `ESP_OK` |
| `STARTING` | `start` | `STARTING` | Rejects duplicate start request. | `ESP_ERR_INVALID_STATE` |
| `STARTING` | `stop` | `STOPPING` -> `STOPPED` | Requests cancellation, waits on `BROKER_EVT_STOPPED` (bounded 2000ms). | `ESP_OK` / `ESP_ERR_TIMEOUT` |
| `RUNNING` | `start` | `RUNNING` | Rejects duplicate start request. | `ESP_ERR_INVALID_STATE` |
| `RUNNING` | `stop` | `STOPPING` -> `STOPPED` | Sets state `STOPPING`, calls `shutdown(listener, SHUT_RDWR)` & `close()`, closes client sockets, waits on `xEventGroupWaitBits(s_broker_event_group, BROKER_EVT_STOPPED, ...)` up to 2000ms. On exit confirmation, returns `ESP_OK`. On timeout, leaves state as `STOPPING` and returns `ESP_ERR_TIMEOUT`. | `ESP_OK` / `ESP_ERR_TIMEOUT` |
| `STOPPING` | `start` | `STOPPING` | Rejects start while stopping. | `ESP_ERR_INVALID_STATE` |
| `STOPPING` | `stop` | `STOPPING` -> `STOPPED` | Re-waits on existing `BROKER_EVT_STOPPED` event bit; does not spawn duplicate shutdown paths. | `ESP_OK` / `ESP_ERR_TIMEOUT` |

### Server Task Exit Behavior
1. `argus_mqtt_server_task()` checks `s_broker_state` after every socket wake or `accept()` error.
2. When `s_broker_state == BROKER_STATE_STOPPING`, task exits main loop.
3. Closes remaining active descriptors and clears `s_broker_task_handle = NULL`.
4. Mutex-protected transition: sets `s_broker_state = BROKER_STATE_STOPPED`.
5. Calls `xEventGroupSetBits(s_broker_event_group, BROKER_EVT_STOPPED)`.
6. Calls `vTaskDelete(NULL)` to terminate cleanly.

---

## 4. Production Call-Site Migration List (Finding 2)

All production code paths requesting local service entry are migrated to invoke `argus_net_mgr_request_service(requested_owner)`. Direct calls to `argus_authority_request_service()` outside `argus_net_mgr.c` are deleted.

| Call-Site File | Function / Section | Old Direct Call | New Orchestrated Call | Failure Handling |
| :--- | :--- | :--- | :--- | :--- |
| `main/app_main.c` | Menu Option `N -> 5` | `argus_authority_request_service(CLI)` | `argus_net_mgr_request_service(ARGUS_AUTH_OWNER_DIAGNOSTIC_CLI)` | Displays error code if `!= ESP_OK`, leaves UI in ungranted state |
| `main/app_main.c` | Hardware Acceptance `H` | `argus_authority_request_service(CLI)` | `argus_net_mgr_request_service(ARGUS_AUTH_OWNER_DIAGNOSTIC_CLI)` | Aborts hardware acceptance sequence on failure |
| `main/app_main.c` | E-Stop Transition Test | `argus_authority_request_service(CLI)` | `argus_net_mgr_request_service(ARGUS_AUTH_OWNER_DIAGNOSTIC_CLI)` | Validates fail-closed state |
| `main/argus_authority_mgr.c` | Header Declaration | Exposed `argus_authority_request_service` | Removed from public header; local service entry owned by `argus_net_mgr.c` | Compile error if invoked externally |

---

## 5. Pure-Test Seam Architecture & Precise Terminology (Finding 5)

### Exact Pure-Test Terminology Definition

> The 18 Phase 4A test cases execute on stack-local cores and mocks with zero production mutation. The test runner separately performs read-only before/after production observation as a non-mutation guard.

### Test Isolation Safeguards
1. **Eliminate Live Dispatch Calls**: Remove `argus_cmd_router_dispatch(&estop_env)` and live `s_dispatch_mutex` locking from `main/argus_tests_4a.c`.
2. **Static Call-Graph Audit Assertions**: Confirm that individual test functions inside `main/argus_tests_4a.c` do NOT invoke:
   - `argus_cmd_router_dispatch`
   - Production authority transition APIs (`argus_authority_mgr_set_mode`, `argus_authority_request_exit`)
   - Production state-manager mutation APIs (`argus_state_mgr_set_target`, `argus_state_mgr_estop`)
   - MQTT broker start/stop (`argus_mqtt_broker_start`, `argus_mqtt_broker_stop`)
   - Network event posting (`argus_net_mgr_post_event`)
   - ESP Wi-Fi mutation APIs (`esp_wifi_set_mode`, `esp_wifi_disconnect`)
   - NVS flash mutation APIs (`nvs_flash_init`, `nvs_commit`, `nvs_set_blob`)
   - Task creation/deletion (`xTaskCreate`, `vTaskDelete`)
   - Hardware GPIO or GPTimer driver APIs

---

## 6. Failure-Injection Matrix & Partial-Transition Truthfulness (Finding 6 & 7)

### Partial-Transition Failure Truthfulness
When an orchestration failure occurs after steps have completed (e.g. MQTT broker stopped or STA disconnected):
- Authority state becomes `NONE / NONE`.
- Network mode becomes `NETWORK_FAULT`.
- Controlled motion is NOT restarted.
- E-stop latch state is preserved.
- **NO automatic rollback** to supervisory authority or STA connection occurs.
- Physical network state remains whatever was actually achieved (e.g., STA disconnected, broker listener stopped) and is reported truthfully via `argus_net_mgr_get_snapshot()`.
- Recovery requires an explicit recovery/service-exit action or device reboot.

### Complete Injection Table

| Stage ID | Target Operation | Injected Callback Error | Expected Preserved `esp_err_t` | Expected Final Authority Mode | Expected Final Network Mode | Expected `grant_local` Count | Expected `abort_transition` Count |
| :---: | :--- | :--- | :--- | :---: | :---: | :---: | :---: |
| 2 | Prepare Transition | `ESP_ERR_INVALID_STATE` | `ESP_ERR_INVALID_STATE` | `NONE/NONE` | `NETWORK_FAULT` | 0 | 1 |
| 4 | Request Normal Stop | `ESP_ERR_TIMEOUT` | `ESP_ERR_TIMEOUT` | `NONE/NONE` | `NETWORK_FAULT` | 0 | 1 |
| 5 | Verify Stopped | `ESP_ERR_INVALID_STATE` | `ESP_ERR_INVALID_STATE` | `NONE/NONE` | `NETWORK_FAULT` | 0 | 1 |
| 6 | Stop Broker | `ESP_ERR_INVALID_STATE` | `ESP_ERR_INVALID_STATE` | `NONE/NONE` | `NETWORK_FAULT` | 0 | 1 |
| 7 | Verify Broker Stopped | `ESP_ERR_TIMEOUT` | `ESP_ERR_TIMEOUT` | `NONE/NONE` | `NETWORK_FAULT` | 0 | 1 |
| 8 | Disconnect STA | `ESP_ERR_INVALID_STATE` | `ESP_ERR_INVALID_STATE` | `NONE/NONE` | `NETWORK_FAULT` | 0 | 1 |
| 9 | Verify STA Disconnected | `ESP_ERR_TIMEOUT` | `ESP_ERR_TIMEOUT` | `NONE/NONE` | `NETWORK_FAULT` | 0 | 1 |
| 10 | Verify STA IP Released | `ESP_ERR_TIMEOUT` | `ESP_ERR_TIMEOUT` | `NONE/NONE` | `NETWORK_FAULT` | 0 | 1 |
| 11 | Set AP-Only Mode | `ESP_ERR_INVALID_STATE` | `ESP_ERR_INVALID_STATE` | `NONE/NONE` | `NETWORK_FAULT` | 0 | 1 |
| 12 | Verify AP Active | `ESP_ERR_TIMEOUT` | `ESP_ERR_TIMEOUT` | `NONE/NONE` | `NETWORK_FAULT` | 0 | 1 |
| 13 | Grant Local Service | `ESP_ERR_INVALID_ARG` | `ESP_ERR_INVALID_ARG` | `NONE/NONE` | `NETWORK_FAULT` | 1 (Failed) | 1 |

---

## 7. Production NVS Observation Result Architecture (Finding 8)

Missing NVS configuration records are valid for uncommissioned hardware. The observation structure and getter explicitly distinguish valid absence from operational read failure.

### NVS Observation Result Table

| Driver Query Target | Status (`esp_err_t`) | `present` (`bool`) | `valid` (`bool`) | Meaning / Handling in Snapshot Capture |
| :--- | :--- | :---: | :---: | :--- |
| Active Selector Byte | `ESP_OK` | `true` | N/A | Active selector byte read successfully (`0` or `1`). |
| Active Selector Byte | `ESP_ERR_NOT_FOUND` | `false` | N/A | Selector key absent (Uncommissioned hardware). Valid observation. |
| Active Selector Byte | Any other error | `false` | N/A | Flash hardware / NVS driver failure. Fails snapshot capture. |
| Slot A / Slot B | `ESP_OK` | `true` | `true` / `false` | Slot payload read successfully. Valid marker and CRC checked. |
| Slot A / Slot B | `ESP_ERR_NOT_FOUND` | `false` | `false` | Slot blob key absent. Valid observation. |
| Slot A / Slot B | Any other error | `false` | `false` | Flash read error / corrupted partition header. Fails snapshot capture. |

### Complete NVS Observation Struct

```c
typedef struct {
    esp_err_t selector_status;
    bool selector_present;
    uint8_t selector;

    esp_err_t slot_a_status;
    bool slot_a_present;
    bool slot_a_valid;
    argus_cfg_slot_t slot_a;

    esp_err_t slot_b_status;
    bool slot_b_present;
    bool slot_b_valid;
    argus_cfg_slot_t slot_b;
} argus_nvs_observation_t;

esp_err_t argus_nvs_config_get_observation_snapshot(argus_nvs_observation_t *out_obs);
```

### Snapshot Capture Verification Rules
1. `capture_prod_snapshot(argus_prod_snapshot_t *out)` succeeds ONLY if every observation read returns either `ESP_OK` or `ESP_ERR_NOT_FOUND`.
2. Any unexpected error code causes `capture_prod_snapshot()` to return that error code immediately.
3. `argus_tests_4a_run_all()` checks return codes for `before` and `after` captures. If either fails, the test suite fails.
4. Before/after comparison compares presence, status, validity, selector, and metadata fields without converting absent records into zero-filled data.

---

## 8. Files Expected to Change

1. `main/argus_cmd_router.h`: Declare clean dispatch lock/unlock APIs.
2. `main/argus_cmd_router.c`: Implement `s_dispatch_mutex` lock/unlock functions without calling authority manager.
3. `main/argus_authority_mgr.h`: Remove public `argus_authority_request_service()`; expose private authority core transition seam `argus_service_authority_ops_t`.
4. `main/argus_authority_mgr.c`: Simplify `argus_authority_prepare_service_transition()` to update internal core under `s_auth_mutex` only without touching dispatch mutex.
5. `main/argus_net_mgr.h`: Declare `argus_net_snapshot_t` and `argus_net_mgr_get_snapshot()`.
6. `main/argus_net_mgr.c`: Integrate `s_net_event_group` using `xEventGroupSetBits()`/`ClearBits()`; implement private `_locked` getters/setters; acquire dispatch lock in `argus_net_mgr_request_service()`; implement bounded polling verification callbacks.
7. `main/argus_mqtt_broker.h`: Declare lifecycle queries and bounded stop interface.
8. `main/argus_mqtt_broker.c`: Implement task handle tracking, `s_broker_event_group`, `BROKER_STATE_STOPPING` cleanup, and task exit notification bit setting.
9. `main/argus_nvs_config.h`: Update `argus_nvs_config_get_observation_snapshot(argus_nvs_observation_t *out_obs)` signature to return `esp_err_t`.
10. `main/argus_nvs_config.c`: Implement error-checked read-only observation snapshot getter.
11. `main/argus_tests_4a.c`: Refactor test suite to be 100% stack-local; remove live dispatch/E-stop calls; implement complete 11-case failure matrix & explicit mock assertions; format 64-bit steps with `PRIu64`.
12. `main/app_main.c`: Migrate all menu and test callers to `argus_net_mgr_request_service()`.
13. `docs/PHASE_4A_RUNTIME_ACCEPTANCE.md`: Update documentation record.

---

## 9. Verification & Build Commands

After implementation, execute the single uncontested build command:

```powershell
. C:\Espressif\tools\Microsoft.v5.5.3.PowerShell_profile.ps1
idf.py fullclean
idf.py build
idf.py size
git diff --check
git status --short
```

---

## 10. Outstanding Verification & Operator Test Boundaries

- Physical On-Device Execution: **PENDING SHAWN'S RUNTIME TEST**
- Hardware Flashing: **PENDING OPERATOR**
- Main Branch Merge / Push / Tagging: **STOPPED / PENDING SHAWN'S REVIEW**

```text
Dispatch-transition serialization: PLANNED
Asynchronous network verification: PLANNED
Production snapshot coverage: PLANNED
Final binary evidence: PENDING IMPLEMENTATION
Scenario D: NOT STARTED
```
