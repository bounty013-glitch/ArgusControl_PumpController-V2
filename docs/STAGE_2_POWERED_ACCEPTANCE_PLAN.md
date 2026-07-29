# Stage 2 — Powered Acceptance Plan

**Status (2026-07-28): S2-B SUPERSEDED, S2-C EXECUTED AND PASSED WITH
SHAWN-AUTHORIZED EQUIVALENT SUBSTITUTIONS. S2-D and S2-E deferred by Shawn
to the sustained pumping test.**
Execution record at the end of this document. Motor wired through a 10:1
planetary gearbox; the glass displays PUMP rpm, the motor turns ten times
faster — every number in this plan is a pump/glass number.

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
direction of rotation, motion that does not **begin visibly decelerating**
within 2 s of a STOP (a full ramp-down takes ~0.1 s per glass-RPM by
design — 7 s from 70 is the configured ramp working, not a failure; this
wording corrected 2026-07-28 after the first measured stop), motion with
the driver reported DISABLED, a fault that does not clear cleanly, any
smell or sound of binding, or a panel that shows a state the controller
does not.

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
4. After STOP, first confirm the controller reports `HOLDING`, zero
   applied/generated RPM, and the driver still enabled. The panel has no
   operator-reachable UNLOCK control in this stage, so use the existing
   **controller diagnostic serial console**:
   - Select `N` → `5` to request `LOCAL_SERVICE` authority as the
     diagnostic CLI.
   - Wait for `[SERVICE ENTRY COMPLETE]`, then select `0` to return to the
     main diagnostic menu.
   - Select `u` to issue UNLOCK through the controller's accepted command
     router.
   - Confirm `UNLOCKED`, zero applied/generated RPM, driver disabled, and
     that the shaft turns freely by hand.

   Entering `LOCAL_SERVICE` deliberately ends the panel's MQTT authority and
   may disconnect the panel. That is expected here because motion is already
   stopped; it must not be mistaken for a communications failure. Before
   S2-D, exit local service/reboot through the existing controlled path and
   confirm the panel reconnects and re-acquires authority before issuing any
   further motion command.

   **Why this step is here.** STOP leaves the driver energised and holding
   position — the motor is still powered and resisting rotation. UNLOCK is
   the separate action that de-energises it. The two are distinct machine
   states (`HOLDING` versus `UNLOCKED`) and the panel reports them
   differently, so this confirms the difference is real on the shaft and not
   only in the state word. It also leaves the bench in the safe state for
   fitting the pump head in S2-D: a coupling should never be fitted to a
   motor that is holding torque.

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
   and the pump has never stopped.

   **What the panel shows during the outage — corrected.** An earlier draft
   of this plan expected `ARGUS OFFLINE - LOCAL CONTROL ACTIVE` on the glass
   while the network was pulled. That is impossible and would have been
   scored as a failure wrongly: pulling the controller's network also cuts
   the panel's own link, so the panel cannot receive the authority state that
   wording is derived from. It goes `DATA_STALE` and then `DISCONNECTED`,
   showing its last known values clearly marked stale — which is the correct
   and truthful behaviour.

   `ARGUS OFFLINE - LOCAL CONTROL ACTIVE` is what the panel shows when it can
   still see the controller and the controller reports ArgusCore's lease as
   expired. That is a different scenario, and it is the one exercised by
   step 4 below rather than by a network pull.

   **What to check here instead:** the pump keeps running, the controller's
   telemetry never shows a setpoint change or a driver disable, and the panel
   marks its data stale rather than inventing a value.
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

## Execution record — 2026-07-28

Shawn at the bench driving the glass and, unannounced until afterwards, the
admin portal from a laptop on the controller's AP; serial capture was made on
both COM5 (controller, duplex with console) and COM18 (panel), each port opened
once before motion and closed once after the bench was safe. The contemporaneous
session record identified `s2_controller.log` (167 KB) and `s2_panel.log`
(223 KB), with host-timestamped annotations marking every console keystroke
sent. Those raw captures were not committed and are no longer available for
independent re-analysis. The execution record therefore relies on the
contemporaneous annotations and Shawn's physical observations; no rerun was
authorized solely to recreate the missing logs.

