# Phase 4A On-Device Network & Authority Acceptance Report

This document records the physical ESP32-S3 hardware acceptance test results for Phase 4A Network Lifecycle, Exclusive Authority, Dual-Slot NVS Staging, and Security Safeguards in `ArgusControl_PumpController-V2`.

---

## Governing Safety Rule

> **One pump, one command authority.**
> At no time may MQTT supervisory control, browser local-service control, and diagnostic CLI control possess motion authority concurrently.

---

## Preconditions & Setup Notes

- **Scenario A Preconditions**: Requires empty or invalid mutable configuration in NVS. If the device is currently commissioned, reach the uncommissioned state ONLY through the explicitly confirmed factory reset procedure (`N -> F`). NVS is never erased automatically.
- **Oscilloscope Latency Measurement Boundary**: Software E-stop bypasses normal command serialization and requests immediate pulse termination. Actual command-to-STEP-inactive latency remains pending oscilloscope measurement and is not safety-rated. For the DSO-510 physical acceptance pass, record only:
  1. STEP signal becomes inactive.
  2. Motion stops immediately to human observation.
  3. E-stop latch is set.
  4. No subsequent STEP pulses are observed.
  5. No automatic restart occurs.

---

## Verification Matrix Summary

| Scenario | Test ID | Description | Status | Evidence / Notes |
| :--- | :--- | :--- | :--- | :--- |
| **Scenario A** | A.1 | Fresh Boot Uncommissioned AP Mode (`UNCOMMISSIONED_AP`) | `PASSED` | Verified on ESP32-S3 hardware |
| | A.2 | Service AP SSID `Argus-Service-XXYYZZ` & WPA2 Security | `PASSED` | SSID & WPA2 build credential active |
| | A.3 | Uncommissioned Authority Mode (`NONE/NONE`) & Motion Command Rejection | `PASSED` | Numeric keys 1..8 rejected as expected |
| | A.4 | Explicit Service Entry via `N -> 5` (`NONE/NONE` $\to$ `SERVICE_TRANSITION/NONE` $\to$ `LOCAL_SERVICE/DIAGNOSTIC_CLI`) | `PASSED` | Granted via explicit entry |
| **Scenario B** | B.1 | Diagnostic STA Credential Staging & NVS Dual-Slot Commit | `NOT RUN` | Staging `N -> 8`, Commit `N -> A` |
| | B.2 | Commissioned STA Boot (`COMMISSIONED_STA`) & MQTT Authority (`SUPERVISORY/MQTT`) | `NOT RUN` | Pending physical execution |
| | B.3 | Low-Risk MQTT Motion (20 RPM Ramp) | `NOT RUN` | Pending physical execution |
| **STA Loss** | B.4 | Incidental STA Loss During Motion (Trajectory Retained Fail-Operational) | `NOT RUN` | Disconnect Wi-Fi at 20 RPM |
| | B.5 | Reconnect STA & Stale Pre-Disconnect Envelope Rejection by Generation | `NOT RUN` | Gen checking rejects old env |
| **Scenario C** | C.1 | Enable Service AP Discoverability (`AP_DISCOVERABLE` / `APSTA` Mode via `N -> 4`) | `NOT RUN` | Pending physical execution |
| | C.2 | AP Association Alone Does Not Transfer Authority (`SUPERVISORY/MQTT` Preserved) | `NOT RUN` | Pending physical execution |
| | C.3 | Non-Owner Command Rejection (Browser / CLI non-mutating probe rejected) | `NOT RUN` | Probes `N -> 6` & `N -> 7` |
| **Scenario D** | D.1 | Active-Motion Service Entry (`SUPERVISORY` 20 RPM $\to$ `SERVICE_TRANSITION/NONE`) | `NOT RUN` | `N -> 5` during active 20 RPM |
| | D.2 | `SERVICE_TRANSITION` Controlled Stop Ramp & State Confirmation | `NOT RUN` | Pending physical execution |
| | D.3 | STA & MQTT Shutdown $\to$ Exclusive `LOCAL_SERVICE/DIAGNOSTIC_CLI` Granted | `NOT RUN` | Pending physical execution |
| **Scenario E** | E.1 | Local Owner Conflict (Browser / MQTT commands rejected under CLI owner) | `NOT RUN` | Pending physical execution |
| | E.2 | Concurrent Transition E-Stop Test (`N -> E` timer-fired E-stop during entry) | `NOT RUN` | Aborts entry; no local auth |
| | E.3 | Internal Software E-Stop Availability Across All Network & Authority Modes | `NOT RUN` | Bypasses authority matrix |
| **Scenario F** | F.1 | Controlled Service Exit & LKG Reboot (`N -> X`) | `NOT RUN` | Pending physical execution |
| **Scenario G** | G.1 | Controlled Factory Reset & Clear Dual-Slot NVS (`N -> F`) | `NOT RUN` | Pending physical execution |

---

## Physical Hardware Evidence (ESP32-S3 Pass 1 Verification)

- **Firmware Version Banner**: `v2-phase4a-dev`
- **Boot Lifecycle**: Clean uncommissioned SoftAP boot (`UNCOMMISSIONED_AP`), MQTT broker remained stopped, no listening socket created on port 1883, zero `broker not started` log errors.
- **Runtime Stability**: Stable uncommissioned operation observed for >210 seconds continuously.
- **Phase 3B Pure Unit Tests**: `PASSED`
- **Phase 4A Pure Unit Tests**: `PASSED` (36 Passed, 0 Failed across 3 consecutive passes)
- **Production Non-Mutation Proof**: `PASSED (100% Equal)`
- **Confirmed Final State Snapshot**:
  - Machine State: `UNLOCKED`
  - Configured Setpoint: `0 mRPM`
  - Applied RPM: `0 mRPM`
  - Network Mode: `UNCOMMISSIONED_AP`
  - Authority Mode: `NONE`
  - Authority Owner: `NONE`
  - Authority Generation: `2`
  - Driver Enabled: `false`
  - E-Stop Latched: `false`
  - Fault Code: `NONE (0)`

---

## Technical Highlights & Implementation Guardrails Verified

- **Single Component Build Graph**: Locked to ESP-IDF v5.5.3, single target compilation pass (`esp-idf/main/CMakeFiles/__idf_main.dir/`).
- **Secrets Protection**: `sdkconfig` untracked from Git; static preprocessor assertions (`_Static_assert`) enforce 8–63 char credential boundary. Passwords masked in logs and displays.
- **Explicit Authority Requirement**: Diagnostic CLI motion commands (`1`..`8`, `g`, `s`, `u`) require prior explicit service entry (`H` or `N -> 5`). Silent auto-claiming is strictly prohibited.
- **Non-Mutating Permission Probes**: Diagnostic probes `N -> 6` and `N -> 7` use `argus_cmd_router_check_authority()` to evaluate authorization without calling `argus_state_mgr` or altering setpoints, trajectory, or machine state. State Invariance Check verifies 15/15 fields remain identical.
- **Pure Test Isolation**: Phase 4A unit tests run on stack-local snapshots without mutating production singletons. Includes 3-pass production snapshot equality assertion.
