# V2 Pump Controller Baseline Audit & Hardware Architecture

This document establishes the baseline audit, hardware wiring specifications, failure mode learnings, and architectural principles for the Argus V2 Peristaltic Pump Controller redesign.

## Build Baseline

*   **ESP-IDF Version**: Verified as `v5.5.3`.
*   **Target MCU**: ESP32-S3.
*   **Clean Build Status**: Verified successful.
    *   Command: `. C:\Espressif\tools\Microsoft.v5.5.3.PowerShell_profile.ps1; idf.py build`
    *   Result: Compiles successfully, generating `ArgusControl_PumpController-V2.bin`.
    *   Partition info: App size is approximately 757 KB (26% free in smallest app partition).

## Active Repository Layout

```text
ArgusControl_PumpController-V2/
├── docs/                      <- Doctrine, V2 architecture specs, and hardware audits
│   ├── ARCHITECTURE_PRINCIPLES.md
│   ├── ARGUS_PHILOSOPHY.md
│   ├── DEVELOPMENT_RULES.md
│   ├── MQTT_STANDARDS.md
│   ├── NAMING_STANDARDS.md
│   ├── SAFETY_MODEL.md
│   ├── UI_PRINCIPLES.md
│   ├── V2_BASELINE_AUDIT.md   <- [THIS FILE] Baseline audit & hardware learnings
│   ├── V2_CONTROLLER_ARCHITECTURE.md
│   ├── V2_IMPLEMENTATION_PLAN.md
│   ├── PHASE_3B_WALKTHROUGH.md
│   └── WHY.md
├── hmi/                      <- [OBSOLETE] Archival Arduino CYD HMI reference (not used in V2)
│   └── README.md              <- Warning notice (OBSOLETE — DO NOT FLASH OR USE WITH V2)
├── main/                      <- ESP-IDF Application code
│   ├── CMakeLists.txt         <- Build configuration
│   ├── Kconfig.projbuild      <- Project-scoped configuration settings
│   ├── app_main.c             <- Startup, Wi-Fi AP/STA, local MQTT broker, status task, CLI menu
│   ├── argus_cmd_parser.c/h   <- Strict numeric/boolean command parser & retain policy seam
│   ├── argus_config.c/h       <- Hardware configuration parameters & Kconfig defaults
│   ├── argus_conversions.c/h  <- Exact fixed-point rational timing conversions
│   ├── argus_feedback.c/h     <- Open-loop feedback seam (returns unsupported by default)
│   ├── argus_mqtt_broker.c/h  <- Embedded local MQTT broker with transport-neutral policy seam
│   ├── argus_state_mgr.c/h    <- [PHASE 3B] Authoritative machine state manager & permission matrix
│   ├── argus_step_gen.c/h     <- GPTimer-based Bresenham step pulse scheduler
│   ├── argus_trajectory.c/h   <- Phase 3A linear trajectory ramp engine (20ms periodic task)
│   └── argus_tests.c/h        <- Pure unit test suite & interactive hardware acceptance runner
├── sdkconfig                  <- Build configuration
└── sdkconfig.defaults         <- Project defaults
```

## Verified Physical Wiring & Common-Anode Polarity Doctrine

The physical wiring between the ESP32-S3 and the UIM344 motor driver is physically verified on production hardware:

*   **UIM344 Brown `COM` -> ESP32 `3.3 V`**:
    > [!CRITICAL]
    > **Wiring Requirement**: The UIM344 `COM` wire MUST be connected to the ESP32 `3.3 V` rail and **MUST NOT** be connected to `GND` or `5V`.
    > In the legacy firmware, `COM` was connected to `GND` as a common-cathode workaround. This forced inverse current drive through the optocoupler diodes, degrading trigger sensitivity and causing mysterious, intermittent motor stalls. Wiring `COM` to `3.3 V` establishes proper common-anode operation.
*   **UIM344 Yellow `PLS` -> ESP32 `GPIO3`**: Step pulse input. Active-low polarity. Idle state is `HIGH` (3.3V, optocoupler OFF). Asserting a step pulls GPIO3 `LOW` (0V, optocoupler ON) for a `15 us` pulse width.
*   **UIM344 Gray `DIR` -> ESP32 `GPIO4`**: Direction input. Inverted polarity (`dir_inverted = true`). Logical forward drives `LOW` (0V), reverse drives `HIGH` (3.3V).
*   **UIM344 Blue `ENA` -> ESP32 `GPIO5`**: Driver enable input. Active-low polarity (`enable_active_low = true`). Pulling GPIO5 `LOW` energizes the ENA optocoupler and produces holding torque. Driving GPIO5 `HIGH` disables the driver and releases shaft holding torque.

