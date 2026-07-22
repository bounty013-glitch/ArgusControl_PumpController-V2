# Phase 4B.4 - Acceptance Disposition and Deferred Integration Record

- **Phase 4B.4 disposition:** ACCEPTED
- **Accepted state:** SOFTWARE-AND-AUTOMATED-RUNTIME-ACCEPTED
- **Firmware identity:** `v2-phase4b.4-dev`
- **Accepted implementation commit:** `1b701e5ffdf820a468070dc1f1a54d129a9537d0`
- **Accepted evidence/documentation commit:** `5dbaf316e94ee4743f14841f1bbd89aa3dc8ecaf`
- **Previously accepted motor-control behavior:** REMAINS ACCEPTED - NOT REOPENED
- **Phase 4B.5 development:** AUTHORIZED TO PROCEED

## 1. Document Purpose

This document records the final Phase 4B.4 acceptance boundary and the decision not to repeat the complete physical motor-control campaign already performed in earlier phases. It supersedes the temporary Phase 4B.4 connected-motor procedure introduced by documentation commit `f6794d585a3183dd73bc3a34d44b20b952b98e60`. That procedure was never executed and created no physical evidence.

The remaining powered question introduced by the browser command ingress will be answered once, through the final Phase 4B.5 browser controls. This is a placement decision, not a failed test, waiver, or claim that the deferred confirmation has already occurred.

## 2. Accepted Phase 4B.4 Scope

Phase 4B.4 Step 1 and Step 2 are accepted for:

- The strict browser motion-command JSON contract.
- Authenticated `POST /api/command` registration and bounded request reception.
- Live `SERVICE_AP_ONLY` and `LOCAL_SERVICE/BROWSER` admission.
- Server-owned command source and authority generation.
- Construction of the accepted command envelope.
- Exclusive dispatch through `argus_cmd_router_dispatch()`.
- Bounded and truthful HTTP responses.
- Connection closure after oversized, truncated, timed-out, or unrecoverable receives leave framing uncertain.
- Preservation of the existing authority, router, state-manager, trajectory, pulse-engine, driver-control, and E-stop architecture.

Phase 4B.4 did not add a second motion path and did not redesign the accepted state or motion implementation.

## 3. Accepted Implementation and Evidence Commits

| Record | Commit | Meaning |
|---|---|---|
| Accepted implementation | `1b701e5ffdf820a468070dc1f1a54d129a9537d0` | Final reviewed Phase 4B.4 Step 2 implementation, including receive-failure session closure |
| Accepted evidence/documentation | `5dbaf316e94ee4743f14841f1bbd89aa3dc8ecaf` | Final accepted build, controller-runtime, boundary-audit, and review evidence |
| Firmware identity | `v2-phase4b.4-dev` | Identity of the accepted candidate |

The evidence commit follows the implementation commit and records evidence; it does not replace or alter the accepted implementation identity.

## 4. Previously Accepted Physical Motor-Control Boundary

Physical motor-control behavior was previously live-tested and accepted at the lower-layer state, trajectory, pulse-engine, driver-control, and E-stop boundaries. Phase 4B.4 does not reopen that acceptance. The remaining powered test is limited to end-to-end confirmation that commands entering through the final authenticated browser UI invoke the previously accepted behavior without bypass, inversion, unintended motion, or loss of safety semantics.

The preserved lower-layer acceptance includes:

- Pulse generation and low- and high-speed motor operation.
- Speed accuracy and forward/reverse direction behavior.
- Smooth trajectory ramp-up and ramp-down.
- Normal stop and stationary holding behavior.
- E-stop pulse cessation and accepted driver-hold behavior.
- Driver enable and disable behavior.
- Recovery without unintended motion.
- Setpoint separation from `START`.
- Motor remaining stationary until explicitly commanded to start.

Those results remain part of the historical acceptance record. They are neither erased nor represented as newly performed during Phase 4B.4.

## 5. New Behavior Introduced by Phase 4B.4

Phase 4B.4 added this ingress chain:

```text
Browser/API request
  -> existing portal authentication
  -> bounded HTTP body reception
  -> strict JSON decoder
  -> live network and authority admission
  -> server-owned command envelope
  -> command router
  -> previously accepted state and motion system
```

The new physical integration question is therefore limited to whether a command issued through the final browser UI reaches the accepted state/motion path and produces the corresponding physical result without bypass, inversion, unintended motion, or loss of safety semantics.

## 6. Evidence Already Accepted

The accepted Phase 4B.4 evidence establishes:

- ESP-IDF v5.5.3 full-clean, no-ccache build.
- Zero compiler warnings and zero compiler errors.
- Application binary size `0xfe590` bytes.
- Total image size 1,041,697 bytes.
- OTA headroom `0x201a70` bytes (67%).
- 163 distinct registered tests.
- Three internal repeat passes and 489/489 executions per invocation.
- Three genuine Windows ConPTY-backed controller invocations.
- 1,467/1,467 aggregate passing executions.
- No panic, reset, watchdog, brownout, assertion, stack-canary failure, heap corruption, task leak, or production-state contamination.
- Exactly one live endpoint call to `argus_cmd_router_dispatch()`.
- Zero direct endpoint calls to state-manager, trajectory, pulse-engine, motor, or GPIO APIs.
- Motor disconnected throughout the accepted automated-runtime work.
- COM5 released after execution.

The accepted automated suite also proves cases that must not be physically manufactured:

