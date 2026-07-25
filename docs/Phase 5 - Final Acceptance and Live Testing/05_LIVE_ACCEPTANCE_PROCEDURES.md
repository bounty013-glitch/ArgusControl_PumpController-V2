# Live Acceptance Procedures

**Status:** NOT STARTED

Every test has:

- initial condition;
- operator action;
- expected result frozen before execution;
- actual result;
- evidence references; and
- `PASS`, `FAIL`, `INVALID`, or `INCOMPLETE`.

Commands are issued one at a time. HTTP/MQTT success proves admission and
dispatch only; the operator separately records physical behavior.

## Human and Agent Execution Workflow

For every numbered live procedure, Codex or another executor must:

1. identify the test and its purpose;
2. verify repository identity, exact candidate commit, firmware identity,
   controller UID, and approved frozen-profile revision;
3. verify prerequisites, equipment, safety observer, and immediate-stop controls;
4. tell Shawn the precise physical action or observation required;
5. pause before the physical action;
6. continue only after Shawn reports the requested observation;
7. capture controller, browser, MQTT, serial, instrument, and operator evidence;
8. record the observation without embellishment;
9. calculate derived results with the approved method;
10. compare results only with criteria frozen before execution;
11. declare `PASS`, `FAIL`, `INVALID`, or `INCOMPLETE`;
12. preserve failed and invalid evidence;
13. update the evidence manifest and trace matrix; and
14. state the next safe test or stop condition.

The executor must never infer physical motion, direction, flow, temperature,
pressure, leakage, tube condition, or safety from firmware logs, responses, or
generated telemetry.

Every physical procedure below records the exact frozen-profile revision from
`02_BASELINE_AND_RELEASE_PROFILE.md`. A `REQUIRES SHAWN CONFIRMATION` dependency
makes the procedure `INCOMPLETE` and prohibits execution.

## Test 1 - Baseline Identity

**Initial condition:** Clean repository and accepted Phase 4D.4 baseline.

**Action:**

- verify local/remote branch descends from planning-inclusive commit
  `6d907a11e294fde26a33fbb60b898839883e1490`;
- separately verify accepted firmware/source baseline
  `31ea4254992f296001d367cece70998659a82783` and tag
  `v2-phase4d.4-machine-client-auth-accepted`;
- verify firmware identity, ELF, MAP, BIN, partition table, build metadata, and
  SHA-256 hashes;
- verify no credentials or temporary artifacts are included; and
- verify the configured release profile matches the physical assembly.

**Expected:** One coherent candidate and no unexplained changes.

**Actual:** `[PENDING]`

**Evidence:** `[PENDING]`

**Result:** `[PENDING]`

## Test 2 - Source Review

Audit:

- one normal browser path and one normal MQTT path terminate at
  `argus_cmd_router_dispatch()`;
- no new direct state-manager, trajectory, step-generator, motor, or GPIO bypass;
- authority generation is server/controller-owned at the correct boundary;
- E-stop priority remains intact;
- diagnostic paths cannot masquerade as production acceptance;
- flow/calibration claims match actual production source;
- active docs and runtime identity agree; and
- all three required independent reviews are closed.

**Actual:** `[PENDING]`

**Evidence:** `[PENDING]`

**Result:** `[PENDING]`

## Test 3 - Clean Build

Use ESP-IDF v5.5.3 only:

```powershell
idf.py --version
idf.py fullclean
$env:CCACHE_DISABLE="true"
idf.py build
idf.py size
```

Audit exact compiler warnings/errors, embedded JavaScript syntax, credentials,
temporary files, partition usage, static RAM, task stacks, and `git diff --check`.

**Expected:** Zero warnings, zero errors, accepted size/headroom, reproducible
hashes, and no secret or artifact contamination.

**Actual:** `[PENDING]`

**Evidence:** `[PENDING]`

**Result:** `[PENDING]`

## Test 4 - Flash and Boot

Verify COM5 or the recorded release port identifies the intended ESP32-S3.
Flash the full-clean image and capture from reset.

Require:

- expected firmware and project identity once;
- expected hardware UID and commissioned identity;
- valid configuration/security/machine directories;
- truthful network and broker lifecycle;
- coherent authority;
- `UNLOCKED`, zero configured/applied/generated output, driver disabled;
- `feedback_available=false` unless a separately accepted feedback provider now
  exists; and
- no panic, reset loop, watchdog, brownout, assertion, stack/heap failure, or
  credential disclosure.

**Actual:** `[PENDING]`

**Evidence:** `[PENDING]`

**Result:** `[PENDING]`

## Test 5 - Preflight Suites