## Hardware & Firmware Discoveries / Failure Modes Resolved

### 1. Optocoupler Pulse Width & Drive Latency
*   **Symptom**: Step pulses generated at 6 us were not producing motor rotation despite active step counts.
*   **Root Cause**: Driver input optocouplers at 3.3V drive voltage have a $3\text{--}10 \text{ us}$ turn-on latency. A 6 us pulse terminated before the phototransistor reached saturation threshold.
*   **Fix**: Expanded STEP active-low pulse duration from `6 us` (60 ticks) to `15 us` (150 ticks), giving slow optocouplers ample saturation margin.

### 2. GPTimer Alarm Misses Under Network Load
*   **Symptom**: Pulse generation froze completely under concurrent Wi-Fi / MQTT load.
*   **Root Cause**: High-priority network interrupts introduced $>6 \text{ us}$ latencies. The alarm callback calculated `next_alarm` in the past relative to the hardware counter, causing the match interrupt to be missed permanently.
*   **Fix**: Implemented a $3 \text{ us}$ (30 ticks) safety clamp on `gptimer_set_alarm_action()`. If `next_alarm` falls in the past or within 3 us, it is clamped to `now + 30`, guaranteeing future hardware match trigger.

### 3. GPTimer Hardware Restart Post-Stop
*   **Symptom**: Re-commanding motion after a stop, unlock, or recovery sequence resulted in `generated_rpm_milli = 0` and `err = 2`.
*   **Root Cause**: `argus_step_gen_stop_immediate()` invokes `gptimer_stop(s_timer)`, placing the hardware timer into the STOPPED state. Re-commanding motion set `s_running = true` but did not restart the GPTimer counter.
*   **Fix**: Added `gptimer_start(s_timer)` checks to `argus_step_gen_start()` and `argus_step_gen_set_rate_rpm_milli()` kickstart logic.

### 4. Step Generator Minimum Speed Technical Floor
*   **Symptom**: Ramping from 0 mRPM threw `err = 2` on initial steps ($200 \text{ mRPM}$ and $400 \text{ mRPM}$).
*   **Root Cause**: `argus_step_gen_set_rate_rpm_milli()` enforced a hardcoded validation floor of $500 \text{ mRPM}$ ($0.5 \text{ RPM}$), rejecting valid intermediate ramp steps.
*   **Fix**: Lowered the pulse-generator technical floor to $1 \text{ mRPM}$ ($0.001 \text{ RPM}$).

### 5. Instantaneous Acceleration Motor Stall
*   **Symptom**: Jumping directly from 1 RPM to 20 RPM or 200 RPM caused audible loss of synchronization and UIM missing-step fault.
*   **Root Cause**: Step rate outran rotor mechanical inertia.
*   **Fix**: Implemented Phase 3A linear trajectory engine ($10.0 \text{ output RPM/sec}$, $200 \text{ mRPM}$ per $20 \text{ ms}$ tick), ensuring smooth linear acceleration and deceleration without overshoot.

## Phase 3B Authoritative State Manager Summary

*   **States**: `BOOTING`, `UNLOCKED`, `STARTING`, `RUNNING`, `DECELERATING`, `HOLDING`, `EMERGENCY_STOPPED`, `RECOVERING`, `FAULTED`.
*   **Deterministic Initial Boot**: `BOOTING -> UNLOCKED`.
*   **Setpoint Isolation**: `SET_TARGET` in `UNLOCKED` or `HOLDING` updates stored setpoint without enabling driver or producing STEP pulses. Motion starts only upon `START`.
*   **Latched Software E-Stop**: `E_STOP` halts pulses immediately, preserves driver state (`ENA LOW` if enabled, `ENA HIGH` if disabled), and latches `EMERGENCY_STOPPED`. Requires explicit `RESET_ESTOP` to return to `HOLDING` or `UNLOCKED` without auto-restart.
*   **Lock Hierarchy**:
    1. `s_command_mutex`
    2. `s_traj_mutex`
    3. `s_lifecycle_mutex`
    4. `s_timing_mux`
*   **Open-Loop Philosophy**: Telemetry fields `target_rpm_milli`, `applied_rpm_milli`, `generated_rpm_milli`, and `generated_step_count` represent controller intent and pulse generation, not verified physical movement.
