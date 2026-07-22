# Phase 4B.6 Tests and Acceptance Record

**Current disposition:** FULLY ACCEPTED on July 22, 2026. Phase 4B is complete and accepted. Phase 4C has not started.

## 1. Acceptance Boundary

Phase 4B.6 completes the controller-hosted technician portal lifecycle: configuration preservation, coordinated restart, Local Service entry and exit, configuration factory reset, truthful uncommissioned recovery, exact recommissioning, and restoration of normal supervisory operation.

This campaign issued no motion command and did not repeat the accepted Phase 4B.5 powered UI-to-motor campaign. It adds no pump-head, hose, tubing, fluid, chemical, pressure, flow-accuracy, calibration, load, torque, process, or endurance acceptance.

## 2. Baseline, Branch, and Identity

- Accepted starting main: `0ab8cbee5aa64d3ded695bbc75ad09c1f0583439`
- Accepted baseline tag: `v2-phase4b.5-hardware-verified`
- Feature branch: `phase4b6-portal-lifecycle-acceptance`
- Firmware identity: `v2-phase4b.6-dev`
- Identity establishment: `75ebd88` (`chore: establish Phase 4B.6 identity`)
- Factory-reset implementation: `512f433` (`feat: add coordinated portal factory reset`)
- Lifecycle tests: `9549dde` (`test: cover Phase 4B.6 portal lifecycle`)

## 3. Implemented Contract

`POST /api/factory-reset` requires valid Basic authentication, `application/json`, exact bounded confirmation `{"confirm":"FACTORY_RESET"}`, `SERVICE_AP_ONLY`, `LOCAL_SERVICE/BROWSER`, no conflicting lifecycle operation, and a stationary restart-safe machine.

The HTTP task authenticates, decodes, evaluates policy, queues once, and sends bounded HTTP 202. The network-manager task revalidates authority, network convergence, and machine safety; establishes safe output; stops HTTP; revokes authority; executes the durable NVS transaction; and reboots only after success. Rejected requests queue no reset. The handler does not synchronously stop itself, erase NVS, or reboot.

The configuration reset erases both configuration slots, selector metadata, identity-provisioning high-water state, identity configuration, and STA configuration. It preserves the reset-pending transaction namespace and the dedicated portal-authentication namespace. Erased identity is presented using truthful uncommissioned effective defaults; `identity_provisioned:false` proves that no identity is persisted or locked.

## 4. Host and Source Validation

| Check | Result |
|---|---|
| `node tools/test_phase4b5_controls.mjs` | PASS |
| `node tools/test_phase4b6_portal.mjs` | PASS |
| Embedded JavaScript syntax | PASS |
| Factory-reset URI and POST registration | PASS |
| Strict decoder/media-type/body-result vectors | PASS |
| Policy and lifecycle-owner revalidation vectors | PASS |
| Durable reset and boot-recovery vectors | PASS |
| Portal-credential namespace exclusion | PASS |
| UI eligibility, exact warning, and two-action confirmation | PASS |
| Production factory-reset execution call sites | Exactly 1 |
| Direct HTTP-handler lifecycle calls | 0 |
| Production singleton mutation from pure tests | None found |
| `git diff --check` | PASS |

The existing Phase 4B.5 host test was changed only to tolerate CRLF or LF source line endings; its semantic assertions were not weakened.

## 5. Build and Size Evidence

| Item | Result |
|---|---|
| ESP-IDF | v5.5.3 |
| Clean command | `idf.py fullclean` |
| Build command | `idf.py --no-ccache build` |
| CMake ccache state | `CCACHE_ENABLE=0` |
| Compiler warnings | 0 |
| Compiler errors | 0 |
| Application binary | `0x10a750` bytes (1,091,408 bytes) |
| Smallest OTA slot | `0x300000` bytes |
| OTA headroom | `0x1f58b0` bytes (2,054,320 bytes; 65%) |

An earlier command supplied conflicting ccache definitions and was rejected as authoritative evidence. The recorded build above is the later full-clean build with ccache explicitly disabled.