**Context discovered during the session, now governing every number here:**
the motor drives the pump through a **10:1 planetary gearbox** and the glass
displays PUMP rpm. The plan's ladder was written before that was stated:
"72 RPM" on the glass is 720 RPM at the motor. Also, the panel's setpoint
resolution is currently **5 RPM increments**, so the plan's 0.5/0.6/0.7/1.0
steps are unreachable from the glass (they remain reachable from the
diagnostic console, which is where those menu entries live).

### S2-B — SUPERSEDED, Shawn's determination

First motion had already occurred informally (Shawn wired the motor and ran
it before the formal session; every command accepted and acted on). The
step's gating purpose — verify signals BEFORE anything moves — was moot,
and the signal behaviour it protects against was demonstrated by operation.
Recorded as superseded, not skipped silently. The scope-level numbers
(pulse width, frequency at 0.5 RPM) remain unverified and available to
S2-B's procedure if ever needed.

### S2-C — PASSED WITH SHAWN-AUTHORIZED EQUIVALENT SUBSTITUTIONS

The written ladder (`0.5 → 5 → 20 → 72 RPM`) was not executed literally.
The panel cannot command 0.5 RPM because its current setpoint resolution is
5 RPM, and the session did not include a discrete 20-RPM hold. Shawn accepted
the actual powered run as equivalent evidence for this stage rather than
repeating the session solely to reproduce those two points. This disposition
must not be read as a claim that the original ladder was followed exactly.

**Ladder as actually run** (glass rpm): 5 → START → brief stop/restart →
~70 → 200 → STOP → 100 → STOP, then the formal measured run at 70 → STOP.
All exercised plan-envelope speeds (≤72) were smooth, with no stall or
cogging and with applied RPM tracking commanded — operator-observed on the
shaft, the glass, and the admin portal simultaneously.

**At 200 glass rpm (2000 motor rpm, discretionary, beyond the plan
ladder): the motor reached speed and then stalled.** The controller
truthfully continued reporting GENERATED output with `feedback_available`
false — a physical stall is invisible to an open-loop controller by
design, and the telemetry honestly claimed generation, not measurement.
This is the plan's own generated-vs-measured distinction demonstrated
live. Because 200 RPM was a discretionary extension beyond the written
≤72-RPM acceptance envelope, the stall ended that extension and is recorded
as a physical anomaly; it does not invalidate the exercised within-envelope
result. A stall at an acceptance-envelope speed would remain an S2-C failure.
Stall investigation (current limit, ramp, resonance) is deferred by Shawn;
70 glass rpm is his stated interim operational maximum and 200 will not be
a service speed.

**The 70-RPM limit is not currently enforced by the controller.** The
controller accepted 200 RPM and, without shaft feedback, could not detect the
stall. After S2-D establishes loaded displacement and behavior, a final
service maximum must be selected and enforced at the controller's
command-admission/configuration boundary before deployment. An HMI-only limit
is insufficient because other authorized command sources use the same
controller.

**Deceleration, measured from the state manager's transition log:**

| Stop from (glass) | Measured | Basis |
|---|---|---|
| 5 RPM | 0.53 s | wire STOP → `DECELERATING -> HOLDING` |
| 70 RPM | **7.04 s** | formal measurement, same basis |
| 100 RPM | 10.03 s | same |
| 200 RPM | 20.04 s | same (shaft stalled; generated ramp anyway) |

Four stops, all ≈0.10 s per glass-RPM: the decel ramp is linear at
~10 RPM/s pump (100 RPM/s motor), matching the configured trajectory.
Whether that rate is operationally right is an open tuning decision —
the plan predicted ramp tuning would be the adjustment point.

