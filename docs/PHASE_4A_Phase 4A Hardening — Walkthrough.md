# Phase 4A Hardening Implementation — Walkthrough

## Summary

Implemented the approved corrective plan across 10 source files, addressing the five blocking audit findings. Build verified with zero errors and zero warnings. Physical flash and on-device acceptance remain pending.

---

## Files Changed

| File | Finding | Change Summary |
|------|---------|---------------|
| argus_authority_mgr.h | F1 | Removed `prepare_service_transition`, `request_service`, `request_exit` declarations |
| argus_authority_mgr.c | F1 | Deleted 3 bypass functions (~90 LOC). Made all `prod_*` ops state-only |
| argus_net_mgr.h | F2,F5 | Added `verify_machine_safe` to ops table. Added snapshot/exit APIs |
| argus_net_mgr.c | F1-F5 | Replaced volatile→atomic. Added event group. Added verify_machine_safe. Implemented snapshot + exit |
| argus_mqtt_broker.h | F3 | Added `argus_mqtt_broker_init()` |
| argus_mqtt_broker.c | F3 | Full lifecycle hardening: state machine, lifecycle_mutex, event group, atomic client count |
| argus_nvs_config.h | F4 | Added `argus_nvs_observation_t` with per-slot error differentiation |
| argus_nvs_config.c | F4 | Per-slot error handling: OK/NOT_FOUND/unexpected |
| app_main.c | Call sites | Removed concurrent E-stop test. Fixed exit/entry error handling. Fixed labels |
| argus_tests_4a.c | Tests | Rich mock context. 5 test categories covering happy path and failure modes |
| argus_cmd_router.c | Evidence | Fixed misleading "microsecond preemption" comment |
| argus_state_mgr.c | Evidence | Fixed misleading "immediate preemption" comment |

---

## Architecture After Changes

### Service Entry Call Graph (No Deadlock Path)

```text
argus_net_mgr_request_service()
  ├── xSemaphoreTake(s_net_mutex)        [L1]
  ├── argus_cmd_router_lock_dispatch()   [L2]
  └── orchestrate_service_entry()
       ├── prepare_transition()          → set_mode only (s_auth_mutex) [L3]
       ├── request_normal_stop()         → stop_normal (s_state_mutex) [L4]
       ├── verify_stopped()              → poll snapshots (no locks)
       ├── stop_broker()                 → broker_stop (lifecycle_mutex) [L5]
       ├── verify_broker_stopped()       → poll is_running
       ├── disconnect_sta()              → esp_wifi_disconnect
       ├── verify_sta_disconnected()     → poll atomic flags
       ├── verify_sta_ip_released()      → poll atomic flags
       ├── set_wifi_ap_only()            → esp_wifi_set_mode(AP)
       ├── verify_ap_active()            → poll driver + atomic
       ├── verify_machine_safe()         → read state snapshot (NEW)
       └── grant_local()                 → set_mode only (s_auth_mutex) [L3]
  ├── argus_cmd_router_unlock_dispatch() [L2]
  └── xSemaphoreGive(s_net_mutex)        [L1]
```

### Lock Acquisition Order (Top to Bottom)

| Priority | Lock | Owner |
|----------|------|-------|
| 1 | `s_net_mutex` | argus_net_mgr.c |
| 2 | `s_dispatch_mutex` | argus_cmd_router.c |
| 3 | `s_command_mutex` / `s_auth_mutex` | cmd_router / authority_mgr |
| 4 | `s_state_mutex` | argus_state_mgr.c |
| 5 | `lifecycle_mutex` | argus_mqtt_broker.c |
| 6 | `client_lock` | argus_mqtt_broker.c |
| 7 | `s_traj_mutex` | argus_trajectory.c |
| 8 | `s_timing_mux` (spinlock) | argus_step_gen.c |

---

## What Was Verified

| Check | Result |
|-------|--------|
| `idf.py build` | 0 errors, 0 warnings |
| Bypass APIs removed | Zero references in source |
| Misleading E-stop comments | Zero remaining |
| Production calls in tests | Only read-only snapshot capture (allowed) |
| Dead code removed | estop_timer_cb, duplicate menu entry |

## What Remains Unverified

| Item | Status |
|------|--------|
| Physical flash | Pending operator |
| On-device acceptance | Pending operator |
| Runtime behavior | Pending operator |
| Pure unit test execution | Pending on-device runtime |