- Every wrong authority-mode/owner combination.
- Browser prohibition from supplying command source or authority generation.
- Server-owned `ARGUS_CMD_SRC_LOCAL_SERVICE_PORTAL` source.
- Server-captured authority generation.
- Stale-generation rejection.
- Exactly one dispatch for accepted requests and zero dispatch for rejected requests.
- Strict malformed JSON rejection.
- Oversized, truncated, timeout, and receive-error handling.
- Connection closure when HTTP framing is uncertain.
- Production isolation and absence of a direct motion-path bypass.
- E-stop routing priority.

No test backdoor or timing-race procedure is authorized to reproduce these proofs physically.

## 7. Why Physical Repetition Is Unnecessary

Repeating the complete motor campaign in Phase 4B.4 would requalify unchanged lower layers using temporary Developer Tools helpers, then repeat the same ingress-to-motion confirmation through the real Phase 4B.5 controls. That duplication adds no proportionate acceptance value.

The lower-layer implementation and its accepted semantics were not modified by Phase 4B.4. The architectural boundary and controller execution are already proven by the accepted source audits and automated suite. The only new powered evidence needed is an end-to-end observation through the actual operator interface that will ship.

Accordingly:

- A temporary Phase 4B.4 connected-motor campaign is not required.
- Developer Tools helpers and disposable command controls are not required.
- Phase 4B.5 development may begin against the accepted Phase 4B.4 API.
- The powered UI-to-motor confirmation is a Phase 4B.5 final-acceptance dependency.

## 8. Deferred Phase 4B.5 Integration Confirmation

The deferred confirmation must use the actual Phase 4B.5 browser controls. It must not use Developer Tools helpers, a temporary test UI, or a second direct API-to-motor campaign.

The motor/pump assembly must be connected, guarded, hydraulically unloaded, and operated only at safe low targets. The check confirms the new UI/API ingress boundary; it does not repeat full lower-layer qualification.

## 9. Phase 4B.5 Minimum Acceptance Checks

1. **Forward setpoint without motion:** Use the real UI to set a safe low forward target. Require admitted command and truthful displayed target, with the motor stationary until `Start`.
2. **Forward start:** Use the real UI to issue `Start`. Require entry through `POST /api/command`, `STARTING` to `RUNNING`, expected forward direction, recognizably preserved trajectory behavior, and no abrupt jump, abnormal buzz, stall, or unintended motion.
3. **Normal stop:** Use the real UI to issue normal `Stop`. Require the previously accepted controlled stop and truthful stationary state and speed.
4. **Brief reverse confirmation:** At a safe low reverse target, require no movement from setpoint alone, motion opposite the recorded forward direction after `Start`, and a safe return to stationary after `Stop`.
5. **E-stop from low-speed motion:** Start at a safe low speed and issue E-stop through the real UI. Require E-stop priority, accepted pulse cessation, truthful `EMERGENCY_STOPPED` and `estop_latched`, and preserved accepted driver-hold behavior.
6. **Latched rejection:** Attempt `Start` while E-stop remains latched. Require rejection, no motor motion, and no state corruption.
7. **Reset and recovery:** Reset/recover through the real UI. Require the latch to clear only through the accepted transition, no automatic restart, and a stationary truthful final state.
8. **Final stability:** Require the UI, API response, controller state, and physical behavior to agree, with no panic, reset, watchdog, brownout, assertion, stack/heap failure, driver fault, or unexplained authority change.

These are end-to-end integration confirmations only. Do not repeat the full speed range, pulse-timing measurement, ramp mathematics qualification, endurance testing, stop-distance characterization, driver-polarity investigation, or lower-layer E-stop fault injection. Do not run a second API-to-motor campaign after the real UI campaign.

## 10. Safety and Hard-Stop Requirements for the Later Powered Test

- Keep the motor/pump assembly hydraulically unloaded, with no chemical or pressure.
- Guard all moving components.
- Keep the physical power disconnect immediately reachable.
- Use safe low-speed targets only.
- Stop immediately for unexpected motion, wrong direction, failure to stop, abnormal sound, driver fault, excess heat, smoke, panic, reset, brownout, watchdog, stack/heap failure, or dishonest status.
- Do not continue after a safety-critical failure.
- Treat the software E-stop as non-safety-rated; it does not replace physical power isolation.

## 11. Explicit Exclusions

This disposition does not claim or require:

- Pump or hose acceptance.
- Chemical or pressure testing.
- Flow-accuracy qualification.
- Process operation.
- Mechanical-endurance acceptance.
- Requalification of previously accepted motor-control lower layers.
- Completion of the deferred Phase 4B.5 powered confirmation.

## 12. Final Phase 4B.4 Disposition

| Disposition item | Result |
|---|---|
| Phase 4B.4 | **ACCEPTED** |
| Accepted state | **SOFTWARE-AND-AUTOMATED-RUNTIME-ACCEPTED** |
| Previously accepted motor-control behavior | **REMAINS ACCEPTED - NOT REOPENED** |
| Temporary Phase 4B.4 connected-motor campaign | **NOT REQUIRED** |
| Phase 4B.5 development | **AUTHORIZED TO PROCEED** |
| Remaining integration gate | **One narrow powered confirmation using the final Phase 4B.5 browser controls** |

The remaining confirmation is a Phase 4B.5 acceptance dependency. It does not leave the Phase 4B.4 implementation incomplete.

No pump, hose, chemical, pressure, flow-accuracy, process, or mechanical-endurance acceptance is implied.
