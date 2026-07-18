# Phase 3B Walkthrough — Authoritative State Manager, Command Arbitration, and Test Isolation

## Overview

Phase 3B established the authoritative controller state layer (`argus_state_mgr`) above the physically validated GPTimer step pulse generator and 20 ms linear trajectory profile engine.

---

## Key Achievements

### 1. State Manager & Permission Matrix ([main/argus_state_mgr.c](main/argus_state_mgr.c))
*   **Deterministic Machine States**: `BOOTING`, `UNLOCKED`, `STARTING`, `RUNNING`, `DECELERATING`, `HOLDING`, `EMERGENCY_STOPPED`, `RECOVERING`, `FAULTED`.
*   **Initial Boot Transition**: `BOOTING -> UNLOCKED`.
*   **Permission Matrix**:
    - `SET_TARGET` in `UNLOCKED`/`HOLDING` updates stored setpoint without enabling driver or producing STEP pulses.
    - `START` requires a valid nonzero setpoint, enables driver (`ENA LOW`), commands trajectory target, and transitions `UNLOCKED/HOLDING -> STARTING -> RUNNING`.
    - `STOP_NORMAL` preserves setpoint for later restart and transitions `RUNNING -> DECELERATING -> HOLDING`.
    - `UNLOCK` is rejected during motion/deceleration; when speed is zero, disables driver (`ENA HIGH`) and transitions to `UNLOCKED`.
*   **E-Stop & Preemption**:
    - `E_STOP` bypasses `s_command_mutex` to preempt normal commands without blocking.
    - Software E-stop bypasses normal command serialization and requests immediate pulse termination. Actual command-to-STEP-inactive latency remains pending oscilloscope measurement and is not safety-rated.
    - Increments `command_generation` counter, causing any in-flight `START` or `RECOVER` command to abort immediately without touching motion hardware or overwriting `EMERGENCY_STOPPED`.
    - If driver was enabled at E-stop time, retains holding torque (`ENA LOW`) and sets reset destination to `HOLDING`. If driver was disabled (`UNLOCKED`), retains `ENA HIGH` and sets reset destination to `UNLOCKED`. Never energizes a disabled driver.

### 2. Lock Hierarchy & Non-Blocking State Pattern
To eliminate deadlock risks, locking strictly follows a top-down order:
1. `s_command_mutex` (Command serialization)
2. `s_traj_mutex` (Trajectory mutex)
3. `s_lifecycle_mutex` (Step generator lifecycle mutex)
4. `s_timing_mux` (ISR timing spinlock)

`s_state_mutex` is held briefly ONLY when reading or modifying state variables, and **NEVER** held across lower-layer API calls, 20 ms enable delays, 500 ms recovery delays, network operations, or logging.

### 3. Extracted Strict Command Parser ([main/argus_cmd_parser.c](main/argus_cmd_parser.c))
*   Replaced `atoi()` with strict numeric validation (`strtol`), rejecting empty, non-numeric (e.g. `50rpm`), negative, or out-of-range (>100) payloads.
*   Parsed booleans (`true`/`false`, `1`/`0`, `on`/`off`) strictly. `unlock=false` is parsed as valid syntax but handled as a no-op command by `app_main.c`.
*   Integrated transport-neutral broker policy seam `argus_cmd_parser_validate_control_message` to reject retained control commands before storage or delivery.

### 4. Pure Unit Test Isolation & Mock Operations Table ([main/argus_tests.c](main/argus_tests.c))
*   Refactored `argus_tests_run_all()` to use a stack-local `argus_state_core_t` instance with an in-memory mock operations table `s_mock_ops`.
*   Pure tests execute production state-transition logic with **0 hardware mutation** and **0 task/mutex leaks**.
*   Isolated hardware acceptance motion tests into `argus_tests_run_hardware_acceptance()` behind explicit CLI trigger (`H`) and individual test confirmations.

### 5. Truthful Telemetry
*   Telemetry publishes uppercase state strings (`UNLOCKED`, `STARTING`, `RUNNING`, etc.), setpoint, applied RPM, generated RPM, step count, driver enable, direction, ramp active, E-stop latch, fault code, and feedback availability (`false`).
*   Generated values are never labeled as actual motor speed or actual position.

---

## On-Device Pure Unit Test Verification Evidence

Two consecutive executions of CLI option `t` were performed on the target ESP32-S3 hardware. Both executions completed with 100% passing assertions:

