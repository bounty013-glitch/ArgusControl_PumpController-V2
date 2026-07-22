# Phase 4B.5 Tests and Acceptance Record

**Current disposition:** Motor-disconnected implementation VALIDATED; motor-disconnected controller and browser testing PASSED; final powered acceptance PENDING.

## 1. Purpose and Acceptance Boundary

This is the living software, automated-runtime, isolated-controller, browser, and later powered-integration record for Phase 4B.5. It records both successful and failed observations. An HTTP acceptance response proves command admission and dispatch only; authoritative controller status reports controller state, and neither proves physical shaft motion.

This record does not grant final Phase 4B.5 acceptance. The remaining gate is the bounded powered UI-to-motor procedure in Section 18.

## 2. Tested Branch, Identity, and Commits

- Branch: `phase4b5-browser-controls-live-status`
- Firmware identity: `v2-phase4b.5-dev`
- Browser-controls implementation: `594445b42d66ada780fd1e34f5084b0a2bab96ac`
- Recovery deadlock correction: `efcc8a3eb2ede7279242a848936408287cd03f7b`
- Motor-disconnected evidence/documentation: `c3122bed41198dba136bda1a029c39a5560dd0f8`
- Browser E-stop pending-command correction: `666c1b0ee610c8041f8afd11bd41b3230e1eee5a`

The documentation commit containing this living record is evidence-only and does not replace the implementation or correction commits above.

## 3. Hardware Isolation

Before any live browser command, the operator confirmed that the motor was fully isolated and physically absent from the bench. No motor, motor-driver load, pump, coupling, or mechanical assembly was operated. The July 22 E-stop correction validation used the same motor-isolated controller boundary.

## 4. Build and Flash Evidence

| Item | Result |
|---|---|
| ESP-IDF | v5.5.3 |
| Build method | `idf.py fullclean` then no-ccache `idf.py build` |
| Compiler warnings | 0 |
| Compiler errors | 0 |
| Corrected E-stop image | `0x107880` bytes |
| Smallest OTA partition | `0x300000` bytes |
| OTA headroom | `0x1f8780` bytes (66%) |
| Controller | COM5, ESP32-S3 QFN56 revision 0.2, 8 MB PSRAM, USB-Serial/JTAG |
| Flash | PASS; all written-image hashes verified |

The earlier post-recovery-correction evidence image was `0x1073e0` bytes with `0x1f8c20` bytes (66%) OTA headroom. Those values remain part of the historical evidence for `efcc8a3` and `c3122be`; the larger values above are authoritative for `666c1b0`.

## 5. Host-Test Evidence

`node tools/test_phase4b5_controls.mjs` passed after the E-stop correction. Coverage includes exact milli-RPM conversion, all documented command response classes, invalid-input non-dispatch, no optimistic machine-state mutation, status freshness behavior, and command concurrency.

The corrective concurrency vectors prove that:

- one pending ordinary request permits exactly one later E-stop POST;
- repeated E-stop clicks while that request is pending emit no duplicate;
- ordinary commands remain blocked while either lane requires serialization;
- the older ordinary response cannot overwrite the newer E-stop result;
- each request retains its own timeout and completion;
- only a later authoritative status response changes displayed machine state or E-stop latch;
- known unauthorized state prevents E-stop transmission; and
- stale or disconnected status does not remove E-stop availability while authentication remains valid and the page remains active.

## 6. Pure-Suite Totals

The corrected image was exercised through three complete diagnostic option `t` invocations using a genuine Windows ConPTY-backed `idf.py monitor` session.

| Invocation | Distinct tests | Internal passes | Executions | Passed | Failed |
|---|---:|---:|---:|---:|---:|
| 1 | 167 | 3 | 501 | 501 | 0 |
| 2 | 167 | 3 | 501 | 501 | 0 |
| 3 | 167 | 3 | 501 | 501 | 0 |
| Aggregate | 167 | 9 | 1,503 | 1,503 | 0 |

No accepted test disappeared. Strengthened browser orchestration checks remained inside the existing four Phase 4B.5 registered test groups, so the distinct count remained 167.

## 7. Browser Preflight Results

**Result: PASS.** The authenticated `/controls` page loaded with automatic live status, correct controller identity, `AP_DISCOVERABLE`, `SUPERVISORY/MQTT`, zero requested/applied/generated speeds, and a disabled driver. Ordinary browser motion controls remained disabled outside browser-owned Local Service authority. The E-stop control remained visually prominent, and the page did not claim that controller output meant physical motion.

## 8. Local Service Entry Results

**Result: PASS.** Authenticated browser service entry established `SERVICE_AP_ONLY` and `LOCAL_SERVICE/BROWSER`, disabled STA operation, stopped the broker, and made eligible browser commands admissible. The transition caused no physical motion.

## 9. Isolated Command and State Tests

**Result: PASS within the motor-disconnected boundary.** The operator exercised the seven accepted browser command forms one at a time and observed both HTTP results and subsequent authoritative status.

- `set_target` accepted forward and reverse targets without creating output before Start.
- `start` produced the expected controller-state and generated-output transitions.
- `stop` returned through deceleration to stationary `HOLDING`.
- `unlock` disabled the driver and left the controller stationary.
- `estop` took priority, latched `EMERGENCY_STOPPED`, and reduced controller output to zero.
- `reset_estop` cleared an eligible latch without automatic restart.
- `recover` returned an eligible stationary controller to `UNLOCKED` after the correction described below.

