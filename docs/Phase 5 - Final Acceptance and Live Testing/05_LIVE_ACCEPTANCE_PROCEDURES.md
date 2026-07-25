# Live Acceptance Procedures

**Status:** NOT STARTED

Every test has:

- initial condition;
- operator action;
- expected result frozen before execution;
- actual result;
- evidence references; and
- `PASS`, `FAIL`, `BLOCKED`, or `NOT APPLICABLE` with justification.

Commands are issued one at a time. HTTP/MQTT success proves admission and
dispatch only; the operator separately records physical behavior.

## Test 1 - Baseline and Artifact Identity

**Initial condition:** Clean repository and accepted Phase 4D.4 baseline.

**Action:**

- verify local/remote branch and exact starting tag;
- verify Phase 5 candidate history descends from the baseline;
- verify firmware identity, ELF, MAP, BIN, partition table, build metadata, and
  SHA-256 hashes;
- verify no credentials or temporary artifacts are included; and
- verify the configured release profile matches the physical assembly.

**Expected:** One coherent candidate and no unexplained changes.

**Actual:** `[PENDING]`

**Evidence:** `[PENDING]`

**Result:** `[PENDING]`

## Test 2 - Source Review and Production Boundaries

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

## Test 3 - Full-Clean Release Build

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

## Test 4 - Flash, Boot, and Stationary Identity

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

## Test 5 - Pure-Suite Preflight and Isolation

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

With motion mechanically safe and the powered gate approved:

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

## Test 7 - Unloaded Mechanical Motion

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

## Test 8 - Pump Installation, Prime, and Zero-Pressure Delivery

After setup inspection and a new bounded powered confirmation:

- install the approved pump head and tubing;
- prime with the approved safe fluid;
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

## Test 9 - Calibration and Characterization Matrix

Execute the frozen matrix from `04_CALIBRATION_AND_PERFORMANCE_PLAN.md`.

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

## Test 10 - Pressure and Load Envelope

At approved incremental pressure points:

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

## Test 11 - Browser End-to-End Control

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

## Test 12 - Authenticated MQTT End-to-End Control

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

## Test 13 - Fail-Operational Supervisory Loss

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

## Test 14 - Final Pure Suite and Production Isolation

After all physical tests, return to stationary `UNLOCKED`, zero output, driver
disabled, pressure relieved, and safe fluid containment. Repeat the complete
three-invocation pure-suite proof and compare all production snapshots and task
counts.

Require zero failures, contamination, panic/reset, watchdog, stack/heap failure,
or task leak.

**Actual:** `[PENDING]`

**Evidence:** `[PENDING]`

**Result:** `[PENDING]`

## Test 15 - Controlled Final State and Reboot

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
