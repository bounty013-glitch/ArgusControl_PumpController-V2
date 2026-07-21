# Phase 4B.3a - Physical Acceptance: Wi-Fi Observability and Recovery

- **Acceptance status:** ACCEPTED
- **Acceptance date:** July 21, 2026
- **Firmware identity:** `v2-phase4b.3a-dev`
- **Accepted firmware commit:** `87eff30f36c9d264351ee939ff4061116c0dd128`
- **Evidence screenshot commit:** `62674ad26312cab040cbe6f72661b7d6f1593db5`

The accepted firmware commit is the reviewed runtime candidate. The later screenshot commit contains evidence files only and does not change the accepted firmware. This record documents observed hardware results for that specific candidate.

## Prerequisites and Evidence Boundary

1. Independent source review completed with no blocking findings.
2. The controller was flashed with accepted firmware commit `87eff30f36c9d264351ee939ff4061116c0dd128`.
3. The serial diagnostic console was available throughout testing.
4. The operator device remained able to use the controller Service AP at `http://192.168.4.1` as required by each test.
5. Screenshot evidence was preserved in commit `62674ad26312cab040cbe6f72661b7d6f1593db5`.

## Flashing and Boot Identity

**Initial condition:** Reviewed Phase 4B.3a firmware candidate prepared for a normal ESP-IDF build, flash, and monitor cycle.

**Operator action:** Performed the normal build, flash, and serial-monitor workflow, then observed the first boot.

**Observed evidence:** Build, flash, and monitor completed successfully. ESP-IDF reported project `ArgusControl_PumpController-V2`, application version `v2-phase4b.3a-dev`, and `app_main` logged `Argus Pump Controller V2 firmware starting (Phase 4B.3a)`.

**Actual outcome:** Firmware identity was displayed once and matched the accepted candidate. **PASS**

## Pure-Suite Preflight

**Initial condition:** First post-flash boot with invalid STA information, active reconnection behavior, `AP_DISCOVERABLE`, broker stopped, machine `UNLOCKED`, and 15 tasks.

**Operator action:** Selected diagnostic option `t` before beginning Physical Tests 1-9.

| Runtime result | Observed value |
|---|---:|
| Distinct tests | 142 |
| Repeat passes | 3 |
| Total executions | 426 |
| Passed | 426 |
| Failed | 0 |

| Production-isolation field | Preflight result |
|---|---|
| Authority generation | UNCHANGED, Gen 30 |
| Network state | UNCHANGED, `AP_DISCOVERABLE` |
| Broker state | UNCHANGED, `STOPPED` |
| Machine state | UNCHANGED, `UNLOCKED` |
| Task count | UNCHANGED, 15 |

**Actual outcome:** The suite banner reported PASS and preserved every observed production baseline. **PASS**

## Test 1: Correct-Credential Baseline

**Initial condition:** Commissioned STA credentials required correction while the Service AP remained available.

**Operator action:** Entered and saved valid commissioned Wi-Fi credentials.

**Observed evidence:** NVS commit and readback verification passed. Serial showed configuration application, STA authentication and association, `STA associated; waiting for IP`, IP acquisition at `192.168.1.22`, broker listener startup, and authority transition from `NONE/NONE` to `SUPERVISORY/MQTT`. The Service AP continued serving a client at `192.168.4.2`.

**Actual outcome:** STA reached the connected/IP state, broker and supervisory authority started only after IP acquisition, and operation remained discoverable through the Service AP. **PASS**

**Screenshots:** [Phase 4B.3a Test 1 - PASS.png](<Evidence Screen Shots/Phase 4B.3a/Phase 4B.3a Test 1 - PASS.png>), [Phase 4B.3a - Wifi Config.png](<Evidence Screen Shots/Phase 4B.3a/Phase 4B.3a - Wifi Config.png>)

## Test 2: Connected Apply Ordering

**Initial condition:** STA connected with broker running and `SUPERVISORY/MQTT` authority.

**Operator action:** Saved a different valid credential set while connected and observed serial and dashboard state.

**Observed evidence:** Authority changed to `NONE/NONE`; the broker task exited and reported cleanly stopped; the intentional disconnect matched transaction 38; only then did the STA authenticate, associate, obtain `10.110.71.141`, restart the broker, and restore `SUPERVISORY/MQTT`. Service AP DHCP activity at `192.168.4.2` continued during the transaction.

