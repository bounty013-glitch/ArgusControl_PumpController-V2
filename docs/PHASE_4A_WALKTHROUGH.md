# Phase 4A Implementation Walkthrough — Device Identity, Persistent Configuration, Network Modes, and Control Authority

> [!WARNING]
> **SUPERSEDED DOCUMENT**
>
> This walkthrough was written before the Phase 4A hardening audit. Several
> claims are stale and have been corrected by the hardening pass. The
> authoritative Phase 4A documents are:
>
> - `PHASE_4A_RUNTIME_ACCEPTANCE.md` — Physically verified evidence
> - `PHASE_4A_Phase 4A Hardening — Walkthrough.md` — Hardening corrections
>
> Stale claims corrected below are marked with **[CORRECTED]**.

## Summary of Completed Implementation & Hardening

Phase 4A: COMPLETE WITHIN REVISED SCOPE.

Phase 4A has been implemented and hardened on ESP32-S3 hardware in
`ArgusControl_PumpController-V2` adhering strictly to all user design
directives, lock hierarchy principles, single-authority model
("One pump, one command authority"), and secrets protection guardrails.

Active-motion MQTT/HMI-to-local authority handoff:
DEFERRED BY DESIGN TO PHASE 4D END-TO-END ACCEPTANCE.

---

## Technical Highlights & Enforced Guardrails

1. **Persistent Device Identity (`argus_identity`)**:
   - Derives hardware UID from efuse MAC address formatted as `ESP32S3-XXYYZZAABBCC`.
   - Incorporates `esp_app_get_description()->version` for build firmware version tracking.
   - Generates service SoftAP SSID as `Argus-Service-XXYYZZ` using the last 3 MAC bytes.
   - Validates commissioning identity strings (`client_id`, `unit_id`, `device_name`).

2. **Power-Loss-Safe Dual-Slot NVS Configuration (`argus_nvs_config`)**:
   - Operating under `"argus_cfg"` namespace with dual-slot redundancy (`cfg_slot_0`, `cfg_slot_1`) and active slot selector `active_slot`.
   - Tracks transaction marker `factory_reset_pending` in `"argus_sys"`.
   - Uses serial number wrap-safe generation comparison: `((int32_t)(gen_a - gen_b)) > 0`.
   - Protects payloads with CRC32 integrity checks and valid marker `0xA5A55A5A`.
   - Enforces Schema V1 non-empty STA SSID requirement (8–63 char password, no open STA support).
   - Erases both dual slots during factory reset while keeping `CONFIG_ARGUS_SERVICE_AP_PASS` build credential intact.
   - Includes injectable driver interface (`argus_nvs_driver_t`) for pure unit testing without NVS flash writes.
   - **[CORRECTED]** Commit uses the selector to determine the target slot, guaranteeing the selector-pointed LKG is never overwritten. On readback verification failure, the selector is not updated.

3. **Exclusive Control Authority & Owner Tracking (`argus_authority_mgr`)**:
   - Enforces authority modes (`NONE`, `SUPERVISORY`, `SERVICE_TRANSITION`, `LOCAL_SERVICE`) and owner tracking (`NONE`, `MQTT`, `BROWSER`, `DIAGNOSTIC_CLI`).
   - Uses `argus_authority_mgr_get_snapshot()` to acquire `s_auth_mutex` briefly, copy snapshot atomically, and release it immediately to prevent lock inversion.
   - Implements permission matrix validation (`argus_authority_validate_permission()`).
   - **[CORRECTED]** Public authority-bypass APIs (`prepare_service_transition`, `request_service`, `request_exit`) were removed during the hardening audit. All service transitions route through the coordinated orchestrator.

4. **Command Router & Dispatch Serialization Gate (`argus_cmd_router`)**:
   - Serializes all state manager commands using `s_dispatch_mutex`.
   - Validates envelope source (`ARGUS_CMD_SRC_MQTT_SUPERVISORY`, `ARGUS_CMD_SRC_LOCAL_SERVICE_PORTAL`, `ARGUS_CMD_SRC_CLI_DIAGNOSTIC`, `ARGUS_CMD_SRC_INTERNAL_SAFETY`), authority generation, and permission before calling `argus_state_mgr`.
   - **[CORRECTED]** E-stop does not bypass `s_dispatch_mutex`. The "microsecond preemption" claim was removed during hardening. E-stop goes through the same serialized dispatch path. Concurrent E-stop preemption (split mutex, atomic latch) is deferred.
   - Emits `ESP_LOGW` warning diagnostics if envelope generation counter mismatches or authority permission is denied.

5. **Dedicated Network Manager Task (`argus_net_mgr`)**:
   - Manages Wi-Fi lifecycle (Uncommissioned AP vs. Commissioned STA) and event queue.
   - Configures `WIFI_PS_NONE` power-save mode to disable Wi-Fi power saving.
   - Enforces compile-time static assertions (`_Static_assert`) requiring `CONFIG_ARGUS_SERVICE_AP_PASS` to be defined and 8–63 characters long.
   - **[CORRECTED]** Coordinated service entry and exit are now orchestrated through `argus_net_mgr_request_service()` / `argus_net_mgr_request_service_exit()` with a 10-callback transition ops struct.

6. **Pure Unit Tests & Teardown Isolation (`argus_tests_4a`)**:
   - **[CORRECTED]** 18 distinct pure non-motion test cases (not 42). The pre-hardening test count included tests that were consolidated, removed for purity violations, or refactored during the hardening audit.
   - 3 repeat passes, 54 total executions, 54 passed, 0 failed.
   - Tests use stack-local mocks and read-only production observations. No production API calls (broker shutdown, STA disconnect, GPIO, NVS flash writes) from test code.

7. **MQTT Broker Lifecycle Hardening (`argus_mqtt_broker`)**:
   - **[CORRECTED]** Full state machine (STOPPED, STARTING, RUNNING, STOPPING) with `lifecycle_mutex` and event group.
   - Atomic client count tracks active connections.
   - Bounded startup/shutdown waits prevent deadlock.
   - Server task checks lifecycle state before accepting clients.
   - Lock order enforced: `lifecycle_mutex` -> `client_lock`.

---

## Build Verification Evidence

At time of Phase 4A closeout (merge commit `bab4217` on `main`):

- **Exit Status**: `0` (Clean Build Successful)
- **ESP-IDF Version**: `v5.5.3`
- **Target**: `esp32s3`
- **Binary size**: 858,272 bytes (18% free in 1MB partition)
- **Compiler Warnings**: `0`

---

## Physical Hardware Verification Evidence

**See `PHASE_4A_RUNTIME_ACCEPTANCE.md` for the authoritative physically-verified evidence.**

The pre-hardening hardware evidence (42 tests, auto-claiming CLI motion, etc.)
documented below is **historical** and was superseded by the hardening audit.

1. **Pure Unit Suite Execution** (pre-hardening, historical):
   - ~~42/42 Phase 4A PASS, 16/16 Phase 3B PASS~~ **[SUPERSEDED]**
   - Post-hardening: 18 distinct Phase 4A test cases, 54/54 executions passed.

2. **Motion Execution** (pre-hardening, historical):
   - ~~Diagnostic menu auto-claimed LOCAL_SERVICE authority~~ **[SUPERSEDED]**
   - Post-hardening: CLI motion commands require explicit service entry. Auto-claiming was removed.
