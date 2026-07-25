# Phase 4D.4 Tests and Acceptance Record

**Status:** COMPLETE AND ACCEPTED

**Acceptance date:** July 25, 2026

**Branch:** `phase4d4-machine-enrollment-mqtt-auth`

**Accepted baseline:** `05d6cde3e00c142beb9f2df6b88f501893254942`

**Accepted implementation candidate:** `cea28c2c8476f8f991f8337699e24cb16ea217e8`

**Firmware identity:** `v2-phase4d.4-dev`

## 1. Acceptance Boundary

This record accepts Phase 4D.4 machine enrollment, MQTT CONNECT
authentication, connection-bound authorization, credential invalidation,
stationary Phase 4C regression, and browser regression.

The motor was physically disconnected for the complete final sequence. No Start,
nonzero setpoint, Recover, or other command capable of motor motion was issued.
This record does not accept powered motion, pump, hose, tubing, fluid, chemical,
pressure, flow accuracy, calibration, load, process, or endurance behavior.

MQTT remains plaintext. This record does not claim HTTPS/TLS, certificate,
hostile-network, penetration, physical key-extraction, HMI, or AI integration
acceptance.

## 2. Corrective Source Review

Independent review paused the first live attempt and required four lifecycle
corrections:

1. Disconnect selection and shutdown are bound to the same client slot,
   principal, connection ID, and socket lifetime. File-descriptor reuse cannot
   redirect shutdown to an unrelated connection.
2. Successful authentication and principal binding coordinate with a bounded
   machine invalidation epoch. Invalidation before binding rejects the stale
   result; invalidation after binding finds and terminates the exact connection.
3. A committed rotate, disable, revoke, or delete mutation attempts immediate
   disconnect even if terminal audit finalization subsequently fails.
4. Ordinary enrollment retains a fail-closed operational capability ceiling.
   Service-tool labels and `AI_TOOL_GATEWAY` are classifications only and grant
   no permission, scope, interface, transport, browser access, authority, or
   command path.

Focused regression coverage also proves that an invalidated connection is
policy-inert if physical socket closure fails. It cannot subscribe, publish,
mutate heartbeat or sequence state, reach authority lookup, or dispatch.
Unrelated machine connections and human sessions remain unaffected.

## 3. Machine-Writer Stack Correction

The first corrected controller image exposed a deterministic machine-directory
writer stack overflow. `commit_locked()` created a 6,520-byte compound-literal
temporary on a 6,144-byte task stack.

The accepted correction writes the new slot directly into the heap-backed
machine-directory storage and uses bounded `memcpy` operations. The component
now compiles with:

`-Wframe-larger-than=2048 -Werror=frame-larger-than=2048`

Final runtime diagnostics showed 5,344 bytes remaining on the 6,144-byte writer
stack, corresponding to approximately 800 bytes peak use. The KDF worker retained
4,764 bytes. No stack-canary, heap-corruption, panic, or reset evidence remained.

## 4. Build and Image Evidence

| Item | Result |
|---|---|
| ESP-IDF | v5.5.3 |
| Build | Full-clean, no-ccache |
| Compiler warnings | 0 |
| Compiler errors | 0 |
| Application binary | `0x12b3b0` bytes |
| Smallest OTA headroom | `0x1d4c50` bytes (61%) |
| Total image size | 1,225,525 bytes |
| DIRAM used | 201,311 bytes |
| `.bss` | 96,544 bytes |
| `.data` | 20,156 bytes |

COM5 identified the expected ESP32-S3, QFN56 revision 0.2, with 8 MB PSRAM.
The final full-clean image flashed with verification.

## 5. Automated Controller Runtime

Diagnostic option `t` ran through a genuine Windows ConPTY-backed ESP-IDF
monitor three complete times.

| Invocation | Distinct tests | Internal passes | Executions | Passed | Failed |
|---|---:|---:|---:|---:|---:|
| 1 | 268 | 3 | 804 | 804 | 0 |
| 2 | 268 | 3 | 804 | 804 | 0 |
| 3 | 268 | 3 | 804 | 804 | 0 |
| **Aggregate** | **268** | **9** | **2,412** | **2,412** | **0** |

Each invocation preserved:

- authority generation 6;
- network mode `SERVICE_AP_ONLY`;
- broker state `STOPPED`;
- machine state `UNLOCKED`; and
- task count 22.

Every invocation returned normally. There was no panic, unexpected reset,
watchdog, brownout, assertion, stack-canary failure, heap corruption, task leak,
motor movement, or production-state contamination. Coordinated service exit then
completed and rebooted cleanly.

## 6. Final Boot and Stationary Preflight

The final attached-from-boot monitor established:

- firmware `v2-phase4d.4-dev`;
- commissioned identity `paladin` / `pump_001`;
- machine directory ready and readable;
- current STA address `192.168.50.236` on `CherryHome1`;
- MQTT broker `RUNNING`;
- network mode `AP_DISCOVERABLE`;
- authority `SUPERVISORY/MQTT`, generation 3;
- machine state `UNLOCKED`;
- configured, applied, and generated speed zero; and
- driver disabled.

The STA address is observed evidence only and is not a permanent configuration
assumption.

## 7. Live Enrollment

