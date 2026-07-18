# Phase 4A Implementation Walkthrough — Device Identity, Persistent Configuration, Network Modes, and Control Authority

## Summary of Completed Implementation & Hardening

Phase 4A has been fully implemented and physically verified on ESP32-S3 hardware in `ArgusControl_PumpController-V2` adhering strictly to all user design directives, lock hierarchy principles, single-authority model ("One pump, one command authority"), and secrets protection guardrails.

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
   - Includes full mock backend injection interface (`argus_nvs_mock_inject_*`) for pure unit testing without NVS flash writes.

3. **Exclusive Control Authority & Owner Tracking (`argus_authority_mgr`)**:
   - Enforces authority modes (`NONE`, `SUPERVISORY`, `SERVICE_TRANSITION`, `LOCAL_SERVICE`) and owner tracking (`NONE`, `MQTT`, `BROWSER`, `DIAGNOSTIC_CLI`).
   - Uses `argus_authority_mgr_get_snapshot()` to acquire `s_auth_mutex` briefly, copy snapshot atomically, and release it immediately to prevent lock inversion.
   - Implements permission matrix validation (`argus_authority_validate_permission()`).

4. **Command Router & Dispatch Serialization Gate (`argus_cmd_router`)**:
   - Serializes all state manager commands using `s_dispatch_mutex`.
   - Validates envelope source (`ARGUS_CMD_SRC_MQTT_SUPERVISORY`, `ARGUS_CMD_SRC_BROWSER_LOCAL`, `ARGUS_CMD_SRC_CLI_DIAGNOSTIC`, `ARGUS_CMD_SRC_INTERNAL_SAFETY`), authority generation, and permission before calling `argus_state_mgr`.
   - Microsecond preemption for `ARGUS_CMD_SRC_INTERNAL_SAFETY` and `ARGUS_CMD_TYPE_ESTOP` bypassing `s_dispatch_mutex`.
   - Emits `ESP_LOGW` warning diagnostics if envelope generation counter mismatches or authority permission is denied.

5. **Dedicated Network Manager Task (`argus_net_mgr`)**:
   - Manages Wi-Fi lifecycle (Uncommissioned AP vs. Commissioned STA) and event queue.
   - Configures `WIFI_PS_NONE` power-save mode to disable Wi-Fi power saving and reduce power-save-related latency variability (physical STEP timing and jitter under Wi-Fi load pending oscilloscope measurement).
   - Enforces compile-time static assertions (`_Static_assert`) requiring `CONFIG_ARGUS_SERVICE_AP_PASS` to be defined and 8–63 characters long.

6. **Comprehensive Pure Unit Tests & Teardown Isolation (`argus_tests_4a`)**:
   - 42 pure non-motion unit tests covering Identity, Dual-Slot CRC & Wrap, NVS Recovery, Authority & Owner matrix, Envelope Validation, Router Dispatch Serialization, and Net Manager config validation.
   - **Teardown Isolation**: Tests save `orig_snap` at entry and restore `orig_snap.mode` and `orig_snap.owner` before exiting so running `t` never leaks `SUPERVISORY` / `MQTT` authority to the live runtime.

7. **Interactive Diagnostic Menu Authority Auto-Claiming (`app_main`)**:
   - Selecting any motion option (`1`–`8`, `g`, `s`, `u`, `c`, `r`) in `argus_diagnostic_menu_task` automatically checks if CLI holds `LOCAL_SERVICE` authority (`AuthMode: 3`, `AuthOwner: 2`).
   - If CLI does not hold authority, it automatically requests service authority (`argus_authority_request_service(ARGUS_AUTH_OWNER_DIAGNOSTIC_CLI)`), updates the authority generation, and dispatches the envelope seamlessly.

---

## Build Verification Evidence

```powershell
. C:\Espressif\tools\Microsoft.v5.5.3.PowerShell_profile.ps1
idf.py build
```

- **Exit Status**: `0` (Clean Build Successful)
- **ESP-IDF Version**: `v5.5.3`
- **Target**: `esp32s3`
- **Application Binary**: `build/ArgusControl_PumpController-V2.bin` (size `0xc0ff0` bytes / 789,952 bytes)
- **Partition Free Space**: `0x3f010` bytes free out of 1.0 MB app partition (25% free)
- **Compiler Warnings**: `0` (with `-Werror` active)
- **Duplicate Compilation Passes**: `0` (single component compilation under `esp-idf/main/CMakeFiles/__idf_main.dir/`)
- **Header Overwrite Warnings**: `0` (`srec_cat` zero matches)

---

## Physical Hardware Verification Evidence

1. **Pure Unit Suite Execution**:
   - Executed option `t` twice consecutively on physical ESP32-S3 hardware.
   - 100% of Phase 3B and Phase 4A unit tests passed (`42/42 Phase 4A PASS`, `16/16 Phase 3B PASS`).
   - Verified authority state restored cleanly to `ARGUS_AUTHORITY_NONE` after test execution without leaking `SUPERVISORY` / `MQTT` mode.

2. **Motion Execution**:
   - Executed option `5` ("Set & Start 20.0 RPM (20000 mRPM)").
   - Diagnostic menu auto-claimed `LOCAL_SERVICE` authority (`AuthMode: 3`, `AuthOwner: 2`).
   - Machine state transitioned from `UNLOCKED` $\to$ `STARTING` $\to$ `RUNNING`.
   - Step generator frequency output matched exact setpoint (20.0 RPM $\to$ 26,666.667 Hz pulse rate).