Run diagnostic option `t` three complete invocations through a genuine
interactive terminal.

Baseline expectation before any Phase 5 test additions:

- 268 distinct registered tests;
- three internal passes per invocation;
- 804 executions per invocation;
- 2,412 executions across three invocations; and
- zero failures.

If the count changes, reconcile every registration and document the reason before
claiming PASS.

Record before/after:

- authority mode/owner/generation;
- network mode and STA lifecycle;
- broker state;
- machine state, target, outputs, direction, E-stop, fault, and driver;
- task count and implicated stack high-water marks; and
- reset reason and heap integrity.

Require zero motor motion and zero production-state contamination.

**Actual:** `[PENDING]`

**Evidence:** `[PENDING]`

**Result:** `[PENDING]`

## Test 6 - Electrical Timing and Driver Configuration

After the existing pre-power inspection and bounded operator confirmation:

- begin at the confirmed minimum test speed;
- keep the motor mechanically safe and unloaded;
- do not install or operate the pump as a hydraulic load;
- do not introduce fluid or perform wet operation;
- verify STEP idle high and 15 microsecond active-low pulse;
- verify DIR polarity and setup/hold timing;
- verify ENA inactive at boot and enable-before-STEP ordering;
- verify output pulse rate at approved low/mid/high unloaded points;
- compare measured frequency with fixed-point expectation;
- verify UIM microstep, working current, idle current, and missing-step threshold;
- observe supply voltage/current and component temperatures; and
- verify Stop, Unlock, and software E-stop electrical outcomes.

Do not exceed the approved test envelope merely to exercise the firmware maximum.

**Actual:** `[PENDING]`

**Evidence:** `[PENDING]`

**Result:** `[PENDING]`

## Test 7 - Unloaded Motion

Use the production command path where possible:

- setpoint without Start causes no motion;
- low-speed Start is smooth and direction is recorded;
- incremental approved speed points do not buzz, stall, jump, or lose steps;
- normal Stop decelerates smoothly to physical zero and enters `HOLDING`;
- Unlock disables the driver and releases holding torque;
- reverse traverses zero before changing physical direction;
- software E-stop halts pulse generation and latches truthfully;
- commands rejected while latched cause zero motion;
- reset clears the latch without automatic restart; and
- Recover ends stationary and unlocked without lower-layer fault.

**Actual:** `[PENDING]`

**Evidence:** `[PENDING]`

**Result:** `[PENDING]`

## Test 8 - Pump Installation and Prime

After setup inspection and a new bounded powered confirmation:

- treat this as the first wet/hydraulic Phase 5 procedure;
- install the approved pump head and tubing;
- prime only with water or another expressly approved benign fluid;
- keep the discharge open and free for the initial wet run;
- inspect for leak, tube walk, trapped gas, cavitation, and siphon;
- confirm setpoint-only isolation;
- Start at the lowest approved speed and zero/minimum discharge pressure;
- verify the intended physical flow direction;
- Stop and quantify residual delivery/drip;
- Unlock and inspect tubing/pump condition; and
- verify UI, MQTT, serial, and physical behavior remain mutually truthful.

**Actual:** `[PENDING]`

**Evidence:** `[PENDING]`

**Result:** `[PENDING]`

## Test 9 - Loaded Trajectory Tuning

**Preconditions:** Tests 1-8 pass; the loaded mechanical/fluid configuration and
frozen-profile revision are unchanged; independent RPM measurement is ready.

**Required equipment:** Guarded loaded assembly, physical disconnect,
tachometer/revolution counter, current and temperature instruments, pressure
instrument, containment, and synchronized evidence capture.

**Safety gates:** Shawn confirms the exact loaded configuration, minimum-load
fluid path, limits, observer, and disconnect. No dead-head, closed discharge, or
unapproved fluid.

**Operator actions:** Observe each commanded start, ramp, steady point, stop, and
permitted reversal. Report actual shaft behavior before the next command.

**Commands/firmware actions:** Through one production interface, apply only the
approved profile points and ramps. Change one variable at a time. Stop and unlock
between configurations.

**Required measurements:** Commanded/generated RPM, external RPM versus time,
rise/settle/stop behavior, pressure, current, component temperatures, sound,
vibration, stall/step-loss evidence, and tube behavior.

**Evidence:** Raw instrument data, serial/MQTT/browser logs, profile revision,
configuration snapshot, video/photographs where useful, and calculation output.

**Pass:** Every approved point remains within the frozen deviation and safety
criteria with smooth, truthful behavior and no immediate-stop condition.

**Fail:** Any frozen criterion is exceeded or behavior is unsafe/dishonest.