A temporary Node-RED-class machine was enrolled through the authenticated SoftAP
security console. Its allowed interface was SoftAP, controller scope was `*`,
topic scope was `argus/paladin/pump_001`, and permissions were limited to
`view_status` and `request_authority`.

The controller generated a 43-character one-time secret. It was displayed once,
kept only in volatile test tooling, and cleared after use. The list and later
administrative responses did not disclose it.

**Result:** PASS

## 8. MQTT CONNECT Matrix

| Case | Interface | Expected | Actual | Result |
|---|---|---|---|---|
| Missing machine credentials | SoftAP | Reject | CONNACK code 2 | PASS |
| Correct identity, incorrect secret | SoftAP | Reject | CONNACK code 4 | PASS |
| Correct identity and secret | STA | Reject by interface policy | CONNACK code 4 | PASS |
| Correct identity and secret | SoftAP | Accept | CONNACK code 0 | PASS |

No rejected CONNECT created a bound machine principal, connected event,
heartbeat mutation, authority change, or command path.

## 9. Subscription and Publication Policy

The authenticated SoftAP machine received SUBACK success for:

- the exact controller authority-status topic; and
- the exact shared Phase 4C `event/pump1/command_result` topic.

The broad root subscription `argus/paladin/pump_001/#` received SUBACK `0x80`.
Authentication alone left the supervisor link `OFFLINE` and authority unchanged.

A QoS 1 publish to controller-owned `state/core/online` was policy-dropped before
retained mutation or subscriber delivery. No PUBACK was returned, the retained
controller value remained `true`, and the socket remained usable. A simultaneous
duplicate MQTT client ID was rejected deterministically with CONNACK code 2.
After the first socket closed cleanly, the same identity reconnected successfully.

**Result:** PASS

## 10. Heartbeat and Authority Isolation

The client read the current retained 16-character command session and sent one
strict, non-retained QoS 1 heartbeat. The broker returned PUBACK. Supervisor link
became `ONLINE`, then naturally became `STALE` after the accepted timeout.

Throughout the heartbeat lifecycle:

- authority remained `SUPERVISORY/MQTT`;
- no command sequence was consumed;
- target and outputs remained zero;
- the driver remained disabled;
- machine state remained `UNLOCKED`; and
- no command dispatch or motion occurred.

**Result:** PASS

## 11. Rotation

The credential for the live machine was rotated through the authenticated
SoftAP console.

- The exact live MQTT socket closed.
- The old secret subsequently received CONNACK code 4.
- The new one-time secret received CONNACK code 0.
- The stable machine ID was unchanged.
- The one-time secret field was cleared and the dialog closed after use.
- The active human browser session remained usable.

**Result:** PASS

## 12. Revocation

The final revocation proof used a detached bounded verifier whose credential was
received through standard input only. It wrote no secret to its command line,
result file, logs, source, or repository.

Observed result:

1. The machine connected successfully with CONNACK code 0.
2. Revocation committed in the authenticated console.
3. The exact live socket disconnected.
4. Reconnect with the same credential received CONNACK code 4.
5. The human browser session remained usable.

Two earlier browser-automation attempts were discarded because the automation
runtime reset and erased its own volatile credential state. They are not used as
acceptance evidence.

**Result:** PASS

## 13. Cleanup and Audit

All temporary machine records were revoked and deleted. The final Machines table
was empty, and the audit ledger contained correlated prepared/succeeded records
for enrollment, revocation, and deletion.

A browser-automation timeout caused one delete request to be repeated after the
first delete had already committed. The repeat truthfully produced
`delete_failed`; the successful deletion remained recorded and no machine record
or credential remained. This is not a firmware defect or acceptance exception.

No reusable machine credential remains in repository files, documentation,
captured result files, shell command history, logs, or chat evidence.

**Result:** PASS

## 14. Browser and Phase 4C Regression

The authenticated Overview and Operations pages remained available and truthful:

- device identity and firmware were correct;
- network mode was `AP_DISCOVERABLE`;
- STA was connected at the observed current address;
- broker was running;
- authority was `SUPERVISORY/MQTT`, generation 3;
- machine state was `UNLOCKED`;
- configured, applied, and generated values were zero;
- driver was disabled; and
- ordinary browser motion controls were disabled because browser authority was
  not admitted.

No browser command was issued. The Phase 4C command router, authority manager,
state manager, fail-operational behavior, and controller-owned topic contract
were not bypassed or weakened.

**Result:** PASS

## 15. Final Acceptance

| Gate | Result |
|---|---|
| Supervisory lifecycle corrections | PASS |
| Machine-writer stack correction | PASS |
| ESP-IDF v5.5.3 full-clean build | PASS |
| Three complete controller suites | PASS |
| Stationary boot and preflight | PASS |
| Enrollment and one-time disclosure | PASS |
| CONNECT authentication/interface matrix | PASS |
| Subscription and publication authorization | PASS |
| Heartbeat and authority isolation | PASS |
| Exact-connection rotation invalidation | PASS |
| Exact-connection revocation invalidation | PASS |
| Temporary-record and credential cleanup | PASS |
| Browser and Phase 4C stationary regression | PASS |
| **Phase 4D.4 software and stationary live acceptance** | **ACCEPTED** |

Acceptance applies specifically to implementation candidate
`cea28c2c8476f8f991f8337699e24cb16ea217e8` plus the later documentation,
merge, and annotated acceptance tag that preserve that source unchanged.
