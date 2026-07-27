# Stage 2 — Powered Acceptance Plan

**Status: NOT STARTED. Preparation only — nothing here has been executed.**

Stage 1 gives the panel real command authority with the motor disconnected.
Stage 2 is the first time this firmware turns a shaft. It is written so that
bench time is execution rather than authoring, and so that every number has
an expected value and an abort condition **before** anything moves.

**Gate: Stage 1 accepted, panel holding control, all Stage 1 evidence
recorded.** Do not start otherwise.

---

## Safety frame

Every step below assumes:

- One person at the controller's physical disconnect at all times.
- Nobody's hands near the pump head or coupling while the driver is enabled.
- The panel's STOP and the physical disconnect are both within reach.
- Software E-stop is **not** safety-rated. It is a software preemption path.
  The physical disconnect is the safety device.

**Abort immediately, and record it, if any of these occur:** an unexpected
direction of rotation, motion that does not stop within 2 s of a STOP,
motion with the driver reported DISABLED, a fault that does not clear
cleanly, any smell or sound of binding, or a panel that shows a state the
controller does not.

---

## S2-B — Signal verification, motor inhibited

**Motor coupling disconnected. Driver enable line left disconnected.**
Scope on STEP, DIR and ENABLE.

| Command | Expected | Tolerance |
|---|---|---|
| 0.5 RPM | 66.67 Hz on STEP | ±0.1 Hz |
| 0.5 → 0.6 RPM | clean frequency step, no discontinuity | no missed or doubled pulses |
| any | STEP pulse width | > 3 µs |
| enable | ENABLE LOW = enabled | already verified on this hardware |
| direction | DIR stable through a speed change | no glitch |

Also confirm from the controller: generated pulses are reported as
**generated**, never as measured motion, and `feedback_available` stays
false. Open-loop output must never be presented as shaft feedback.

**Record:** scope captures at 0.5 and 20 RPM, and the console telemetry
alongside each.

## S2-C — First powered motion, bare motor, no pump head

Motor connected, **no pump head, no tubing, nothing in the flow path.**

1. 0.5 RPM from HOLDING. Confirm rotation, direction, and that it stops on
   STOP within the ramp time.
2. Step 0.5 → 5 → 20 → 72 RPM. At each: shaft turning smoothly, no stall, no
   audible cogging step, applied RPM tracking commanded.
3. STOP from 72 RPM. Record the deceleration time and compare against the
   configured ramp.
4. UNLOCK and confirm the shaft releases.

**What is being tuned here:** trajectory ramp acceleration against real
inertia. The current value has only ever been exercised against a simulated
load. Expect to adjust it; record before and after.

**Abort if:** the motor stalls at any commanded speed, direction is wrong,
or STOP does not visibly decelerate within 2 s.

## S2-D — Pump head and displacement scaling

Pump head fitted, tubing installed, **water only.**

1. Prime at low speed. Confirm flow direction matches the panel's direction.
2. At 20 RPM, collect into a graduated vessel for 60 s. Record measured
   volume.
3. Repeat at 72 RPM and at 100 RPM.
4. Compute actual mL/rev and compare against the configured displacement.

**This is a measurement, not a confirmation.** The configured value is
arithmetic from the tubing spec. If measured differs by more than a few
percent, the measured value wins and the configuration changes.

**Record:** three volumes, three durations, computed mL/rev, and the
configuration before and after.

## S2-E — Live fail-operational proof

**The step that justifies the entire architecture.** Everything else in both
repositories exists to make this true.

Pump running at 72 RPM under panel control, water flowing.

1. **Pull the controller's network.** Expect: the pump **keeps running at 72
   RPM**. The panel goes stale and says so. Nothing stops, nothing ramps
   down, the setpoint is not cleared.
2. Wait past the six-second lease timeout. Expect: the controller reports
   the lease expired and the supervisor link OFFLINE. **The pump is still
   running.**
3. Restore the network. Expect: the panel reconnects, re-acquires control,
   and the pump has never stopped. The panel should read
   `ARGUS OFFLINE - LOCAL CONTROL ACTIVE` during the outage and return to
   `LOCAL CONTROL ACTIVE` after.
4. Repeat with the **panel** powered off instead of the network. Same
   expectation: the controller keeps pumping.

**Any stop, ramp-down, driver disable or setpoint clear during this test is a
STAGE 2 FAILURE**, not a tuning issue. It means something in the comms path
is wired to motion, which is the one thing the fail-operational rule
forbids. Record it and stop.

**Record:** video if practical, plus controller telemetry across the whole
outage and the panel's log either side.

---

## Instrumentation available

Already on the devices, no new build needed:

| Source | Gives you |
|---|---|
| Controller console `[i]` | one status snapshot |
| Controller console `[b]` | admission pools, capacity, heap, tasks |
| Controller console `[t]` | full diagnostic suite |
| Controller telemetry topics | configured/trajectory/applied/generated RPM, step count |
| Panel `AUTHORITY-EVIDENCE` | owner, local control, lease, epoch, holds_authority |
| Panel `COMMAND-EVIDENCE` | sent, refusals by cause, results applied |
| Panel `ACQUIRE-EVIDENCE` | acquisition state, heartbeats, releases |

If S2-E needs finer resolution than 1 Hz telemetry, say so before the
session and I will add a higher-rate capture — it is a small change and much
easier to make before the pump is wet.

---

## Deliberately not in Stage 2

- ArgusCore participation (Stage 3).
- Certificates and encrypted transport (Stage 4).
- Panel graphics and feel tuning (Stage 5, your standing instruction).
- Software E-stop as a safety function. It is not one and will not be
  presented as one.