**Setpoint retention:** STOP retained the 70000 mRPM setpoint; so did
UNLOCK. No comms event, stop, or authority change cleared it at any point
in the session (fail-operational held throughout).

**S2-C.4 UNLOCK proof — PASSED, with three independent witnesses.**
Console `N` → `5`: `[SERVICE ENTRY COMPLETE]`, authority
`SUPERVISORY/MQTT` gen 3 → `LOCAL_SERVICE/DIAGNOSTIC_CLI` gen 5. As the
plan predicted, service entry reconfigured the network (`SERVICE_AP_ONLY`)
and the panel disconnected — the glass dropped to its non-authority
presentation (Shawn's observation), the laptop's portal session died
(Shawn's observation, exactly as designed), and the panel's serial showed
`mqtt=DISCONNECTED`, last-known data clearly marked stale, heartbeats
correctly stopped. Console `u` from HOLDING: `UNLOCKED`, `Driver Enabled:
NO`, zero output, fault 0 — **and the shaft turned freely by hand** where
minutes earlier it had held torque. HOLDING vs UNLOCKED is real on the
steel, not only in the state word.

**Controlled exit and autonomous recovery — PASSED.** Exit via `N` → `X`
(confirmed reboot, no configuration change). The controller booted to
`SUPERVISORY/MQTT` gen 3 at t=5.6 s; the panel — which never rebooted —
re-authenticated at t=22.7 s and was GRANTED 173 ms later. Total
controller/network/broker recovery to panel reacquisition was approximately
17 seconds. Once the necessary controller/broker state was available, the
panel's authority reacquisition sequence completed within 295 ms. The
sub-300-ms figure applies only to that final authority sequence, not to the
controller reboot as a whole:

```
#8  HOLDING->IDLE        cause=OWNERSHIP_LOST    (fresh controller: owner=NONE)
#9  IDLE->REQUESTED      cause=REQUEST_SENT       same tick
#10 REQUESTED->CONFIRMING cause=GRANTED           +295 ms, epoch=1 published
#11 CONFIRMING->HOLDING  cause=OWNERSHIP_OBSERVED same tick
```

The session-synchronization invariant then did its job unprompted: new
controller session, `last_accepted_seq=0` received, panel discarded its
old numbering (next_seq 21) and reported `next_seq=1 sync=READY`.

**Command integrity across the whole powered session: 20 commands sent,
20 accepted, 20 results resolved on the exact session/sequence/command-id
triple, 0 stale, 0 unmatched, 0 refusals of any class, and controller
`last_accepted_seq` in lockstep with the panel throughout. There were zero
command, authority, synchronization, or correlation anomalies. The separate
physical anomaly at the discretionary 200-RPM extension remains recorded
above.**

### Observations recorded for later

- Second AP client during the session was Shawn's laptop on the admin
  portal; the portal's Machine State dialog tracked RPM changes and the
  UNLOCKED transition accurately in real time.
- Panel setpoint resolution (5 RPM) vs the plan's sub-1-RPM steps —
  presentation decision for the polish phase.
- Ramp rate (~10 RPM/s pump) — tuning decision, open.
- 200-glass-rpm stall — deferred investigation, non-operational speed.
- Controller-enforced maximum — pre-deployment requirement after S2-D;
  interim 70-RPM operator limit is not yet an admission limit.
- Raw serial captures — generated during the session but not retained;
  numerical claims cannot now be independently re-derived from those logs.
- Serial port discipline that held all session: open each port once
  before motion, never close while energized; no device reset occurred at
  either open.

### Still pending in Stage 2

S2-D (pump head, displacement measurement) and S2-E (live
fail-operational proof under flow) — deferred to Shawn's sustained
pumping test, to be run with the same capture arrangement.

## Deliberately not in Stage 2

- ArgusCore participation (Stage 3).
- Certificates and encrypted transport (Stage 4).
- Panel graphics and feel tuning (Stage 5, your standing instruction).
- Software E-stop as a safety function. It is not one and will not be
  presented as one.
