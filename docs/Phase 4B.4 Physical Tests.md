# Phase 4B.4 - Physical Test and Evidence Record

- **Software acceptance status:** SOFTWARE-AND-AUTOMATED-RUNTIME-ACCEPTED
- **Physical acceptance status:** PENDING
- **Firmware identity:** `v2-phase4b.4-dev`
- **Accepted implementation commit:** `1b701e5ffdf820a468070dc1f1a54d129a9537d0`
- **Accepted software-evidence commit:** `5dbaf316e94ee4743f14841f1bbd89aa3dc8ecaf`
- **Physical acceptance date:** `[PENDING]`

The accepted implementation commit is the reviewed Phase 4B.4 Step 2 software candidate. The later accepted software-evidence commit records the successful controller-only automated runtime and does not change that implementation. This procedure must be executed against that exact implementation candidate, or against a later explicitly reviewed candidate identified in the completed evidence.

## 1. Evidence Boundary

Phase 4B.4 software and automated runtime are accepted. Three genuine Windows ConPTY-backed diagnostic-option `t` invocations of the corrected candidate each completed 163 distinct tests, three internal repeat passes, 489 executions, 489 passed, and 0 failed, for 1,467/1,467 passing outcomes. Those runs used a disconnected motor and do not establish physical command, shaft, pump, or mechanical behavior.

This document is the operator-ready procedure for the remaining connected-motor and physical browser-command gate. Every required gate remains pending until its actual outcome and evidence are recorded here.

HTTP `200 OK` proves that a request was admitted and dispatched. It does **not** prove that the shaft moved, moved in the intended direction, reached the intended speed, or stopped correctly. Physical behavior must be observed independently.

Phase 4B.4 physical acceptance is limited to the hydraulically unloaded motor/pump assembly and the browser-command behaviors defined below. Pump, hose, chemical, pressure, flow-accuracy, process-load, and mechanical-endurance acceptance remain outside this gate.

For every gate, preserve the initial condition, operator action, relevant browser response, `/api/status` result, dashboard and serial transition, physical observation, actual outcome, PASS/FAIL result, and screenshot or log filename when it materially adds evidence.

## 2. Safety and Hard-Stop Rules

1. Test only a hydraulically unloaded motor/pump assembly. Do not install chemical, pressure, process load, or a charged hose path.
2. Guard the shaft and coupling. Keep hands, clothing, tools, leads, and loose material clear of all moving parts.
3. Keep the physical motor-power disconnect immediately reachable throughout testing. The software E-stop is not safety-rated and does not replace physical power isolation.
4. Use only the low speeds specified by this procedure. Do not increase the commanded speeds to make motion easier to see.
5. Issue one browser command at a time. Observe its HTTP response, controller state, and physical result before issuing the next command.
6. Stop immediately for unexpected motion, wrong direction, failure to stop, abnormal sound, buzz, stall, step loss, abrupt motion, driver fault, excessive heat, smoke, panic, reset, brownout, watchdog, assertion, stack-canary failure, heap corruption, task leak, or dishonest status.
7. On an unsafe condition, use the physical power disconnect first as circumstances require, preserve evidence, and stop the procedure.
8. Do not continue after a failed safety-critical test. Do not work around a failure, manufacture an authority state, inject a timing race, or add a test backdoor.

## 3. Required Equipment

- ESP32-S3 ArgusControl Pump Controller matching the accepted hardware configuration.
- Hydraulically unloaded motor/pump assembly with guarded shaft and coupling.
- Motor power supply with an immediately reachable physical disconnect.
- USB data connection and the normal ESP-IDF v5.5.3 serial monitor environment.
- Operator computer or phone connected to the controller Service AP.
- Browser with Developer Tools console access and an authenticated Service Portal session.
- Windows `curl.exe` for the unauthenticated authentication-gate test.
- A means to observe shaft direction without entering the guarded area.
- Evidence storage for serial logs, browser-console output, status captures, and screenshots.

## 4. Flash, Boot Identity, and Connected-Motor Preflight