Responses and displayed controller status remained distinct. No physical-motion conclusion was drawn because the motor and driver load were absent.

## 10. Initial Recovery Failure

**Initial result: FAILED.** The first live browser `recover` request did not complete. Status polling became stale and the HTTP task remained blocked. The controller did not panic or reset, and no unsafe output or physical motion occurred. This failed test is retained because it exposed the defect corrected in `efcc8a3`.

## 11. Deadlock Diagnosis

`argus_trajectory_recover()` held the non-recursive trajectory mutex and called public `argus_trajectory_clear_error()`, which attempted to acquire the same mutex. The resulting self-deadlock blocked the request and status service path. The evidence supported a trajectory mutex defect, not a Wi-Fi, browser parsing, authority, router, or motor-output failure.

## 12. Correction and Regression Coverage

Commit `efcc8a3eb2ede7279242a848936408287cd03f7b` added a lock-held error-clear helper. Recovery uses that helper while it owns the mutex; the public clear function preserves its lock-taking contract. A host regression fails if recovery again calls the public lock-taking function from inside the locked section.

Commit `666c1b0ee610c8041f8afd11bd41b3230e1eee5a` separated ordinary-command and E-stop in-flight state. It permits one authenticated E-stop to overtake a pending ordinary request, suppresses duplicates, blocks new ordinary concurrency, preserves independent finite timeouts, and uses monotonically ordered result tokens so an older response cannot replace the newer E-stop result. It does not change the endpoint, router, state manager, trajectory, pulse engine, GPIO path, or physical E-stop semantics.

## 13. Corrected Recovery Reproduction

**Result: PASS.** From stationary `HOLDING` with zero generated and applied speed, one browser Recover action returned `Recover accepted by API`. Subsequent authoritative status reported `UNLOCKED`, zero generated/applied speed, disabled driver, clear E-stop, preserved `SERVICE_AP_ONLY` and `LOCAL_SERVICE/BROWSER`, and continued healthy polling.

Recovery deadlock disposition: **CORRECTED AND REPRODUCED SUCCESSFULLY.**

## 14. Service Exit and Reboot Recovery

**Result: PASS.** Authenticated service exit was accepted and the coordinated reboot restored configured `AP_DISCOVERABLE`, `SUPERVISORY/MQTT`, `UNLOCKED`, zero output, disabled driver, and running broker state. No stale browser command, physical movement, or reset loop was observed.

## 15. Stability and Production Isolation

Every final corrected-image suite invocation preserved its starting production state:

- authority generation unchanged at Gen 3;
- network state unchanged at `AP_DISCOVERABLE`;
- broker state unchanged at `RUNNING`;
- machine state unchanged at `UNLOCKED`; and
- task count unchanged at 16.

All three runs reported production-singleton isolation with zero live mutations and zero task leaks. There was no panic, unexpected reset, watchdog, brownout, assertion, stack-canary failure, heap corruption, task leak, or production-state contamination.

## 16. Operator-Assisted Actions and Observations

Operator assistance was limited to confirming physical isolation, arranging the laptop/controller network path, reconnecting Windows to the controller Service AP when required, authenticating the browser session, and observing browser/controller behavior during the isolated command sequence. Automated tooling performed the clean builds, flash, host checks, and final ConPTY suite capture. No operator observation was promoted into powered-motion evidence.

## 17. Explicit Exclusions

The following were not connected, operated, or accepted:

- motor or loaded motor driver;
- pump head, tubing, or hose;
- chemical, water, pressure, or process load;
- physical direction, shaft motion, torque, or stopping distance;
- flow accuracy or calibration; and
- mechanical endurance.

No pump, hose, chemical, pressure, flow-accuracy, process, or mechanical-endurance acceptance is implied.

## 18. Remaining Powered UI-to-Motor Procedure

The remaining gate is a narrow, separately authorized connected-motor confirmation using the final `/controls` page and accepted lower-layer hardware doctrine. It must positively establish physical isolation controls and safe test conditions before power is applied, then confirm at low speed that setpoint alone causes no motion, Start produces the commanded direction and smooth ramp, Stop returns smoothly to zero, Unlock disables holding output, reverse produces opposite shaft direction, and E-stop halts output with truthful latched status. HTTP 200 must not be used as proof of physical motion or cessation.

Any unexpected motion, wrong direction, failure to stop, abnormal sound, driver fault, heat, smoke, panic, reset, watchdog, brownout, stack/heap failure, or dishonest status is an immediate hard stop. No powered procedure is authorized or performed by this record.

## 19. Current Acceptance Disposition

| Gate | Disposition |
|---|---|
| Phase 4B.5 motor-disconnected implementation | **VALIDATED** |
| Motor-disconnected controller and browser testing | **PASSED** |
| Recovery deadlock | **CORRECTED AND REPRODUCED SUCCESSFULLY** |
| Browser E-stop pending-command correction | **PASSED** |
| Powered UI-to-motor integration | **PENDING** |
| Final Phase 4B.5 acceptance | **NOT YET GRANTED** |

## 20. Final Powered Result

**Tested firmware commit:** `[PENDING]`

**Powered UI-to-motor result:** `[PENDING]`

**Evidence:** `[PENDING]`

**Final Phase 4B.5 acceptance:** `[PENDING]`

Phase 4B.5 may be marked finally accepted only after the powered procedure passes against an explicitly identified firmware commit and its evidence is reviewed. That acceptance must not imply pump, hose, chemical, pressure, flow-accuracy, process, or mechanical-endurance acceptance.