**Immediate-stop:** Apply Section 03, including unexpected motion, wrong
direction, no-stop, stall, leak, pressure/current/temperature excursion, abnormal
sound, panic, or reset.

**Recovery/reset:** Physically isolate, relieve pressure, preserve configuration
and evidence, identify root cause, and do not use Recover merely to continue.

**Retest:** Approve a new profile revision for any ramp/limit change, then repeat
the affected point and all dependent points. Result: `[PENDING]`

## Test 10 - Calibration

After Test 9 loaded trajectory behavior is accepted, execute the frozen matrix
from `04_CALIBRATION_AND_PERFORMANCE_PLAN.md`.

Require:

- all repetitions and raw data preserved;
- independent shaft and volume/flow measurement;
- pressure and temperature recorded at every point;
- no unapproved outlier removal;
- all approved accuracy/repeatability/drift criteria met; and
- release statements limited to the tested configuration and envelope.

**Actual:** `[PENDING]`

**Evidence:** `[PENDING]`

**Result:** `[PENDING]`

## Test 11 - Pressure and Load Characterization

After Test 10 establishes the accepted baseline characterization, proceed at
approved incremental pressure points:

- stabilize at the approved speed;
- verify pressure remains below limit and relief remains functional;
- record flow, shaft behavior, current, temperature, sound, vibration, and
  controller state;
- verify no tube/fitting movement, leak, stall, or driver lock;
- perform normal Stop at each required load point;
- perform the separately approved software E-stop point; and
- inspect for residual pressure and safe restart behavior.

No dead-head test is performed unless specifically engineered, relieved,
reviewed, and approved.

**Actual:** `[PENDING]`

**Evidence:** `[PENDING]`

**Result:** `[PENDING]`

## Test 12 - Browser Control

Through the authenticated SoftAP and production browser controls:

- verify status freshness and authority truthfulness;
- enter Local Service through the supported path;
- set target, Start, Stop, Unlock, E-stop, reset E-stop, and Recover only at
  preapproved safe points;
- verify each accepted command maps to exactly one production transition;
- verify every rejected command maps to zero motion transition;
- verify logout/session loss does not silently mutate motion; and
- exit service cleanly with no stale command after reboot.

**Actual:** `[PENDING]`

**Evidence:** `[PENDING]`

**Result:** `[PENDING]`

## Test 13 - MQTT Control

Using an enrolled, least-privilege machine on the approved interface:

- prove CONNECT authentication and exact topic policy;
- establish the current heartbeat lease;
- issue current-session QoS 1 non-retained commands with newer sequences;
- correlate each application `command_result`;
- verify exact duplicate result replay with no second dispatch;
- verify stale, conflicting, retained, malformed, unauthorized, wrong-interface,
  and wrong-session traffic causes zero motion;
- perform safe Start, normal Stop, software E-stop, reset, and Unlock;
- verify authoritative retained state after reconnect; and
- revoke the temporary machine and prove immediate invalidation.

Do not store the one-time credential in evidence or shell history.

**Actual:** `[PENDING]`

**Evidence:** `[PENDING]`

**Result:** `[PENDING]`

## Test 14 - Supervisory Loss

Begin only after Tests 12 and 13 have exercised browser and MQTT production
control successfully.

While running at the specifically approved safe speed/load:

- record stable state and physical delivery;
- stop heartbeat and disconnect the supervisory client;
- require link observability to become `STALE`/`OFFLINE`;
- require no automatic Stop, target change, driver change, or synthetic command;
- verify the physical process remains within the separately approved safe
  fail-operational envelope;
- reconnect and require truthful state republication without synthetic Start; and
- issue a fresh explicit Stop and Unlock.

If continued pumping during communications loss is not safe for the intended
process, Phase 5 must not waive the contradiction. The deployment needs an
independent interlock or a separately reviewed architecture change.

**Actual:** `[PENDING]`

**Evidence:** `[PENDING]`

**Result:** `[PENDING]`

## Test 15 - Thermal and Endurance Soak

**Preconditions:** Tests 9-14 pass where applicable; duration, cadence, load,
limits, tube-aging endpoint, and supervision plan are frozen.

**Required equipment:** Loaded fixture, physical disconnect, pressure/current/
temperature instruments, leak containment, time-series logger, and tube
inspection tools.

**Safety gates:** Initial soak is attended. Unattended operation is prohibited
unless a later risk assessment and procedure explicitly approve it.

**Operator actions:** Inspect the assembly at every approved interval and report
temperature, leak, sound, vibration, tube, fitting, and flow observations.

**Commands/firmware actions:** Run the approved production command sequence at
the frozen representative load; perform only approved cycle interruptions.