**Initial condition:** Controller de-energized; motor power disconnected; hydraulically unloaded motor/pump assembly connected according to the accepted wiring and polarity doctrine; shaft/coupling guarded; USB connected; accepted implementation commit checked out and independently identified.

**Operator action:** Verify the exact firmware commit and firmware identity, perform the separately authorized normal flash/monitor workflow, observe boot with motor power initially disconnected, then energize the guarded and unloaded motor assembly while the machine is `UNLOCKED` and stationary.

**Expected result:**

- The tested firmware is identified as implementation commit `1b701e5ffdf820a468070dc1f1a54d129a9537d0` with firmware identity `v2-phase4b.4-dev`.
- Boot completes without panic, reset loop, watchdog, brownout, assertion, stack-canary failure, heap corruption, or lower-layer fault.
- Before and after motor power is applied, machine state is `UNLOCKED`, target/applied/generated speeds are zero, E-stop is clear, and the driver is disabled.
- Applying motor power causes no shaft movement, unexpected holding torque, abnormal sound, fault, heat, or smoke.
- The Service AP, dashboard, `/api/status`, and serial console are available and truthful.

**Actual outcome:** `[PENDING]`

**Evidence:** `[PENDING]`

**Result:** `[PENDING]`

## 5. Pure-Suite Preflight

**Initial condition:** Connected motor/pump assembly remains guarded, hydraulically unloaded, stationary, and safe. Controller has completed the accepted boot preflight. Record the production state before entering diagnostic option `t`.

**Operator action:** Select diagnostic option `t` once and allow the complete suite, including all three internal repeat passes, to finish without interacting with the controller or browser.

**Expected runtime result:**

| Runtime result | Expected value | Observed value |
|---|---:|---:|
| Distinct tests | 163 | `[PENDING]` |
| Internal repeat passes | 3 | `[PENDING]` |
| Total executions | 489 | `[PENDING]` |
| Passed | 489 | `[PENDING]` |
| Failed | 0 | `[PENDING]` |

**Production-isolation record:**

| Field | Before suite | After suite | Required result |
|---|---|---|---|
| Authority generation | `[PENDING]` | `[PENDING]` | Unchanged |
| Network mode | `[PENDING]` | `[PENDING]` | Unchanged |
| Broker state | `[PENDING]` | `[PENDING]` | Unchanged |
| Machine state | `[PENDING]` | `[PENDING]` | Unchanged |
| Task count | `[PENDING]` | `[PENDING]` | Unchanged |

The motor must not move during the suite. The suite must return normally with no panic, reset, watchdog, brownout, assertion, stack-canary failure, heap corruption, task leak, or production-state contamination.

**Actual outcome:** `[PENDING]`

**Evidence:** `[PENDING]`

**Result:** `[PENDING]`

## 6. Browser-Console Helper

Phase 4B.5 polished controls do not exist yet. Open the authenticated controller dashboard, open the browser Developer Tools console on that same origin, and define these helpers exactly:

```javascript
window.argusRaw = async function (body) {
  const response = await fetch("/api/command", {
    method: "POST",
    credentials: "same-origin",
    headers: { "Content-Type": "application/json" },
    body
  });
  const result = {
    status: response.status,
    body: await response.text(),
    cacheControl: response.headers.get("cache-control")
  };
  console.log(result);
  return result;
};

window.argusCommand = async function (payload) {
  return window.argusRaw(JSON.stringify(payload));
};

window.argusStatus = async function () {
  const response = await fetch("/api/status", {
    credentials: "same-origin",
    cache: "no-store"
  });
  const result = await response.json();
  console.log(result);
  return result;
};
```

Issue commands one at a time. Wait for the promise result, call `argusStatus()`, and observe the dashboard, serial output, and physical assembly before continuing.

The seven exact accepted command forms are:

```javascript
argusCommand({command: "set_target", target_rpm_milli: 8000, forward: true})
argusCommand({command: "start"})
argusCommand({command: "stop"})
argusCommand({command: "unlock"})
argusCommand({command: "estop"})
argusCommand({command: "reset_estop"})
argusCommand({command: "recover"})
```

Do not add browser-supplied source, authority generation, pump, channel, axis, or routing fields.

## 7. Detailed Physical Tests