## 6. Controller and Flash Evidence

- Port: COM5
- Chip: ESP32-S3 QFN56 revision 0.2
- PSRAM: 8 MB
- Transport: USB Serial/JTAG
- Hardware MAC: `3c:dc:75:6e:c2:d0`
- Flash command: `idf.py -p COM5 flash`
- Flash result: PASS; all four written-image hashes verified

The controller was authoritatively stationary before flash and every lifecycle mutation: `UNLOCKED`, zero applied speed, zero generated speed, idle ramp, E-stop clear, and driver disabled.

## 7. Pure-Suite Runtime Evidence

Three complete top-level diagnostic option `t` invocations ran through a genuine Windows ConPTY-backed `idf.py monitor` session.

| Invocation | Distinct tests | Internal passes | Executions | Passed | Failed |
|---|---:|---:|---:|---:|---:|
| 1 | 177 | 3 | 531 | 531 | 0 |
| 2 | 177 | 3 | 531 | 531 | 0 |
| 3 | 177 | 3 | 531 | 531 | 0 |
| Aggregate | 177 | 9 | 1,593 | 1,593 | 0 |

Every invocation preserved authority generation 3, `AP_DISCOVERABLE`, MQTT `RUNNING`, machine `UNLOCKED`, and 16 tasks. No panic, reset, watchdog, brownout, assertion, stack-canary failure, heap corruption, task leak, or production-state contamination occurred during the suite.

## 8. Initial Commissioned Checks

**Result: PASS.** The accepted configuration was recorded through authorized masked APIs:

- client ID `paladin`
- unit ID `pump_001`
- device name `Argus Peristaltic Pump V2 - TEST`
- STA SSID `Paladin6661`
- identity provisioned and locked
- firmware `v2-phase4b.6-dev`
- STA address `192.168.1.22` at each observed commissioned recovery

A same-SSID Wi-Fi save omitted the password. The connection was intentionally lost during runtime reapply, so the transport result was treated as unknown and the request was not retransmitted. Authoritative recovery showed `sta_pass_set:true`, connected STA, running broker, and unchanged identity. A subsequent identity mutation returned HTTP 403 `identity_locked` with `Cache-Control: no-store`.

## 9. Coordinated Restart

**Result: PASS.** Authenticated `POST /api/restart` returned `restart_initiated`. After reboot the controller returned commissioned in `AP_DISCOVERABLE` with `SUPERVISORY/MQTT`, broker running, zero output, and driver disabled. Serial evidence showed an expected coordinated software reset and no fault signature.

## 10. Local Service Entry and Exit

**Result: PASS.** Service entry returned HTTP 202 and converged to:

- network `SERVICE_AP_ONLY`
- authority `LOCAL_SERVICE/BROWSER`
- STA disconnected with no STA IP
- broker stopped
- machine `UNLOCKED`, stationary, driver disabled

Service exit returned HTTP 202, revoked browser authority, rebooted, and restored `AP_DISCOVERABLE`, connected STA, running broker, and `SUPERVISORY/MQTT`. A second entry established the same safe state for factory reset.

## 11. Factory-Reset UI and Reboot

**Result: PASS.** A disposable local Chromium session loaded the real authenticated dashboard while Windows was associated with the controller Service AP. The reset control was authoritatively enabled. The automation performed the two deliberate operator actions implemented by production UI:

1. Click `Factory Reset`.
2. Accept the exact scope warning stating that identity, identity lock, and Wi-Fi configuration are erased while the portal password is preserved.

The production request returned HTTP 202 with `factory_reset_accepted`; the accepted transition view rendered before disconnect. Serial evidence showed the durable reset workflow and expected reboot.

## 12. Post-Reset State and Credential Boundary

**Result: PASS.** After reboot:

- commissioned was false
- network was `UNCOMMISSIONED_AP`
- authority was `NONE/NONE`
- broker was stopped
- machine remained `UNLOCKED`, stationary, and driver-disabled
- identity provisioning lock was clear
- STA SSID/password were absent
- uncommissioned identity values were truthful effective defaults, not persisted identity
- the prior nondefault portal credential authenticated successfully
- default `admin/admin` returned HTTP 401