**Actual outcome:** Disconnect, configuration application, reconnect, broker restart, and authority restoration occurred in the required order without losing AP/HTTP access. **PASS**

**Screenshot:** [Phase 4B.3a Test 2 - PASS.png](<Evidence Screen Shots/Phase 4B.3a/Phase 4B.3a Test 2 - PASS.png>)

## Test 3: Incorrect Password and Bounded Authentication Retry

**Initial condition:** Connected commissioned operation with broker and supervisory authority active.

**Operator action:** Saved the valid SSID with an incorrect password and observed authentication failures and bounded operator behavior.

**Observed evidence:** Authority was revoked to `NONE/NONE`, the broker stopped cleanly, raw reason 15 (`4WAY_HANDSHAKE_TIMEOUT`) was retained and classified as `AUTHENTICATION`, retry behavior remained bounded, and failure information remained available while the Service AP and HTTP dashboard continued operating.

**Language caveat:** Dashboard/operator wording did not exactly match the originally planned text. The observed language remained truthful, understandable, actionable, and acceptable. This was accepted as functional behavior, not a runtime defect or acceptance exception.

**Actual outcome:** Raw reason, authentication classification, retained failure evidence, bounded operator behavior, stopped broker, and continued Service AP/HTTP availability were accepted. **PASS**

## Test 4: Manual Reconnect with Unchanged Bad Credentials

**Initial condition:** Authentication recovery retained bad credentials and truthful failure history.

**Operator action:** Requested one manual reconnect, then attempted a duplicate while recovery was in progress.

**Observed evidence:** The first request began a manual reconnect transaction. The dashboard showed recovery in progress and retained the earlier evidence. The duplicate request did not start a second transaction. The failed attempt remained truthful, with reason 205 (`CONNECTION_FAIL`) followed by reason 15 (`4WAY_HANDSHAKE_TIMEOUT`) and scheduled bounded retries.

**Actual outcome:** Manual recovery did not clear history merely because it was requested, and duplicate work was suppressed. **PASS**

**Screenshot:** [Phase 4B.3a Test 4 - Recovery in Progress.png](<Evidence Screen Shots/Phase 4B.3a/Phase 4B.3a Test 4 - Recovery in Progress.png>)

## Test 5: Correct Credentials and Successful Recovery

**Initial condition:** STA was in failed/retry recovery with retained failure evidence and broker stopped.

**Operator action:** Saved correct credentials.

**Observed evidence:** NVS commit/readback passed; STA authenticated and associated; the manager reported `STA associated; waiting for IP`; IP `192.168.1.22` was acquired; the broker restarted; and authority changed from `NONE/NONE` to `SUPERVISORY/MQTT` at generation 73.

**Actual outcome:** The asynchronous apply completed only after IP acquisition, normal connected operation returned, and active failure state cleared. **PASS**

**Screenshot:** [Phase 4B.3a Test 5 - PASS.png](<Evidence Screen Shots/Phase 4B.3a/Phase 4B.3a Test 5 - PASS.png>)

## Test 6: AP Unavailable and Automatic Recovery

**Initial condition:** Connected commissioned operation with broker and supervisory authority active.

**Operator action:** Disabled the commissioned upstream AP and then restored it.

**Observed evidence:** Loss of the upstream AP revoked authority, stopped the broker cleanly, retained raw driver reason 1 as truthful `UNKNOWN`/category 4 evidence, and scheduled a non-authentication retry without entering authentication `ACTION_REQUIRED`. After the AP returned, the STA associated, acquired `10.110.71.141`, restarted the broker, and restored `SUPERVISORY/MQTT` at generation 77. No countdown underflow was observed.

**Actual outcome:** Upstream loss remained observable, retry stayed bounded and non-authentication-specific, and automatic recovery restored connected operation. **PASS**

## Test 7: Service AP and HTTP Preservation

**Initial condition:** Recovery was exercised across retry, action-required, connected-apply, and manual-reconnect states.

**Operator action:** Remained attached to the Service AP and repeatedly refreshed the dashboard throughout those states.

**Observed evidence:** The dashboard remained reachable at `192.168.4.1`; Service AP DHCP/client activity continued; and displayed recovery state and retained failure evidence remained consistent with serial observations.

**Actual outcome:** Service AP and HTTP access remained available throughout all tested recovery states. **PASS**

## Test 8: Service-Entry Cancellation