### Test 1 - Authentication Gate

**Initial condition:** Controller reachable through the Service AP; machine stationary; record `/api/status`, dashboard state, and physical state. Do not enter Local Service yet.

**Operator action:** From Windows, send a real unauthenticated POST without `-u`, an `Authorization` header, cookies, or other credentials:

```powershell
curl.exe -i -X POST -H "Content-Type: application/json" --data '{"command":"start"}' http://192.168.4.1/api/command
```

**Expected result:** Response is HTTP 401 with bounded `{"error":"unauthorized"}` JSON, a `WWW-Authenticate` header, and `Cache-Control: no-store`. Machine state, target, applied/generated speeds, driver state, E-stop state, authority, and generation remain unchanged. There is zero motion and zero command dispatch.

**Actual outcome:** `[PENDING]`

**Evidence:** `[PENDING]`

**Result:** `[PENDING]`

### Test 2 - Rejection Outside Service Mode

**Initial condition:** Network mode is not `SERVICE_AP_ONLY`; authenticated browser session is active; machine is stationary. Capture target, machine state, driver state, applied/generated speeds, authority, generation, and physical state.

**Operator action:** In the authenticated browser console, define the helpers and send:

```javascript
argusCommand({command: "set_target", target_rpm_milli: 8000, forward: true})
```

**Expected result:** Response is HTTP 403 with `{"ok":false,"error":"command_not_admitted"}` and `Cache-Control: no-store`. Target, machine state, driver state, applied/generated speeds, authority, and generation remain unchanged. The motor does not move and no command reaches the state manager.

**Actual outcome:** `[PENDING]`

**Evidence:** `[PENDING]`

**Result:** `[PENDING]`

### Test 3 - Browser-Owned Local Service Entry

**Initial condition:** Controller is stationary and eligible for service entry; E-stop is clear; browser is authenticated; capture network, STA, broker, authority, generation, and machine state.

**Operator action:** From the authenticated browser console, request service entry:

```javascript
fetch("/api/service/enter", {
  method: "POST",
  credentials: "same-origin"
}).then(async response => console.log({status: response.status, body: await response.text()}))
```

Reconnect to the Service AP and reopen the authenticated dashboard if the transition interrupts the browser connection. Poll with `argusStatus()` until the transition converges.

**Expected result:** The request returns 202 Accepted, or idempotent 200 only if the controller was already in the exact required state. Final network mode is `SERVICE_AP_ONLY`; authority is `LOCAL_SERVICE/BROWSER`; STA and broker are stopped according to accepted service-entry behavior; the Service AP and HTTP portal are available; machine state remains safe and stationary; and the motor does not move.

**Actual outcome:** `[PENDING]`

**Evidence:** `[PENDING]`

**Result:** `[PENDING]`

### Test 4 - Forward Setpoint Without Start

**Initial condition:** `SERVICE_AP_ONLY`; `LOCAL_SERVICE/BROWSER`; machine `UNLOCKED`; driver disabled; target/applied/generated speeds zero; motor stationary.

**Operator action:** Send:

```javascript
argusCommand({command: "set_target", target_rpm_milli: 8000, forward: true})
```

Then call `argusStatus()` and observe the assembly.

**Expected result:** Response is HTTP 200 with `{"ok":true,"status":"accepted"}` and `Cache-Control: no-store`. Target becomes 8000 milli-RPM (8.0 RPM), direction is forward, machine remains `UNLOCKED`, applied/generated speeds remain zero, driver remains disabled, and the motor does not move.

**Actual outcome:** `[PENDING]`

**Evidence:** `[PENDING]`

**Result:** `[PENDING]`

### Test 5 - Forward Start

**Initial condition:** Accepted 8000 milli-RPM forward target; machine `UNLOCKED`; driver disabled; applied/generated speeds zero; motor stationary.

**Operator action:** Send `argusCommand({command: "start"})`. Observe repeated `argusStatus()` results, dashboard and serial transitions, driver state, and shaft motion without entering the guarded area.