**Required measurements:** Runtime/cycles, external RPM, characterized delivery,
pressure, voltage/current, all named temperatures, generated telemetry, heap/
task/stack observations, tube runtime/cycle count, and post-run inspection.

**Evidence:** Complete time series, event log, instrument identities, tube
before/after images, profile revision, and raw controller logs.

**Pass:** Duration/cycles complete within every frozen thermal, electrical,
pressure, drift, reliability, and tube criterion with no leak or software fault.

**Fail:** Any limit, reliability criterion, or safety condition fails.

**Immediate-stop:** Section 03 plus rising/unbounded thermal trend, pressure or
current excursion, leak/tube walk, abnormal wear, stall, reset, task leak, or
memory corruption.

**Recovery/reset:** Stop if safe, physically isolate, relieve pressure, cool,
preserve state/data, and quarantine damaged tubing/components.

**Retest:** Root-cause review determines the full soak and dependent calibration
scope that must be repeated. Result: `[PENDING]`

## Test 16 - Start/Stop and Permitted Direction Cycling

**Preconditions:** Test 9 loaded trajectory and Test 15 endurance pass; cycle
count, directions, setpoints, ramps, dwell, and wear limits are frozen.

**Required equipment:** Guarded fixture, physical disconnect, independent RPM,
pressure/current/temperature capture, and cycle counter.

**Safety gates:** Only profile-approved directions; no automatic cycling without
a present safety observer and an independently reachable disconnect.

**Operator actions:** Observe the first cycle and every scheduled inspection;
confirm physical zero after Stop and no motion after Unlock.

**Commands/firmware actions:** Use production paths for approved Start, Stop,
Unlock, setpoint-only, permitted reversal through zero, software E-stop/reset,
and Recover cycles.

**Required measurements:** Completed/failed cycles, external RPM, start/stop
times, state transitions, direction, pressure/current/temperature, tube runtime,
wear, and any command/result mismatch.

**Evidence:** Command/result chronology, raw cycle log, synchronized physical
observations, profile revision, and pre/post inspection.

**Pass:** All required cycles complete with zero uncommanded motion, no failed
stop, no state/physical disagreement, and all wear/drift criteria met.

**Fail:** Any safety-critical transition fails or a frozen cycle/wear criterion
is missed.

**Immediate-stop:** Unexpected restart/direction, failure to stop, stall, leak,
abnormal wear, limit excursion, or controller fault.

**Recovery/reset:** Isolate and inspect before any reset; do not clear evidence
or automatically resume the cycle index.

**Retest:** Preserve failed count and repeat the acceptance campaign required by
the approved root-cause disposition. Result: `[PENDING]`

## Test 17 - Restart, Power-Loss, Storage, and Recovery

**Preconditions:** Tests 1-16 pass where applicable. Stationary restart tests
pass before any running power-loss test; persistence inventory, backup/restore
plan, cycle counts, and safe physical consequences are frozen.

**Required equipment:** Physical disconnect, serial/network capture, approved
backup/restore artifacts, loaded fixture and containment when running, and
independent motion observation.

**Safety gates:** Running power removal requires separate approval, safe loss of
delivery, no hazardous siphon/pressure consequence, and a human at the
disconnect.

**Operator actions:** Confirm physical state before removal and after restoration;
report any motion, pressure, leak, or hardware anomaly.

**Commands/firmware actions:** Execute controlled reboot, reset, cold power cycle,
approved running power loss, supported security recovery, and approved
configuration/storage recovery paths.

**Required measurements:** Reset reason, boot identity, machine/output/driver
state, persistence of identity/network/security/machine/calibration data,
authority/session regeneration, physical zero, pressure, and restoration result.

**Evidence:** Full boot logs, before/after storage/configuration hashes or
sanitized inventories, operator observations, and profile revision.

**Pass:** Every approved case boots stationary and truthful with no stale command,
automatic motion, corrupt storage, silent privilege change, or unexplained loss.

**Fail:** Any unsafe restart, replay, corruption, persistence mismatch, or
unrecoverable supported path.

**Immediate-stop:** Motion on boot, reset loop, brownout/watchdog, pressure/leak
hazard, storage corruption, or dishonest state.

**Recovery/reset:** Isolate power/process energy and restore only through the
preapproved reproducible plan.

**Retest:** Repeat the affected destructive sequence from a restored known
baseline plus every invalidated downstream test. Result: `[PENDING]`

## Test 18 - Security Regression and Deferred-Hardening Disposition

**Preconditions:** Test 17 recovery evidence is complete; candidate and interface
inventory frozen; temporary accounts and machines planned; DHR owners and
acceptance authority identified.

