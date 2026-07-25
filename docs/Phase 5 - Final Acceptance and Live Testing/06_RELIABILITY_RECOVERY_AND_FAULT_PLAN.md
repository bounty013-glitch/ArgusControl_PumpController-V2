# Reliability, Recovery, and Fault Plan

**Status:** TEST MATRIX NOT APPROVED

## 1. Purpose

This campaign proves that the declared release profile remains understandable,
bounded, and recoverable through credible electrical, mechanical, network, and
operator disturbances. It does not inject faults that create uncontrolled
pressure, chemical exposure, or mechanical hazard.

## 2. Criteria to Freeze

| Item | Approved value |
|---|---|
| Continuous endurance duration | `[REQUIRED]` |
| Start/stop cycle count | `[REQUIRED]` |
| Forward/reverse cycle count | `[REQUIRED or N/A]` |
| Reboot count | `[REQUIRED]` |
| Cold power-cycle count | `[REQUIRED]` |
| Maximum current by test point | `[REQUIRED]` |
| Maximum component/fluid temperatures | `[REQUIRED]` |
| Maximum pressure and excursion duration | `[REQUIRED]` |
| Permitted flow drift over endurance | `[REQUIRED]` |
| Tubing inspection/replacement interval | `[REQUIRED]` |
| Leak acceptance | `ZERO visible leak unless a stricter measurable criterion is defined` |
| Panic/watchdog/heap/stack/task failures | `ZERO` |
| Uncommanded motion or restart | `ZERO` |

Suggested planning baseline, not an approved criterion: one 8-hour continuous
run and 100 controlled start/stop cycles. The owner must approve or replace these
values based on intended duty and risk before execution.

## 3. Thermal and Electrical Soak

At the approved representative continuous load:

- record controller, driver, motor, pump head, tubing, fluid, ambient, supply
  voltage, current, pressure, flow, state, and generated output at the approved
  interval;
- preserve startup, warm-up, steady-state, and cooldown data;
- verify no thermal runaway, throttling, drift outside criteria, stall, step
  loss, leak, tube walk, or reset;
- verify health publication remains bounded and truthful; and
- inspect stack high-water marks and heap/task stability before and after.

Thermal limits come from approved component/manufacturer specifications and the
release profile, not from "it felt warm."

## 4. Cycle Campaign

Run the approved number of:

- Start to stable delivery;
- normal Stop to physical zero/HOLDING;
- Unlock to driver disabled;
- setpoint-only no-motion update;
- permitted direction reversal through zero;
- software E-stop and reset without automatic restart; and
- authority transfer/service entry/exit cycles where applicable.

Record failure count, timing drift, state/physical disagreement, and tube wear.
Automated cycle tooling must still route through production interfaces and must
have independent physical stop supervision.

## 5. Network and Supervisory Faults

Prove, at safe approved conditions:

- Wi-Fi STA loss and restoration;
- MQTT client disconnect/reconnect;
- heartbeat expiry;
- broker lifecycle restart while stationary;
- browser disconnection/session expiry;
- SoftAP client disconnect/reconnect;
- duplicate MQTT client ID;
- stale broker session and stale sequence rejection; and
- authority invalidation during service transition.

Fail-operational behavior is intentional: heartbeat/MQTT loss must not
automatically stop. This behavior must be compatible with the declared process
or the release is blocked pending an independent protection.

## 6. Controller Restart and Power Loss

### Stationary

Test controlled reboot, reset, and cold power cycle from `UNLOCKED` and zero
output. Require clean boot, no automatic motion, valid configuration/security
state, new broker command session, and truthful authority/network recovery.

### While Running

Only after explicit safety review, containment, and a test-specific powered gate:

- remove controller/motor power using the approved physical method;
- verify motion and delivery cease as physically expected;
- verify no hazardous pressure/siphon consequence;
- restore power without issuing a command;
- require boot to stationary `UNLOCKED`, zero output, driver disabled, and no
  stale command replay; and
- require fresh authority/session/sequence before further motion.

This is not a substitute for a safety-rated power-removal design.

## 7. Storage and Recovery

Without exposing credentials:

- verify normal reboot persistence of commissioned identity, network,
  security, machine directory, and any accepted calibration data;
- verify configuration factory reset behavior against its accepted boundary;
- verify physically local security recovery entry and authenticated exit;
- verify reset/recovery never starts motion, clears an E-stop improperly, or
  changes process state silently; and
- verify calibration preservation/erasure behavior matches the approved
  Profile A/B contract.

Destructive tests use a backed-up, reproducible test configuration and cannot be
performed on the sole evidence artifact without an approved restoration plan.

## 8. Mechanical and Fluid Fault Observations

Safe bounded observations may include:

- loss of prime;
- inlet starvation without damaging the pump/tube;
- approved low-level backpressure increase;
- tube wear progression;
- discharge relief activation; and
- driver missing-step lock behavior at a safe condition.

Do not create a blocked discharge, rupture, chemical release, overpressure, or
unguarded stall to "see what happens." If the controller cannot observe a driver
or process fault, document that limitation instead of fabricating detection.

## 9. Failure Classification

| Class | Meaning | Response |
|---|---|---|
| Safety critical | Could cause injury, release, overpressure, unexpected motion, or loss of required protection | Stop campaign; release blocked |
| Functional blocker | Violates declared state, control, recovery, accuracy, or persistence contract | Correct, review, and repeat affected/downstream gates |
| Reliability blocker | Panic, reset, leak, drift, thermal, task/heap/stack, or endurance failure | Root cause and repeat full reliability gate |
| Evidence defect | Test result cannot be traced or criteria were not frozen | Result invalid; repeat test |
| Bounded limitation | Outside declared release profile and truthfully documented | Acceptance authority disposition required |

## 10. Final Reliability Evidence

The report includes raw time series, cycle counts, thermal plots, current and
pressure logs, flow drift, tube inspections, reset reasons, stack/heap/task
observations, fault chronology, corrections, and the final stationary state.
