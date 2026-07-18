# Phase 4A Hardening — Amended Plan Sections

Amendments to the source-reconciled corrective plan. Only changed sections are shown.

No source was edited to produce this document.

---

## 1. Authority Callbacks — State-Only (Corrected)

The prior plan incorrectly duplicated motion control inside `prepare_transition`. The corrected architecture separates authority mutation from motion control.

### Corrected `prepare_transition` — Authority-State-Only

```c
static esp_err_t prod_prepare_transition(void *ctx) {
    (void)ctx;
    // Acquire s_auth_mutex, set SERVICE_TRANSITION/NONE, increment generation, release.
    return argus_authority_mgr_set_mode(ARGUS_AUTHORITY_SERVICE_TRANSITION, ARGUS_AUTH_OWNER_NONE);
}
```

That is the complete callback. It does NOT:
- Acquire `s_dispatch_mutex`
- Call `argus_state_mgr_stop_normal()`
- Poll machine state
- Call E-stop
- Operate MQTT or Wi-Fi
- Delay

### Corrected `grant_local` — Authority-State-Only

```c
static esp_err_t prod_grant_local(void *ctx, argus_authority_owner_t owner) {
    (void)ctx;
    return argus_authority_grant_local_service(owner);
}
```

`argus_authority_grant_local_service` ([argus_authority_mgr.c:206–234](file:///c:/Users/bount/Dev/Argus/ArgusControl_PumpController-V2/main/argus_authority_mgr.c#L206-L234)) currently reads the authority snapshot and verifies machine state before granting. The state verification (`argus_state_mgr_get_snapshot`) is read-only; the grant call itself only acquires `s_auth_mutex`.

> [!IMPORTANT]
> `argus_authority_grant_local_service` also validates machine state via `argus_state_mgr_get_snapshot` at L223. This is read-only observation but should the authority layer be checking machine state at all? The orchestrator's `verify_stopped` callback has already confirmed the machine is stopped before `grant_local` is called. Decision: retain the guard as defense-in-depth — it does not mutate state or acquire motion locks.

### Corrected `abort_transition` — Authority-State-Only

```c
static void prod_abort_transition(void *ctx) {
    (void)ctx;
    argus_authority_mgr_set_mode(ARGUS_AUTHORITY_NONE, ARGUS_AUTH_OWNER_NONE);
}
```

Short `s_auth_mutex` take-set-give. No dispatch, no motion, no polling.

### Corrected Production Call Graph

```text
argus_net_mgr_request_service(requested_owner)     [argus_net_mgr.c]
  │
  ├─► xSemaphoreTake(s_net_mutex)                   LOCK #1
  │   Validates: s_net_mode ∈ {AP_DISCOVERABLE, UNCOMMISSIONED_AP}
  │   Validates: s_ap_started == true
  │
  ├─► argus_cmd_router_lock_dispatch()               LOCK #2 (s_dispatch_mutex)
  │
  ├─► argus_net_mgr_orchestrate_service_entry(...)
  │   ├─ auth_ops->prepare_transition()
  │   │   └─ argus_authority_mgr_set_mode(SERVICE_TRANSITION, NONE)
  │   │      └─ [s_auth_mutex: take → set mode/owner, gen++ → give]
  │   │
  │   ├─ ops->request_normal_stop()                  SOLE controlled stop
  │   │   └─ argus_state_mgr_stop_normal()
  │   │      └─ [s_command_mutex → s_state_mutex → stop_normal op → release]
  │   │
  │   ├─ ops->verify_stopped()                       bounded 5.0s poll
  │   │   └─ argus_state_mgr_get_snapshot() (read-only)
  │   │      Accepts: HOLDING, UNLOCKED
  │   │      Rejects: EMERGENCY_STOPPED, FAULTED, estop_latched
  │   │
  │   ├─ ops->stop_broker()
  │   ├─ ops->verify_broker_stopped()                bounded 2.0s
  │   ├─ ops->disconnect_sta()
  │   ├─ ops->verify_sta_disconnected()              bounded 2.0s
  │   ├─ ops->verify_sta_ip_released()               bounded 2.0s
  │   ├─ ops->set_wifi_ap_only()
  │   ├─ ops->verify_ap_active()                     bounded 2.0s
  │   ├─ *net_mode = SERVICE_AP_ONLY
  │   └─ auth_ops->grant_local(owner)
  │       └─ argus_authority_grant_local_service(owner)
  │          └─ [s_auth_mutex: take → set mode/owner → give]
  │
  ├─► argus_cmd_router_unlock_dispatch()             release LOCK #2 (all paths)
  └─► xSemaphoreGive(s_net_mutex)                    release LOCK #1 (all paths)
```

### Deletion Scope for `argus_authority_prepare_service_transition()`

Delete entirely:
- Declaration at [argus_authority_mgr.h:77](file:///c:/Users/bount/Dev/Argus/ArgusControl_PumpController-V2/main/argus_authority_mgr.h#L77)
- Implementation at [argus_authority_mgr.c:162–204](file:///c:/Users/bount/Dev/Argus/ArgusControl_PumpController-V2/main/argus_authority_mgr.c#L162-L204) (43 lines including dispatch lock/unlock, stop_normal, 5s poll, E-stop fallback)

The `prod_prepare_transition` callback ([argus_authority_mgr.c:99–102](file:///c:/Users/bount/Dev/Argus/ArgusControl_PumpController-V2/main/argus_authority_mgr.c#L99-L102)) becomes a single `set_mode` call.

---

## 2. Corrected Production Call-Site Audit

### Search Command and Output

Working-tree search (includes uncommitted modifications):
```
app_main.c:453:  esp_err_t err = argus_net_mgr_request_service(ARGUS_AUTH_OWNER_DIAGNOSTIC_CLI);
app_main.c:554:  esp_err_t res = argus_net_mgr_request_service(ARGUS_AUTH_OWNER_DIAGNOSTIC_CLI);
app_main.c:886:  argus_net_mgr_request_service(ARGUS_AUTH_OWNER_DIAGNOSTIC_CLI);
argus_authority_mgr.c:242: esp_err_t argus_authority_request_service(...)  [definition]
argus_authority_mgr.h:101: esp_err_t argus_authority_request_service(...)  [declaration]
```

**Committed version** at SHA `bdb3b34` (the actual audit branch state):
```
app_main.c:  argus_authority_request_service(ARGUS_AUTH_OWNER_DIAGNOSTIC_CLI);  [3 call sites]
```

### Discrepancy Explanation

The working tree has 4 tracked files modified since commit (`M main/app_main.c`, `M main/CMakeLists.txt`, `M main/argus_console_helpers.c`, `M main/argus_mqtt_broker.c`). These uncommitted edits migrated the 3 `app_main.c` call sites from `argus_authority_request_service` to `argus_net_mgr_request_service`. However, **the audit branch commit `bdb3b34` still contains the direct bypass calls**.

### Required Migration (Implementation Phase)

All 3 committed call sites must be migrated to `argus_net_mgr_request_service()` with error checking:

| Call Site | Menu Path | Required Change |
|:---:|:---|:---|
| `app_main.c` (committed near L453 equivalent) | N → 5 "Request LOCAL_SERVICE" | Check and display returned error |
| `app_main.c` (committed near L554 equivalent) | Concurrent E-stop transition test | Check error; if not ESP_OK, report failure |
| `app_main.c` (committed near L886 equivalent) | H "Claim CLI authority" | Open hardware controls only after `ESP_OK` |

### Working Tree Status

```
SHA:     bdb3b34a4eb44f961a5407dea2f4c674e4cad20d
Branch:  phase4a-hardening-audit
Dirty:   M main/CMakeLists.txt
         M main/app_main.c
         M main/argus_console_helpers.c
         M main/argus_mqtt_broker.c
         6 untracked doc files under docs/
```

---

## 3. E-Stop Preemption — Honest Analysis and Phase 4A Scope Decision

### Current Architecture (Source-Verified)

E-stop enters through `argus_cmd_router_dispatch` ([argus_cmd_router.c:44–47](file:///c:/Users/bount/Dev/Argus/ArgusControl_PumpController-V2/main/argus_cmd_router.c#L44-L47)):

```c
if (env->source == ARGUS_CMD_SRC_INTERNAL_SAFETY || env->command_type == ARGUS_CMD_TYPE_ESTOP) {
    return argus_state_mgr_estop();  // bypasses s_dispatch_mutex
}
```

`argus_state_mgr_estop` ([argus_state_mgr.c:480–487](file:///c:/Users/bount/Dev/Argus/ArgusControl_PumpController-V2/main/argus_state_mgr.c#L480-L487)):

```c
esp_err_t argus_state_mgr_estop(void) {
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);   // BLOCKS here
    esp_err_t err = argus_state_core_estop(&s_prod_core);
    xSemaphoreGive(s_state_mutex);
    return err;
}
```

### Blocking Analysis

`s_state_mutex` is held by normal commands during their entire motion-ops call chain:

```text
argus_state_mgr_start():
  s_command_mutex (100ms timeout) →
    s_state_mutex (portMAX_DELAY) →
      core->ops->enable_driver()     ← argus_trajectory_enable_driver()
        → s_traj_mutex → s_lifecycle_mutex → s_timing_mux
      core->ops->set_target_rate()   ← argus_trajectory_set_target_rpm_milli()
        → s_traj_mutex → s_lifecycle_mutex → s_timing_mux
    ← s_state_mutex released
  ← s_command_mutex released
```

**E-stop blocks behind `s_state_mutex` until the in-flight motion-ops call chain completes.** Worst-case latency equals the duration of the longest motion-ops sequence (`enable_driver` + `set_target_rate` in `start`, or `stop_normal` in `stop_normal`, or `stop_immediate` + `disable_driver` + `enable_driver` + `stop_immediate` in `recover`).

### Generation Counter Is Dead Code

`argus_state_core_start` ([argus_state_mgr.c:159–184](file:///c:/Users/bount/Dev/Argus/ArgusControl_PumpController-V2/main/argus_state_mgr.c#L159-L184)) checks `command_generation` after each motion op:

```c
uint32_t entry_gen = core->command_generation;
// ... enable_driver() ...
if (core->command_generation != entry_gen || core->estop_latched) { return ...; }
// ... set_target_rate() ...
if (core->command_generation != entry_gen || core->estop_latched) { return ...; }
```

But `command_generation` is incremented only by `argus_state_core_estop` at [argus_state_mgr.c:267](file:///c:/Users/bount/Dev/Argus/ArgusControl_PumpController-V2/main/argus_state_mgr.c#L267). E-stop can only write this field while holding `s_state_mutex`. The `start` caller already holds `s_state_mutex`. Therefore the generation counter **never changes between the checks** — the revalidation logic at L172 and L182 is unreachable.

### Actual Pulse-Stop Path

When E-stop finally acquires `s_state_mutex`, `argus_state_core_estop` at [argus_state_mgr.c:271–273](file:///c:/Users/bount/Dev/Argus/ArgusControl_PumpController-V2/main/argus_state_mgr.c#L271-L273) calls `core->ops->stop_immediate()` which cascades:

```text
argus_trajectory_stop_immediate()     [argus_trajectory.c:205]
  → s_traj_mutex (portMAX_DELAY)
    → argus_step_gen_stop_immediate() [argus_step_gen.c:553]
      → s_lifecycle_mutex (portMAX_DELAY)
        → taskENTER_CRITICAL(&s_timing_mux)
          → s_running = false
          → gptimer_stop()
          → gpio_set_level(STEP, inactive)  ← ACTUAL pulse halt
        → taskEXIT_CRITICAL(&s_timing_mux)
      → s_lifecycle_mutex release
    → s_traj_mutex release
```

The `gptimer_stop()` and `gpio_set_level()` calls in `argus_step_gen_stop_immediate` are the actual hardware operations that halt pulse generation. These execute under `s_timing_mux` (spinlock), which ensures ISR-safe atomicity for the timer alarm callback at [argus_step_gen.c:72–106](file:///c:/Users/bount/Dev/Argus/ArgusControl_PumpController-V2/main/argus_step_gen.c#L72-L106).

### Phase 4A Decision: Defer Concurrent E-Stop Preemption

A true software preemption design requires:
1. Splitting `s_state_mutex` so that the E-stop latch and pulse-halt can proceed without waiting for normal command completion.
2. Making `estop_latched` and `command_generation` atomics or spinlock-protected for cross-task visibility.
3. Ensuring interrupted normal commands detect the latch after releasing motion locks and do not overwrite `EMERGENCY_STOPPED`.
4. Verifying the timer alarm ISR honors the stop flag before the next pulse.
5. Measuring actual worst-case latency on hardware.

This is a substantial cross-cutting refactor of `argus_state_mgr.c`, `argus_trajectory.c`, and potentially `argus_step_gen.c`. It cannot be safely designed without physical testing infrastructure.

**Concurrent E-stop preemption is deferred to Phase 4B.** Phase 4A scope explicitly excludes:
- Concurrent transition E-stop completion testing
- Any claim of "immediate" or "microsecond" E-stop latency
- The concurrent E-stop menu test in `app_main.c`

Phase 4A E-stop testing scope:
- Stack-local mock simulation proving orchestrator abort-on-estop (mock `verify_stopped` returns `ESP_ERR_INVALID_STATE` with `estop_latched = true`)
- Existing E-stop dispatch bypass test (E-stop bypasses `s_dispatch_mutex`) remains valid as a static/build-time assertion

### Files Affected (Deferred to Phase 4B)

- `main/argus_state_mgr.c` / `.h`: Split `s_state_mutex` or introduce atomic latch
- `main/argus_trajectory.c` / `.h`: Possibly needs cancellation-aware motion ops
- `main/argus_step_gen.c` / `.h`: Verify ISR alarm callback honors stop flag before next pulse
- `main/argus_tests_4a.c`: Add physical E-stop latency measurement test
- `docs/PHASE_4A_RUNTIME_ACCEPTANCE.md`: Document deferred scope

The concurrent E-stop menu test in `app_main.c` (committed near the L554 equivalent) should either:
- **(A)** Be removed from Phase 4A and marked as Phase 4B, or
- **(B)** Be retained but documented as testing only "E-stop is eventually applied after normal command completes" — not preemption.

> [!IMPORTANT]
> Which option for the concurrent E-stop menu test? (A) Remove from Phase 4A, or (B) Retain with honest latency documentation?

---

## 4. Broker Lifecycle — Server and Client Task Tracking

### Revised Design

```c
typedef enum {
    BROKER_STATE_STOPPED = 0,
    BROKER_STATE_STARTING,
    BROKER_STATE_RUNNING,
    BROKER_STATE_STOPPING,
} argus_broker_state_t;

typedef struct {
    // Firmware-lifetime mutex — created once in argus_mqtt_broker_init(), never deleted
    SemaphoreHandle_t lifecycle_mutex;

    // Server task tracking
    TaskHandle_t server_task_handle;

    // Client task tracking
    _Atomic int32_t active_client_count;   // decremented by each client task on exit

    // Lifecycle signals
    EventGroupHandle_t lifecycle_event_group;
    // BROKER_EVT_STARTED  (1 << 0)  — server task bound and listening
    // BROKER_EVT_STOPPED  (1 << 1)  — server task exited

    // State
    argus_broker_state_t state;

    // Runtime fields (existing)
    uint16_t port;
    int listen_sock;
    SemaphoreHandle_t client_lock;  // protects client slot array during accept/publish/close
    argus_mqtt_broker_message_cb_t on_message;
    argus_mqtt_broker_policy_cb_t policy_check;
    void *user_ctx;
    argus_mqtt_client_t clients[ARGUS_MQTT_MAX_CLIENTS];
    argus_mqtt_retained_t retained[ARGUS_MQTT_MAX_RETAINED];

    // Startup error capture
    esp_err_t startup_error;
} argus_mqtt_broker_t;
```

### Initialization — Firmware-Lifetime Mutex

```c
esp_err_t argus_mqtt_broker_init(void) {
    if (s_broker.lifecycle_mutex == NULL) {
        s_broker.lifecycle_mutex = xSemaphoreCreateMutex();
    }
    if (s_broker.lifecycle_event_group == NULL) {
        s_broker.lifecycle_event_group = xEventGroupCreate();
    }
    atomic_store(&s_broker.active_client_count, 0);
    s_broker.state = BROKER_STATE_STOPPED;
    return ESP_OK;
}
```

`lifecycle_mutex` and `lifecycle_event_group` survive stop/start cycles. Never deleted or `memset`ed.

### Start Sequence

1. Under `lifecycle_mutex`: verify `state == STOPPED` AND `active_client_count == 0` AND `server_task_handle == NULL`. Reject otherwise.
2. Transition `state = STARTING`.
3. Clear stale event bits.
4. Zero runtime fields (client slots, retained, sockets) — NOT `lifecycle_mutex`, `lifecycle_event_group`, or `state`.
5. Create `client_lock = xSemaphoreCreateMutex()`.
6. Create server task, store `server_task_handle`.
7. Release `lifecycle_mutex`.
8. Wait bounded (2000ms) on `xEventGroupWaitBits` for `BROKER_EVT_STARTED | BROKER_EVT_STOPPED`.
9. On `BROKER_EVT_STARTED`: return `ESP_OK`.
10. On `BROKER_EVT_STOPPED` or timeout: read `startup_error`, return it.

Server task on successful bind/listen:
- Set `state = RUNNING`, set `BROKER_EVT_STARTED`.

Server task on bind/listen failure:
- Set `startup_error`, clean up socket, set `state = STOPPED`, set `server_task_handle = NULL`, set `BROKER_EVT_STOPPED`, self-delete.

### Client Allocation After STOPPING

Server task's accept loop checks `state` before allocating a new client slot:
```c
if (s_broker.state != BROKER_STATE_RUNNING) break;
```

Client task on exit (recv error, disconnect, protocol error):
```c
// Clean up client slot under client_lock
// Then:
atomic_fetch_sub(&s_broker.active_client_count, 1);
vTaskDelete(NULL);
```

### Stop Sequence

1. Under `lifecycle_mutex`: if `state == STOPPED`, return `ESP_OK` (idempotent). If `state == STARTING`, reject.
2. Set `state = STOPPING`.
3. Clear stale `BROKER_EVT_STOPPED`.
4. `shutdown(listen_sock, SHUT_RDWR)` then `close(listen_sock)`. Set `listen_sock = -1`.
5. Under `client_lock`: close every in-use client socket.
6. Release `lifecycle_mutex`.
7. Wait bounded (2000ms) on `BROKER_EVT_STOPPED` (server task exit).
8. Poll `active_client_count` with bounded wait (additional 2000ms) for zero.
9. If server exited AND client count == 0:
   - Under `lifecycle_mutex`: `vSemaphoreDelete(client_lock)`, `client_lock = NULL`, `server_task_handle = NULL`, `state = STOPPED`.
   - Return `ESP_OK`.
10. If timeout: remain `state = STOPPING`, return `ESP_ERR_TIMEOUT`.

Server task exit path:
```c
// After loop exits:
s_broker.server_task_handle = NULL;
xEventGroupSetBits(s_broker.lifecycle_event_group, BROKER_EVT_STOPPED);
vTaskDelete(NULL);
```

### Restart Protection

`start()` under `lifecycle_mutex` checks:
- `state == STOPPED`
- `server_task_handle == NULL`
- `active_client_count == 0`

If any condition fails, returns `ESP_ERR_INVALID_STATE`. This blocks restart while any prior task remains.

---

## 5. Authoritative Network Synchronization — C11 Atomics

### Decision

Replace `volatile bool` flags with C11 `_Atomic bool` for network lifecycle state. Do **not** retain unsynchronized `volatile bool` as secondary reads.

### Affected Variables

Replace in [argus_net_mgr.c:55–58](file:///c:/Users/bount/Dev/Argus/ArgusControl_PumpController-V2/main/argus_net_mgr.c#L55-L58):

```c
// Before:
static volatile bool s_sta_started = false;
static volatile bool s_sta_connected = false;
static volatile bool s_sta_ip_acquired = false;
static volatile bool s_ap_started = false;

// After:
static _Atomic bool s_sta_started = false;
static _Atomic bool s_sta_connected = false;
static _Atomic bool s_sta_ip_acquired = false;
static _Atomic bool s_ap_started = false;
```

Event callbacks use `atomic_store(&s_sta_connected, true/false)` with implicit `memory_order_seq_cst`.

Public getters use `atomic_load(&s_sta_connected)`.

### Event Group Role

`s_net_event_group` is still added for **blocking waits** in verification callbacks (e.g., `prod_verify_sta_disconnected` needs to block until STA disconnect event fires). Event group bits are set/cleared from event callbacks alongside the atomic stores.

Atomic flags provide the authoritative single-point-of-truth for snapshot reads. Event group bits provide blocking wait capability for orchestration.

### Coherent Snapshot

`argus_net_mgr_get_snapshot()` under `s_net_mutex` reads:
- `s_net_mode` (protected by `s_net_mutex`)
- `s_last_error` (protected by `s_net_mutex`)
- `atomic_load(&s_sta_connected)` etc. (atomic, no additional lock needed)

The snapshot does not mix unsynchronized and synchronized fields. All fields have defined memory ordering.

---

## 6. Stack-Local E-Stop Test Seam — Rich Mock Context

### Mock Context Structure

```c
typedef struct {
    // Authority state (stack-local)
    argus_authority_core_t auth_core;

    // Machine simulation
    argus_machine_state_t mock_machine_state;
    bool estop_latched;

    // Call counters
    int motion_start_count;
    int prepare_count;
    int abort_count;
    int grant_count;

    // Per-operation call counts
    int normal_stop_count;
    int verify_stopped_count;
    int stop_broker_count;
    int verify_broker_count;
    int disconnect_sta_count;
    int verify_sta_disc_count;
    int verify_sta_ip_count;
    int set_ap_count;
    int verify_ap_count;

    // Exact call sequence
    int call_sequence[20];
    int call_count;

    // Failure injection
    int fail_stage;
    bool estop_during_stop;
} mock_orchestration_ctx_t;
```

### E-Stop Simulation in `verify_stopped` Callback

```c
static esp_err_t mock_verify_stopped(void *ctx) {
    mock_orchestration_ctx_t *m = (mock_orchestration_ctx_t *)ctx;
    m->verify_stopped_count++;
    m->call_sequence[m->call_count++] = 5;

    if (m->estop_during_stop) {
        m->mock_machine_state = ARGUS_STATE_EMERGENCY_STOPPED;
        m->estop_latched = true;
        return ESP_ERR_INVALID_STATE;
    }
    if (m->fail_stage == 5) return ESP_ERR_INVALID_STATE;
    return ESP_OK;
}
```

### Complete Assertion Set for E-Stop Test

```c
// After orchestrator returns:
assert(res == ESP_ERR_INVALID_STATE);
assert(m.estop_latched == true);
assert(m.mock_machine_state == ARGUS_STATE_EMERGENCY_STOPPED);
assert(m.auth_core.mode == ARGUS_AUTHORITY_NONE);
assert(m.auth_core.owner == ARGUS_AUTH_OWNER_NONE);
assert(net_mode == ARGUS_NET_MODE_NETWORK_FAULT);
assert(m.grant_count == 0);
assert(m.motion_start_count == 0);

// Cutoff verification: only request_normal_stop and verify_stopped were called
assert(m.call_count == 2);
assert(m.call_sequence[0] == 4);  // request_normal_stop
assert(m.call_sequence[1] == 5);  // verify_stopped

// All subsequent ops were NOT called
assert(m.stop_broker_count == 0);
assert(m.disconnect_sta_count == 0);
assert(m.set_ap_count == 0);
assert(m.verify_ap_count == 0);

// Abort was called exactly once
assert(m.abort_count == 1);

// ZERO production APIs were invoked by this test
```

This tests the latch assertion, the cutoff behavior, and the authority abort — all using stack-local state.

---

## 7. Open Question Resolutions

### Q1 — Service Exit: Network Manager Owns Serialization ✓

`argus_authority_request_exit()` at [argus_authority_mgr.c:249–297](file:///c:/Users/bount/Dev/Argus/ArgusControl_PumpController-V2/main/argus_authority_mgr.c#L249-L297) currently acquires `s_dispatch_mutex` independently at L252. Per your directive, the network manager must own coordinated exit serialization, and authority callbacks must be state-only.

**Change:** Refactor exit into the same pattern as entry:
- Network manager task SERVICE_EXIT handler acquires `s_net_mutex` (already does at [argus_net_mgr.c:131](file:///c:/Users/bount/Dev/Argus/ArgusControl_PumpController-V2/main/argus_net_mgr.c#L131)), then acquires `s_dispatch_mutex`, then calls an exit orchestrator with state-only authority callbacks.
- Authority exit callback only sets `ARGUS_AUTHORITY_NONE/NONE` under `s_auth_mutex`.
- Motion stop and network restoration are separate orchestrator steps, not embedded in authority callbacks.
- Delete the standalone `argus_authority_request_exit()` public API.

### Q2 — Phantom Stage 3: Removed ✓

The failure injection loop will iterate stages 2, then 4–13. Stage 3 is removed entirely. There are exactly 11 injectable callbacks, so 11 failure stages are tested. The loop becomes:

```c
int fail_stages[] = {2, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13};
for (int i = 0; i < 11; i++) {
    int fail_stg = fail_stages[i];
    // ...
}
```

### Q3 — Legacy `argus_stepper.c`: Excluded With Evidence ✓

**Evidence:**

1. **Not in build:** [CMakeLists.txt](file:///c:/Users/bount/Dev/Argus/ArgusControl_PumpController-V2/main/CMakeLists.txt) lists 19 source files. `argus_stepper.c` is NOT among them. It is not compiled.

2. **Not referenced:** Search for `argus_stepper_` across all `.c` and `.h` files returns results ONLY within `argus_stepper.c` and `argus_stepper.h` themselves. No other V2 production file calls any `argus_stepper_*` API.

3. **No shared hardware access:** `argus_stepper.c` configures its own GPTimer, PCNT, and GPIO at init time. The V2 production motion stack uses separate instances configured in `argus_step_gen.c`.

4. **Does not create a task unless initialized:** `argus_stepper_init()` creates a `ramp_task`, but since `argus_stepper_init()` is never called (not in any compiled source), no task is created.

**Conclusion:** `argus_stepper.c` and its mutex `s_ctx.lock` are dead code. Excluded from the active V2 lock hierarchy. The hierarchy contains **8 active mutexes + 1 spinlock** (9 total primitives).

---

## 8. Status Confirmation

```text
Branch:        phase4a-hardening-audit
SHA:           bdb3b34a4eb44f961a5407dea2f4c674e4cad20d
Working tree:  4 tracked files modified (uncommitted), 6 untracked doc files
Source edited: NO — this document was produced from read-only searches only
```

No source files were edited to produce this document. All search outputs are from the repository's current working tree and committed state.