**Initial condition:** Commissioned Wi-Fi retry/recovery was active with Service AP available, STA disconnected, no STA IP, broker observably stopped, and `NONE/NONE` authority.

**Operator action:** Used the browser to enter Local Service during recovery, observed delayed events, then initiated service exit.

**Observed evidence:** Entry reached `SERVICE_AP_ONLY` and `LOCAL_SERVICE/BROWSER`; delayed disconnect and stop events reconfirmed disabled STA state; HTTP restarted on port 80; reconnect was unavailable; and later exit moved through `SERVICE_TRANSITION` to `NONE/NONE`. No delayed event restarted recovery or exposed staged credentials.

**Actual outcome:** Eligibility, cancellation ordering, AP-only convergence, delayed-event suppression, and service ownership behaved as required. **PASS**

**Screenshot:** [Phase 4B.3a Test 8 - PASS.png](<Evidence Screen Shots/Phase 4B.3a/Phase 4B.3a Test 8 - PASS.png>)

**Correction history:** Review after `d8c898f` found that this checklist required recovery service entry while production rejected `AP_DISCOVERABLE + NONE/NONE` and could mutate recovery before rejection. Subsequent corrections added shared policy evaluation, preflight/execution fingerprinting, cancellation truthfulness, and service-mode event suppression. This PASS applies only to accepted firmware commit `87eff30f36c9d264351ee939ff4061116c0dd128`.

## Test 9: Service Exit and Commissioned Reboot

**Initial condition:** Controller in `SERVICE_AP_ONLY` with browser-owned Local Service authority.

**Operator action:** Exited Local Service and reconnected to the Service AP after reboot.

**Observed evidence:** Exit transitioned through `SERVICE_TRANSITION`, revoked browser ownership to `NONE/NONE`, rebooted, and returned commissioned operation to `AP_DISCOVERABLE` with a clean STA lifecycle. No stale transaction or timer event altered the recovered state.

**Actual outcome:** Service exit and commissioned reboot completed cleanly under the always-on AP policy. **PASS**

**Screenshot:** [Phase 4B.3a Test 9 - PASS.png](<Evidence Screen Shots/Phase 4B.3a/Phase 4B.3a Test 9 - PASS.png>)

## Test 10: Final Pure-Suite Execution and Isolation Proof

**Initial condition:** Physical Tests 1-9 completed; controller in `AP_DISCOVERABLE`, broker running, machine `UNLOCKED`, and 16 tasks.

**Operator action:** Selected diagnostic option `t` again after physical testing.

| Runtime result | Observed value |
|---|---:|
| Distinct tests | 142 |
| Repeat passes | 3 |
| Total executions | 426 |
| Passed | 426 |
| Failed | 0 |

| Production-isolation field | Final result |
|---|---|
| Authority generation | UNCHANGED, Gen 3 |
| Network state | UNCHANGED, `AP_DISCOVERABLE` |
| Broker state | UNCHANGED, `RUNNING` |
| Machine state | UNCHANGED, `UNLOCKED` |
| Task count | UNCHANGED, 16 |

**Actual outcome:** The final suite banner reported PASS, all 426 executions passed, and every production baseline remained unchanged. **PASS**

The differing preflight and final isolation baselines are expected. Each suite execution preserved the production state that existed when that execution began.

## Final Acceptance

| Acceptance item | Result |
|---|---|
| Flash and boot identity | PASS |
| Pure-suite preflight | PASS, 426/426 |
| Physical Tests 1-9 | PASS |
| Final pure suite and isolation proof | PASS, 426/426 |
| Phase 4B.3a physical acceptance | **ACCEPTED** |

Phase 4B.3a was physically accepted on July 21, 2026, specifically for firmware commit `87eff30f36c9d264351ee939ff4061116c0dd128` with firmware identity `v2-phase4b.3a-dev`. Screenshot evidence was added later in evidence-only commit `62674ad26312cab040cbe6f72661b7d6f1593db5`.

## Preserved Correction Record

Earlier Phase 4B.3a candidates were not accepted merely because they built or passed source-level checks. Review after `d8c898f` exposed recovery service-entry policy and mutation-order defects. Review after `5daab467` exposed stale operational-event flag corruption and incomplete broker-stop convergence. Commit `085e8ef` corrected those composition-level blockers, and commit `87eff30` corrected final firmware/runtime evidence identity. Physical acceptance applies only after that complete review and correction sequence and the hardware results recorded above.
