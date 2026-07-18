# Phase 4A Hardening Audit Corrective Plan

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

## 2. Final Lock Ownership & Synchronization Architecture

### Lock Ownership Table

| Lock Name | Declared Location | Owned Scope | Mutex Type | Nested Locks Allowed | Holding During Async/Net Waits |
| :--- | :--- | :--- | :--- | :--- | :--- |
| `s_net_mutex` | `main/argus_net_mgr.c` | Network mode transitions & event queue processing | Non-recursive FreeRTOS Mutex | May call `s_dispatch_mutex` & `s_auth_mutex` | Yes (Protected by lock-free event bits) |
| `s_dispatch_mutex` | `main/argus_cmd_router.c` | Serializes command envelope execution & authority mode transitions | Non-recursive FreeRTOS Mutex | May call `s_auth_mutex` & `s_state_mutex` | Yes (Bounded transition sequence) |
| `s_auth_mutex` | `main/argus_authority_mgr.c` | Atomic read/write of private `s_authority` core state | Non-recursive FreeRTOS Mutex | NONE (Leaf lock) | NO (Microsecond atomic updates only) |
| `s_state_mutex` | `main/argus_state_mgr.c` | Atomic state machine transitions & trajectory updates | Non-recursive FreeRTOS Mutex | NONE (Leaf lock) | NO (Microsecond atomic updates only) |

### Event-Driven Network Lifecycle Synchronization

Raw `volatile bool` flags are replaced with a FreeRTOS Event Group (`s_net_event_group`):
- `NET_BIT_STA_STARTED` (`1 << 0`)
- `NET_BIT_STA_CONNECTED` (`1 << 1`)
- `NET_BIT_STA_IP_ACQUIRED` (`1 << 2`)
- `NET_BIT_AP_STARTED` (`1 << 3`)

Wi-Fi & IP event handlers set/clear bits using `xEventGroupSetBitsFromISR()` and `xEventGroupClearBitsFromISR()`. Verification callbacks perform lock-free bounded waits using `xEventGroupWaitBits()`. Because event handlers never acquire `s_net_mutex`, deadlocks and lock inversions are physically impossible.

---

## 3. MQTT Broker Task Lifecycle Redesign (Finding 4)

To ensure `argus_mqtt_broker_stop()` waits for complete server task termination rather than merely updating a flag:

### Broker Lifecycle State Machine
```c
typedef enum {
    ARGUS_BROKER_STATE_STOPPED = 0,
    ARGUS_BROKER_STATE_STARTING,
    ARGUS_BROKER_STATE_RUNNING,
    ARGUS_BROKER_STATE_STOPPING
} argus_broker_state_t;
```

### Lifecycle Implementation Sequence
1. **Task Handle Tracking**: Maintain `TaskHandle_t s_broker_task_handle = NULL;` and `TaskHandle_t s_stopping_waiter = NULL;`.
2. **Stop Trigger (`argus_mqtt_broker_stop()`)**:
   - Acquires broker mutex, sets `s_broker_state = ARGUS_BROKER_STATE_STOPPING`.
   - Closes `s_server_fd` (unblocks `accept()` in `argus_mqtt_server_task`).
   - Closes all active client socket connections.
   - Registers calling task handle in `s_stopping_waiter`.
   - Releases broker mutex.
   - Waits on `ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000))` for server task cleanup notification.
3. **Server Task Cleanup (`argus_mqtt_server_task()`)**:
   - Upon loop exit (socket error or `STOPPING` state), closes any remaining descriptors.
   - Acquires broker mutex, sets `s_broker_state = ARGUS_BROKER_STATE_STOPPED`, clears `s_broker_task_handle = NULL`.
   - Notifies `s_stopping_waiter` if non-NULL.
   - Calls `vTaskDelete(NULL)`.
4. **Operational Query (`argus_mqtt_broker_is_running()`)**:
   - Returns `true` ONLY if `s_broker_state == ARGUS_BROKER_STATE_RUNNING` AND `s_broker_task_handle != NULL`.

---

## 4. Production Call-Site Migration List (Finding 2)

All production code paths that request local authority will be migrated to invoke `argus_net_mgr_request_service(requested_owner)`. Direct calls to `argus_authority_request_service()` outside `argus_net_mgr.c` are deleted.

| Call-Site File | Function / Section | Old Direct Call | New Orchestrated Call | Failure Handling |
| :--- | :--- | :--- | :--- | :--- |
| `main/app_main.c` | Menu Option `N -> 5` | `argus_authority_request_service(CLI)` | `argus_net_mgr_request_service(ARGUS_AUTH_OWNER_DIAGNOSTIC_CLI)` | Displays error code if `!= ESP_OK`, leaves UI in ungranted state |
| `main/app_main.c` | Hardware Acceptance `H` | `argus_authority_request_service(CLI)` | `argus_net_mgr_request_service(ARGUS_AUTH_OWNER_DIAGNOSTIC_CLI)` | Aborts hardware acceptance sequence on failure |
| `main/app_main.c` | E-Stop Transition Test | `argus_authority_request_service(CLI)` | `argus_net_mgr_request_service(ARGUS_AUTH_OWNER_DIAGNOSTIC_CLI)` | Validates fail-closed state |
| `main/argus_authority_mgr.c` | Header Declaration | Exposed `argus_authority_request_service` | Removed from public header; local service entry owned by `argus_net_mgr.c` | Compile error if invoked externally |