**Expected result:** Response is HTTP 200. Machine transitions `STARTING` to `RUNNING`; driver enables; applied/generated speed rises smoothly to 8000 milli-RPM. The shaft accelerates smoothly without buzz, stall, step loss, or abrupt jump. Record the observed shaft direction as **forward** for comparison in Test 7.

**Actual outcome:** `[PENDING]`

**Evidence:** `[PENDING]`

**Result:** `[PENDING]`

### Test 6 - Normal Stop and Unlock

**Initial condition:** Machine `RUNNING` at the accepted 8000 milli-RPM forward target with stable physical motion.

**Operator action:** Send `argusCommand({command: "stop"})`, observe through completion, then send `argusCommand({command: "unlock"})` only after `HOLDING` and zero speed are confirmed.

**Expected result:** Stop returns HTTP 200 and produces `DECELERATING` to `HOLDING`, with a smooth return of applied/generated speeds and shaft motion to zero. Driver remains enabled in `HOLDING` with accepted active-low hold behavior. Unlock returns HTTP 200, changes state to `UNLOCKED`, disables the driver, and causes no restart or additional shaft movement.

**Actual outcome:** `[PENDING]`

**Evidence:** `[PENDING]`

**Result:** `[PENDING]`

### Test 7 - Reverse-Direction Proof

**Initial condition:** Machine `UNLOCKED`; driver disabled; applied/generated speeds zero; motor stationary. Forward direction from Test 5 is recorded.

**Operator action:** Send `argusCommand({command: "set_target", target_rpm_milli: 4000, forward: false})`; prove the setpoint alone causes no motion; send `argusCommand({command: "start"})`; observe direction and smooth motion; then stop normally and unlock as in Test 6.

**Expected result:** Setpoint and start each return HTTP 200. The setpoint alone leaves the driver disabled and motor stationary. Start produces smooth motion at 4000 milli-RPM in the direction opposite the direction recorded in Test 5. Normal stop reaches `HOLDING` at zero speed, and unlock returns to `UNLOCKED` with the driver disabled and no restart.

**Actual outcome:** `[PENDING]`

**Evidence:** `[PENDING]`

**Result:** `[PENDING]`

### Test 8 - E-Stop Priority and Latched Rejection

**Initial condition:** Re-establish the 8000 milli-RPM forward target and `RUNNING` state. Confirm stable motion, E-stop clear, and driver enabled.

**Operator action:** Send `argusCommand({command: "estop"})`. After `EMERGENCY_STOPPED` is confirmed, separately send `argusCommand({command: "start"})` and `argusCommand({command: "set_target", target_rpm_milli: 8000, forward: true})`, observing each response and state before continuing.

**Expected result:** E-stop returns HTTP 200 and causes immediate pulse halt, `EMERGENCY_STOPPED`, `estop_latched: true`, and zero generated/applied speed. Because E-stop began while the driver was enabled, accepted active-low hold behavior is retained: ENA remains active LOW for holding torque while STEP is inactive HIGH and no pulses are generated. The shaft stops without continued commanded motion. While latched, both `start` and `set_target` return HTTP 409 with `{"ok":false,"error":"command_conflict"}` and `Cache-Control: no-store`; neither causes motion, dispatch-side state corruption, or latch loss.

**Actual outcome:** `[PENDING]`

**Evidence:** `[PENDING]`

**Result:** `[PENDING]`

### Test 9 - Reset Eligibility

**Initial condition:** Machine `EMERGENCY_STOPPED` following the running E-stop in Test 8; E-stop latched; applied/generated speeds zero; driver retaining accepted holding behavior; motor stationary.

**Operator action:** Send `argusCommand({command: "reset_estop"})`. After the result is fully observed, send the same command again while no longer E-stopped.

**Expected result:** First reset returns HTTP 200, clears the latch, returns to `HOLDING`, preserves zero applied/generated speed, and does not restart motion. The second reset returns HTTP 409 with `{"ok":false,"error":"command_conflict"}`; machine state, driver state, target, speeds, authority, and physical state remain unchanged.

**Actual outcome:** `[PENDING]`

**Evidence:** `[PENDING]`

**Result:** `[PENDING]`

### Test 10 - Recovery