```text
I (1250) argus_tests: Running PURE non-motion unit tests (stack-local core, 0 tasks, 0 mutexes, 0 hardware touch)...
I (1255) argus_tests: [PASS] Strict Command Parser tests
I (1260) argus_tests: [PASS] Isolated State Core Permissions & Setpoint Isolation tests
I (1265) argus_tests: [PASS] Injected Error Propagation & Recovery tests
I (1270) argus_tests: [PASS] Production Singleton Isolation tests (0 live mutations, 0 task leaks)
I (1275) argus_tests: All PURE non-motion unit tests PASSED successfully.
```

### Execution Findings & Metrics
*   **Singleton Non-Mutation**: The production state manager singleton (`s_prod_core`) remained 100% unchanged before and after test execution.
*   **Task Leak Audit**: Zero task leaks were detected.
*   **Controller Post-Test Snapshot**: Controller returned cleanly to `state=UNLOCKED`, `target_rpm_milli=0`, `applied_rpm_milli=0`, `generated_rpm_milli=0`, `steps=0`, `enabled=0`, `estop=0`, `fault=0`.
*   **Expected Negative-Path Stimuli Logs**: Warning and error log entries emitted during test execution represent intentional negative-path stimuli:
    - Retained control messages are intentionally rejected by transport policy checks.
    - Invalid state commands are intentionally rejected by core state permissions.
    - Lower-layer error injection intentionally forces state transition to `FAULTED` to prove recovery logic.
    - These expected logs are followed immediately by passing assertions and do not indicate uncontrolled faults.

---

## Unloaded Hardware Acceptance Verification Evidence

The interactive hardware acceptance sequence (`H` menu) was manually executed on real target hardware.

### Verified Hardware Acceptance Test Results
*   `[PASS]` Setpoint isolation while `UNLOCKED` (updating speed setpoint produces 0 STEP pulses and keeps `ENA HIGH`).
*   `[PASS]` Low-speed start and ramp (0.5 output RPM / 66.67 Hz STEP pulse generation).
*   `[PASS]` Normal deceleration to `HOLDING` (ramps to 0 RPM, retains holding torque `ENA LOW`).
*   `[PASS]` Driver unlock and shaft release (`ENA HIGH`, releasing holding torque).
*   `[PASS]` Software E-stop while running (instant pulse termination, latches `EMERGENCY_STOPPED`).
*   `[PASS]` E-stop latch enforcement (rejects `START`, `SET_TARGET`, and `UNLOCK` while latched).
*   `[PASS]` E-stop reset without automatic restart (`RESET_ESTOP` clears latch to `HOLDING`/`UNLOCKED` without restarting motion).
*   `[PASS]` Direction reversal through zero speed (ramps to 0, toggles DIR, ramps up in reverse).
*   `[PASS]` Full-speed ramp to 200 output RPM (26.67 kHz STEP pulse generation).
*   `[PASS]` Full-speed normal deceleration to zero.
*   `[PASS]` Submenu interaction safety (did not auto-chain tests; required explicit key confirmation `'y'` for each test; clean input reader prevented menu loops).
*   `[PASS]` Physical hardware stability:
    - No unexpected physical motion
    - No audible stall or loss-of-synchronization buzz
    - No abnormal driver indication
    - No watchdog reset
    - No stuck state transition
    - No post-test STEP pulse generation

> **Unloaded Test Boundary Notice**: This sequence was performed as an **unloaded motor hardware acceptance test** (no pump head or process fluid attached).
> It does **NOT** claim verification of:
> 1. Loaded pump torque
> 2. Xanthan-gel process fluid performance
> 3. Flow rate calibration
> 4. Physical motor feedback
> 5. Mechanical stall detection
> 6. Measured command-to-STEP-inactive E-stop latency (pending oscilloscope measurement)
> 7. Long-duration loaded reliability

---

## Open-Loop Software Recovery & Feedback Boundaries

*   **Feedback Limitations**: In an open-loop architecture with no physical quadrature encoder, software recovery cannot verify physical motor motion, position, holding torque, or mechanical stall resolution.
*   **Software Recovery Contract**: Production recovery verifies immediate-stop API success (`ESP_OK`), driver-disable API success (`ESP_OK`), software error-latch clearing, and fresh software snapshots confirming `latched_error == 0`. Software state transitions to `UNLOCKED` upon success. Motion is never restarted automatically.

---

## Planned Work Deferred to Phase 4

- Dynamic dynamic topic namespace (`argus/<client>/<unit>/<category>/...`).
- Service Portal Wi-Fi AP credential redesign (unique device AP SSID, fixed `192.168.4.1` address).
- Browser commissioning interface.