---

## 5. Pure-Test Isolation & Seam Architecture (Finding 5)

Lowercase `t` (`main/argus_tests_4a.c`) must be 100% stack-local with zero live production calls:

1. **Eliminate Production Dispatch Calls**: Remove `argus_cmd_router_dispatch(&estop_env)` and live `s_dispatch_mutex` locking from `main/argus_tests_4a.c`.
2. **Stack-Local Authority Core**: All authority tests instantiate stack-local `argus_authority_core_t local_acore` and call `argus_authority_core_*` functions.
3. **Stack-Local NVS Drivers**: All NVS tests instantiate stack-local `mock_nvs_store_t local_store` and call `argus_nvs_core_*` functions via context pointers.
4. **Mock Orchestration Seam**: Test 16 uses mock `argus_service_authority_ops_t` and `argus_service_transition_ops_t` targeting stack-local test context `mock_orchestration_test_ctx_t`. Zero FreeRTOS tasks, zero Wi-Fi driver calls, zero MQTT broker calls, and zero state manager calls occur during `t`.

---

## 6. Failure-Injection Matrix (Finding 6 & 7)

Test 16 in `main/argus_tests_4a.c` will use a deterministic array of failure injection cases. Each iteration initializes a fresh stack-local `mock_orchestration_test_ctx_t` struct to guarantee zero cross-test contamination.

```c
typedef struct {
    const char *case_name;
    int inject_stage; // 2 through 13
    esp_err_t injected_err;
    esp_err_t expected_err;
} test_injection_case_t;
```

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

### Simulated E-Stop Preemption Case
During `verify_stopped` (Stage 5), mock returns `ESP_ERR_INVALID_STATE` with `estop_latched = true`.
- Assert `estop_latched == true`.
- Assert `grant_local_count == 0`.
- Assert `motion_start_count == 0`.
- Assert `authority.mode == NONE` & `owner == NONE`.
- Assert `net_mode == NETWORK_FAULT`.

---

## 7. Production Observation Design (Finding 8)

### Read-Only NVS Observation API
`argus_nvs_config_get_observation_snapshot()` will return `esp_err_t`:
- Calls `drv->read_selector(drv->ctx, &out_obs->selector)` and captures `esp_err_t selector_err`.
- Calls `drv->read_slot(drv->ctx, 0, &out_obs->slot_a)` and captures `esp_err_t slot_a_err`.
- Calls `drv->read_slot(drv->ctx, 1, &out_obs->slot_b)` and captures `esp_err_t slot_b_err`.
- Validates slot CRCs and markers to set `slot_a_valid` and `slot_b_valid`.
- **Zero Mutation Guarantee**: Does NOT call `nvs_flash_init()`, repair slots, write selector, update `s_active_config`, write keys, or commit.

### Full Snapshot Capture & Safe Formatting
1. `capture_prod_snapshot(argus_prod_snapshot_t *out)` returns `esp_err_t`. If state, authority, network, or NVS queries fail, `capture_prod_snapshot()` returns the failing `esp_err_t`.
2. `argus_tests_4a_run_all()` checks return codes for `before` and `after` captures. If either fails, the suite fails immediately.
3. 64-bit step counts (`generated_step_count`) are formatted using `PRIu64` from `<inttypes.h>` instead of casting to `long`.
4. Slot metadata structures are compared fieldwise (`schema_version`, `config_generation`, `payload_length`, `crc32`, `valid_marker`) to prevent uninitialized padding comparison errors.

---

## 8. Files Expected to Change

1. `main/argus_cmd_router.h`: Declare clean dispatch lock/unlock APIs.
2. `main/argus_cmd_router.c`: Implement `s_dispatch_mutex` lock/unlock functions without calling authority manager.
3. `main/argus_authority_mgr.h`: Remove public `argus_authority_request_service()`; expose private authority core transition seam `argus_service_authority_ops_t`.
4. `main/argus_authority_mgr.c`: Simplify `argus_authority_prepare_service_transition()` to update internal core under `s_auth_mutex` only without touching dispatch mutex.
5. `main/argus_net_mgr.h`: Update Wi-Fi mode representation; expand `argus_service_transition_ops_t`.
6. `main/argus_net_mgr.c`: Integrate `s_net_event_group`; acquire dispatch lock in `argus_net_mgr_request_service()`; implement bounded polling verification callbacks.
7. `main/argus_mqtt_broker.h`: Declare lifecycle queries and bounded stop interface.
8. `main/argus_mqtt_broker.c`: Implement task handle tracking, `BROKER_STATE_STOPPING` cleanup, and task exit notification.
9. `main/argus_nvs_config.h`: Update `argus_nvs_config_get_observation_snapshot()` signature to return `esp_err_t`.
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