**Required equipment:** Isolated approved network, browser and MQTT clients,
serial capture, audit export tooling, and credential-safe evidence workspace.

**Safety gates:** Controller stationary, zero output, driver disabled, and motor
power physically isolated. No reusable secret enters evidence or shell history.

**Operator actions:** Perform only human-presence actions explicitly required by
authentication/recovery and confirm no physical motion.

**Commands/firmware actions:** Run browser and machine authentication,
authorization, interface, CSRF, throttle, enrollment, rotation, revocation,
disconnect, scope, audit, reset, and recovery regressions from Section 07.

**Required measurements:** HTTP/MQTT results, connection lifecycle, zero
dispatch/motion on rejection, audit prepared/terminal records, resource bounds,
and final temporary-identity cleanup.

**Evidence:** Sanitized logs, route/topic matrix, DHR disposition table,
independent audit report where applicable, and candidate/profile identity.

**Pass:** Accepted security contracts pass and every DHR has a truthful Gate B
disposition. `PRODUCTION` additionally requires DHR-009 closure.

**Fail:** Security regression, credential disclosure, unauthorized access,
unbounded resource behavior, missing audit result, or unsupported production
claim.

**Immediate-stop:** Any motion, credential exposure, panic/reset, loss of
administrative control, or unsafe network exposure.

**Recovery/reset:** Revoke temporary credentials, isolate interfaces, preserve
audit evidence, and restore the approved security/network state.

**Retest:** Correct under normal history, independently review, and repeat all
affected security and downstream release gates. Result: `[PENDING]`

## Test 19 - Retained MQTT Discovery and Configuration Behavior

**Preconditions:** Test 18 security disposition is recorded; exact Phase 4C/4D.4
topic contract and retained-topic inventory are reconciled from source;
controller stationary; least-privilege machine enrolled.

**Required equipment:** Approved MQTT client, packet/log capture, serial monitor,
topic-inventory tool, and credential-safe evidence storage.

**Safety gates:** Motor power physically isolated. Retained command, heartbeat,
command-result, secret, or credential publication is prohibited.

**Operator actions:** Observe controller state across broker/client reconnect and
controller reboot; confirm no physical motion or stale command effect.

**Commands/firmware actions:** Subscribe to the canonical root, inventory retained
metadata/state/status/open-loop telemetry, reconnect, restart while stationary,
and verify obsolete or unauthorized retained records are absent. Do not invent a
configuration topic or mutate accepted source merely to satisfy this procedure.

**Required measurements:** Exact topic, retain/QoS/payload, ownership, value,
republish timing, broker session change, last accepted sequence, duplicate/stale
record behavior, retained capacity, and no-motion/no-dispatch proof.

**Evidence:** Before/after topic inventories, packet/log capture, source-derived
expected inventory, profile/candidate identity, and sanitized discrepancy report.

**Pass:** Every source-authorized retained record is bounded, truthful,
controller-owned, and republished correctly; prohibited/obsolete records are
absent; reconnect/reboot causes no stale command or motion replay.

**Fail:** Missing, stale, contradictory, unauthorized, secret-bearing, evicted,
or replay-capable retained data, or any ownership/dispatch violation.

**Immediate-stop:** Any motion, command replay, credential disclosure, broker
instability, panic/reset loop, or retained-capacity corruption.

**Recovery/reset:** Disconnect the test client, isolate the controller, preserve
the broker inventory, and restore only the accepted configuration.

**Retest:** Reconcile contract/source first; repeat the complete retained
inventory and lifecycle sequence after any correction. Result: `[PENDING]`

## Test 20 - Final Pure Suite

After every applicable campaign in Tests 1-19 is complete, return to stationary
`UNLOCKED`, zero output, driver disabled, pressure relieved, and safe fluid
containment. Repeat the complete three-invocation pure-suite proof and compare
all production snapshots and task counts.

Require zero failures, contamination, panic/reset, watchdog, stack/heap failure,
or task leak.

**Actual:** `[PENDING]`

**Evidence:** `[PENDING]`

**Result:** `[PENDING]`

## Test 21 - Controlled Final State and Reboot

- Begin only after Test 20 passes.
- Stop normally and confirm physical zero.
- Unlock and confirm driver disabled.
- Relieve pressure and isolate process energy.
- Remove temporary machine clients and test accounts.
- Restore approved production network/security configuration.
- Reboot from the supported path.
- Confirm no stale command, no automatic motion, truthful retained state, and
  clean commissioned operation.
- Release serial and network test resources.

**Actual:** `[PENDING]`

**Evidence:** `[PENDING]`

**Result:** `[PENDING]`