**Initial condition:** Stationary, E-stop-clear `HOLDING` state after Test 9; applied/generated speeds zero; no active lower-layer fault.

**Operator action:** Send `argusCommand({command: "recover"})` and observe the response, state transition, driver state, lower-layer fault indication, and physical assembly.

**Expected result:** Response is HTTP 200. A brief `RECOVERING` transition may occur. Final state is `UNLOCKED`, driver disabled, applied/generated speeds zero, no lower-layer fault, and no shaft movement.

**Actual outcome:** `[PENDING]`

**Evidence:** `[PENDING]`

**Result:** `[PENDING]`

### Test 11 - Strict Live JSON Rejection

**Initial condition:** `SERVICE_AP_ONLY`; `LOCAL_SERVICE/BROWSER`; machine `UNLOCKED`; E-stop clear; driver disabled; applied/generated speeds zero; motor stationary. Capture the complete status baseline.

**Operator action:** Send each request separately. Observe its response, call `argusStatus()`, and confirm physical state before sending the next request:

```javascript
argusRaw('{"command":')
argusRaw('{"command":"start","command":"stop"}')
argusRaw('{"command":"jog"}')
argusRaw('{"command":"start","extra":1}')
argusRaw('{"command":"set_target","target_rpm_milli":200001,"forward":true}')
argusRaw('{"command":"set_target","target_rpm_milli":8000,"forward":"true"}')
argusRaw('{"command":"start"} trailing')
```

**Expected result:** Every request returns HTTP 400, exact `{"ok":false,"error":"invalid_request"}` JSON, and `Cache-Control: no-store`. Across all seven requests there is zero state mutation, zero motor motion, no partial or default dispatch, and no credential or raw-body logging.

**Actual outcome:** `[PENDING]`

**Evidence:** `[PENDING]`

**Result:** `[PENDING]`

### Test 12 - Truthfulness and Automation-Only Authority Proofs

**Initial condition:** Complete evidence from Tests 1-11 is available; controller remains `SERVICE_AP_ONLY`, `LOCAL_SERVICE/BROWSER`, `UNLOCKED`, and stationary; broker remains stopped.

**Operator action:** Compare every browser response, `/api/status` result, dashboard transition, serial event, and physical observation recorded throughout the sequence. Review the accepted 163-test automation evidence for authority and dispatch cases that the browser cannot safely construct.

**Expected result:**

- An accepted HTTP response is recorded only as admission and dispatch evidence, never as standalone proof of shaft motion.
- Network remains `SERVICE_AP_ONLY`; authority remains `LOCAL_SERVICE/BROWSER`; broker remains stopped.
- There are no unexplained authority-generation changes.
- Each accepted command corresponds to one production transition; every rejection corresponds to zero motion transitions.
- Browser, status API, dashboard, serial output, and physical behavior remain mutually truthful.

Do **not** instruct the operator to manufacture `SERVICE_AP_ONLY` with an intentionally wrong authority owner or to supply a stale authority generation. Supported service entry intentionally establishes `SERVICE_AP_ONLY` and `LOCAL_SERVICE/BROWSER` together. The browser is intentionally prohibited from supplying source or generation. Do not add a test backdoor or timing-race procedure.

The accepted 163-test suite is the automation-only proof for:

- Every wrong authority-mode/owner combination.
- Server-owned `ARGUS_CMD_SRC_LOCAL_SERVICE_PORTAL` source.
- Server-captured authority generation.
- Stale-generation router rejection.
- Exactly one dispatch for an accepted request.
- Zero dispatch on rejection.
- E-stop priority.
- Production isolation.

**Actual outcome:** `[PENDING]`

**Evidence:** `[PENDING]`

**Result:** `[PENDING]`

## 8. Final Pure-Suite and Isolation Proof

**Initial condition:** Physical Tests 1-12 completed without a safety-critical failure; controller remains stationary in the safe state produced by Test 10 and preserved through Tests 11-12. Record the current production baseline.

**Operator action:** Select diagnostic option `t` once and allow the complete suite, including all three internal repeat passes, to finish without browser or controller interaction.

**Expected runtime result:**