The factory reset therefore preserved portal authentication without silently restoring the default credential.

## 13. Recommissioning and Final Round Trip

**Result: PASS.** The exact original identity was provisioned once. A second mutation returned HTTP 403 `identity_locked`, proving the lock was restored. The exact `Paladin6661` STA configuration was restored through a local in-memory transfer from the unchanged authorized Windows profile. Runtime apply returned the controller to commissioned `AP_DISCOVERABLE` operation.

A final service entry/exit round trip again proved exclusive browser authority in AP-only service and clean restoration to supervisory operation. Final authoritative state:

| Field | Final value |
|---|---|
| Firmware | `v2-phase4b.6-dev` |
| Commissioned | true |
| Identity | Exact pre-reset values restored; locked |
| STA SSID | `Paladin6661` |
| STA | Connected; `192.168.1.22` |
| Network | `AP_DISCOVERABLE` |
| Broker | Running |
| Authority | `SUPERVISORY/MQTT` |
| Machine | `UNLOCKED` |
| Applied/generated speed | 0 / 0 |
| Driver | Disabled |

## 14. Single-Adapter Runner and Failure History

The temporary runner was outside the repository. It used bounded timeouts, sanitized timestamped evidence, in-memory credential transfer, serial capture, deterministic transitions, and guaranteed `Paladin6661` restoration in `finally`. It did not alter or delete either saved Windows WLAN profile. Secret buffers were wiped and the temporary NVS image was deleted after every run.

The following non-product failures remain part of the record:

1. Two startup runs timed out while Windows association was still settling after the USB reset used for authorized NVS credential retrieval. Both stopped before a controller HTTP mutation and restored `Paladin6661`.
2. One run accepted service entry, then the harness treated a transient nonzero `netsh wlan connect` result as final. Serial proved lifecycle entry had begun; controller recovery and cleanup were confirmed. The runner was corrected to retry association throughout a bounded window.
3. The successful destructive run stopped after reset because the harness incorrectly expected empty `/api/identity` display strings. Source review proved uncommissioned effective defaults are intentional; masked config and `identity_provisioned:false` were authoritative. The runner resumed from the already-proven post-reset state without repeating factory reset.

These events did not establish a firmware defect. They were corrected only in temporary tooling and were not hidden from acceptance evidence.

## 15. Safety and Fault Audit

- No Start, setpoint, reverse, Stop-as-test, Recover-as-test, E-stop-as-test, or other motion command was issued.
- Every observed lifecycle state had zero applied and generated speed.
- No panic, watchdog, brownout, assertion, stack-canary failure, heap corruption, reset loop, task leak, or unexpected reset was observed.
- COM5 was closed and released after validation.
- No credential, authorization header, Wi-Fi password, portal password, or raw secret-bearing request body was written to repository or evidence.

## 16. Final Acceptance

| Gate | Result |
|---|---|
| Identity-first process | PASS |
| Host and source validation | PASS |
| ESP-IDF v5.5.3 full-clean no-ccache build | PASS |
| Flash and chip verification | PASS |
| Three pure-suite invocations | PASS - 1,593/1,593 |
| Same-SSID password preservation | PASS |
| Identity lock before reset | PASS |
| Coordinated restart | PASS |
| Local Service entry/exit | PASS |
| Production UI factory reset | PASS |
| Truthful uncommissioned recovery | PASS |
| Portal-password preservation | PASS |
| Exact identity restoration and relock | PASS |
| Exact Wi-Fi restoration | PASS |
| Final service round trip | PASS |
| Phase 4B.6 | FULLY ACCEPTED |
| Phase 4B | COMPLETE AND ACCEPTED |

The remaining product work begins with Phase 4C. Phase 4D security hardening and unified portal-credential recovery remain deferred, and the physical/process exclusions in Section 1 remain in force.
