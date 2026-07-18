# Phase 4A Hardening Implementation — Task List

## Authority Manager (Finding 1: Recursive Deadlock)
- [x] Remove `argus_authority_prepare_service_transition()` — deleted function + declaration
- [x] Remove `argus_authority_request_service()` — deleted function + declaration
- [x] Remove `argus_authority_request_exit()` — deleted function + declaration
- [x] Make `prod_prepare_transition` state-only (set_mode only, no dispatch/motion/polling)
- [x] Make `prod_grant_local` state-only (remove machine-state validation)
- [x] Make `prod_abort_transition` state-only (set_mode NONE/NONE)
- [x] Remove stale includes (`argus_cmd_router.h`, `argus_state_mgr.h`, `argus_nvs_config.h`, `esp_system.h`)

## Network Manager (Orchestrator Hardening)
- [x] Replace `volatile bool` flags with C11 `_Atomic bool`
- [x] Add `EventGroupHandle_t s_net_event_group` + event bits
- [x] Update all WiFi/IP event handlers to use `atomic_store` + `xEventGroupSetBits/ClearBits`
- [x] Update all polling callbacks to use `atomic_load`
- [x] Add `verify_machine_safe` to `argus_service_transition_ops_t`
- [x] Add `verify_machine_safe` validation + call in orchestrator (between verify_ap_active and grant)
- [x] Add `prod_verify_machine_safe` callback (machine state + E-stop check)
- [x] Implement `argus_net_mgr_get_snapshot()` (mutex-protected mode + atomic flags)
- [x] Implement `argus_net_mgr_request_service_exit()` (coordinated exit with reboot)
- [x] Fix SERVICE_EXIT event handler to use new exit API
- [x] Add `esp_system.h` include

## MQTT Broker (Lifecycle Hardening)
- [x] Add `argus_mqtt_broker_init()` — firmware-lifetime mutex + event group creation
- [x] Add `argus_broker_state_t` enum (STOPPED/STARTING/RUNNING/STOPPING)
- [x] Replace `bool started` with state machine
- [x] Rename `lock` → `client_lock`, add `lifecycle_mutex`, `lifecycle_event_group`
- [x] Add `TaskHandle_t server_task_handle`, `_Atomic int32_t active_client_count`
- [x] Refactor `start()` — lifecycle_mutex, event-bit wait, bounded 2000ms
- [x] Refactor `stop()` — lifecycle_mutex, shutdown listener, bounded waits
- [x] Refactor server task — socket creation inside task, event-bit signaling
- [x] Client tracking — atomic count, CLIENTS_EXITED event bit
- [x] Lock order enforced: lifecycle_mutex → client_lock

## NVS Config (Observation API)
- [x] Add `argus_nvs_observation_t` struct with per-slot error differentiation
- [x] Replace 3-parameter observation API with single-struct API
- [x] Implement per-slot ESP_OK/ESP_ERR_NOT_FOUND/unexpected error handling

## App Main (Call-Site Migration)
- [x] Remove concurrent E-stop test case `[E]` + menu entry (deferred to Phase 4B)
- [x] Remove `estop_timer_cb` (dead code)
- [x] Fix `[X]` exit to use `argus_net_mgr_request_service_exit()`
- [x] Fix `[H]` handler to check service entry return code
- [x] Fix E-stop label to truthful wording ("non-safety-rated")
- [x] Fix H label ("Request" instead of "Claim")
- [x] Add `argus_mqtt_broker_init()` to app_main startup sequence (step 10)

## Tests (Pure Unit Tests)
- [x] Rich unified mock context with call sequence tracking
- [x] Stage numbering: 2,4-14 (no phantom stage 3)
- [x] `mock_verify_machine_safe` — checks mock_machine_state + estop_latched
- [x] Happy path test — all 12 callbacks verified
- [x] Failure injection matrix — stages {4-13} individually tested
- [x] Prepare transition failure (stage 2) — no subsequent ops called
- [x] E-stop during verify_stopped — aborts before broker/network ops
- [x] verify_machine_safe failure (stage 13) — aborts before grant
- [x] NVS observation caller updated to new struct API

## Evidence & Comments
- [x] Fix misleading "microsecond preemption" comment in cmd_router.c
- [x] Fix misleading "immediate preemption" comment in state_mgr.c

## Build Verification
- [x] `idf.py build` — 0 errors, 0 warnings, successful link + binary generation

## Remaining
- [ ] Physical flash and on-device acceptance (operator)