| Runtime result | Expected value | Observed value |
|---|---:|---:|
| Distinct tests | 163 | `[PENDING]` |
| Internal repeat passes | 3 | `[PENDING]` |
| Total executions | 489 | `[PENDING]` |
| Passed | 489 | `[PENDING]` |
| Failed | 0 | `[PENDING]` |

**Production-isolation record:**

| Field | Before suite | After suite | Required result |
|---|---|---|---|
| Authority generation | `[PENDING]` | `[PENDING]` | Unchanged |
| Network mode | `[PENDING]` | `[PENDING]` | Unchanged |
| Broker state | `[PENDING]` | `[PENDING]` | Unchanged |
| Machine state | `[PENDING]` | `[PENDING]` | Unchanged |
| Task count | `[PENDING]` | `[PENDING]` | Unchanged |

The motor must not move. The suite must return normally with no panic, reset, watchdog, brownout, assertion, stack-canary failure, heap corruption, task leak, or production-state contamination. A preflight and final baseline may differ because of the physical test sequence; each suite must preserve the state that existed when that suite began.

**Actual outcome:** `[PENDING]`

**Evidence:** `[PENDING]`

**Result:** `[PENDING]`

## 9. Controlled Service Exit and Reboot

**Initial condition:** Final pure suite passed; machine is stationary and safe; network is `SERVICE_AP_ONLY`; authority is `LOCAL_SERVICE/BROWSER`; broker is stopped; browser is authenticated.

**Operator action:** Send the authenticated service-exit request:

```javascript
fetch("/api/service/exit", {
  method: "POST",
  credentials: "same-origin"
}).then(async response => console.log({status: response.status, body: await response.text()}))
```

Allow the controlled reboot to complete. Reconnect to the Service AP and verify status, dashboard, and serial output without issuing another motion command.

**Expected result:** Request returns 202 Accepted; browser authority is revoked; controller performs one clean reboot; commissioned or discoverable operation is restored according to configuration; no motor movement occurs; no stale service command executes; and there is no panic, watchdog, brownout, assertion, stack/heap failure, or reset loop.

**Actual outcome:** `[PENDING]`

**Evidence:** `[PENDING]`

**Result:** `[PENDING]`

## 10. Final Acceptance Table

| Acceptance gate | Result | Evidence |
|---|---|---|
| Flash/boot identity and connected-motor preflight | `[PENDING]` | `[PENDING]` |
| Pure-suite preflight, 489/489 | `[PENDING]` | `[PENDING]` |
| Test 1 - Authentication gate | `[PENDING]` | `[PENDING]` |
| Test 2 - Rejection outside service mode | `[PENDING]` | `[PENDING]` |
| Test 3 - Browser-owned Local Service entry | `[PENDING]` | `[PENDING]` |
| Test 4 - Forward setpoint without start | `[PENDING]` | `[PENDING]` |
| Test 5 - Forward start | `[PENDING]` | `[PENDING]` |
| Test 6 - Normal stop and unlock | `[PENDING]` | `[PENDING]` |
| Test 7 - Reverse-direction proof | `[PENDING]` | `[PENDING]` |
| Test 8 - E-stop priority and latched rejection | `[PENDING]` | `[PENDING]` |
| Test 9 - Reset eligibility | `[PENDING]` | `[PENDING]` |
| Test 10 - Recovery | `[PENDING]` | `[PENDING]` |
| Test 11 - Strict live JSON rejection | `[PENDING]` | `[PENDING]` |
| Test 12 - Truthfulness and automation-only authority proofs | `[PENDING]` | `[PENDING]` |
| Final pure suite and isolation proof, 489/489 | `[PENDING]` | `[PENDING]` |
| Controlled service exit and reboot | `[PENDING]` | `[PENDING]` |
| Phase 4B.4 physical acceptance | **`[PENDING]`** | `[PENDING]` |

Phase 4B.4 may be marked **PHYSICALLY ACCEPTED** only after every required test passes against the accepted implementation candidate and the evidence identifies the exact tested firmware commit.

Physical acceptance must not imply pump, hose, chemical, pressure, flow-accuracy, process-load, or mechanical-endurance acceptance.
