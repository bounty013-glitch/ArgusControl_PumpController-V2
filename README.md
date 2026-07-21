# ArgusControl_PumpController-V2

Argus V2 Peristaltic Pump Controller firmware for ESP32-S3.

This repository implements the V2 architecture for precision motor control, featuring exact fixed-point Bresenham GPTimer pulse generation, a 20 ms linear trajectory profile engine, verified common-anode active-low hardware integration, an embedded local MQTT broker, and open-loop safety doctrine.

**Current Status**: Phase 4B.1 through Phase 4B.3, including the Phase 4B.3a Wi-Fi observability and recovery close-out, are complete and physically accepted. Phase 4B.4 Step 1 is accepted at `eb1a6cc`; Step 2 authenticated HTTP admission and router dispatch is in progress. Connected-motor and physical acceptance remain pending.

---

## Hardware Configuration & Polarity Doctrine

The physical wiring and logic levels between the ESP32-S3 and the UIM344 motor driver are physically verified on production hardware:

| UIM344 Signal | Wire Color | ESP32-S3 Pin | Polarity / Active Level | Behavior |
| :--- | :--- | :--- | :--- | :--- |
| **COM** | Brown | **`3.3 V`** | **Common-Anode Supply** | **MUST NOT** be connected to GND or 5V. |
| **PLS (STEP)** | Yellow | **`GPIO3`** | Active-Low ($15 \text{ us}$ pulse) | Idle state `HIGH` (optocoupler OFF). Asserting pulse drives `LOW` (optocoupler ON). |
| **DIR** | Gray | **`GPIO4`** | Inverted Polarity | Logical forward drives `LOW` (0V), reverse drives `HIGH` (3.3V). |
| **ENA** | Blue | **`GPIO5`** | Active-Low Output | Driving `LOW` energizes ENA optocoupler (holding torque). Driving `HIGH` disables driver (releases shaft). |

> [!CRITICAL]
> **Common-Anode Wiring Warning**: Connecting `COM` to `GND` (the legacy workaround) forced inverse current drive through the optocoupler diodes, degrading trigger sensitivity and causing mysterious, intermittent motor stalls. Wiring `COM` to `3.3 V` establishes proper common-anode operation.

---

## Architecture & Core Features

### 1. GPTimer Bresenham Pulse Engine ([main/argus_step_gen.c](file:///c:/Users/bount/Dev/Argus/ArgusControl_PumpController-V2/main/argus_step_gen.c))
*   Uses ESP32-S3 64-bit hardware `GPTimer` at 10 MHz resolution (100 ns ticks).
*   Implements exact rational period calculation ($T = \frac{600,000,000,000}{\text{milli\_rpm} \times \text{steps\_per\_rev}}$) with Bresenham remainder accumulation.
*   Eliminates accumulator rounding drift (0.000 ms cumulative error over $10^6$ steps).
*   Pulse duration is configured to $15 \text{ us}$ (150 ticks) to saturate slow optocoupler diodes safely at 3.3V.
*   Includes a 3 us (30 tick) safety clamp on `gptimer_set_alarm_action()` to prevent missed timer alarms under concurrent Wi-Fi / MQTT interrupt load.

### 2. Linear Trajectory Ramp Engine ([main/argus_trajectory.c](file:///c:/Users/bount/Dev/Argus/ArgusControl_PumpController-V2/main/argus_trajectory.c))
*   Operates via a periodic 20 ms FreeRTOS task.
*   Acceleration / Deceleration Limit: $10.0 \text{ output RPM/sec}$ ($10,000 \text{ mRPM/sec}$).
*   Per-tick Increment: $200 \text{ milli-RPM}$ ($0.2 \text{ output RPM}$) per 20 ms update.
*   Performs exact target clamping without overshoot.
*   Handles direction reversal through zero speed by decelerating, stopping pulses, toggling DIR during setup time, and ramping in the new direction.

### 3. Open-Loop Speed & Telemetry Architecture
Supervisory telemetry strictly distinguishes controller intent from physical motor movement:
*   `target_rpm_milli`: Requested output target speed setpoint.
*   `applied_rpm_milli`: Trajectory-limited speed sent to the pulse generator.
*   `generated_rpm_milli`: Active rate currently output by GPTimer step scheduling.
*   `generated_step_count`: Total generated pulse count.
*   *Open-Loop Note*: Without a physical encoder feedback source, generated pulses are not proof of physical motor shaft motion.

### 4. Diagnostic CLI & Recovery Path
An interactive serial CLI menu (`app_main.c`) provides comprehensive hardware diagnostic options:
*   `[1]` to `[8]`: Ramped target speeds ($0.5 \text{ RPM}$ to $200 \text{ RPM}$).
*   `[s]`: Normal soft-stop (ramps applied speed to 0, retains holding torque).
*   `[u]`: Unlock driver (halts pulses, drives ENA HIGH to release shaft).
*   `[r]`: Diagnostic Recovery (halts pulses, forces STEP inactive, drives ENA HIGH, waits 500 ms, leaves unlocked without requiring MCU reboot).
*   `[t]`: Manually run the complete pure non-motion suite. The suite derives and reports its registration/execution counts at runtime; compilation is not test-pass evidence.
*   `[N]`: Phase 4A Network & Authority acceptance submenu.

---

## Embedded Local MQTT Broker

The control node hosts a local MQTT broker listening on port `1883` on both its Wi-Fi Station IP and Access Point IP (`192.168.4.1`):

*   **AP SSID**: `Argus-Service-XXYYZZ` (derived from device MAC address)
*   **AP Security**: WPA2-PSK, single-client limit
*   **Broker Port**: `1883`
*   **Broker Lifecycle**: Full state machine (STOPPED → STARTING → RUNNING → STOPPING) with bounded startup/shutdown and atomic client tracking.
*   **Authority Model**: Exclusive command authority — MQTT supervisory, local browser, and diagnostic CLI authority never coexist. See `docs/PHASE_4A_RUNTIME_ACCEPTANCE.md` for verified behavior.

> [!NOTE]
> MQTT topic paths and payload schemas are preliminary. The production
> MQTT contract will be defined in Phase 4C.

---

## Build and Flash Instructions

Built with ESP-IDF `v5.5.3` for ESP32-S3:

```powershell
# Activate the verified ESP-IDF environment
. C:\esp\v5.5.3\esp-idf\export.ps1

# Build the project
idf.py build

# Flash to target device and open serial monitor
idf.py -p PORT flash monitor
```
