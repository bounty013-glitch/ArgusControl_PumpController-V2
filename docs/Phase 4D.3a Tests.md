# Phase 4D.3a Chronological Test Record

**Status:** FULLY ACCEPTED (July 23, 2026)

**Branch:** `phase4d3a-security-acceptance-corrections`

**Firmware identity:** `v2-phase4d.3a-dev`

## 1. Starting Repository State

The exact repository was clean and synchronized at accepted `main` commit `c765825f33f7dbeb1538f256a564ae3901b330cc`. Annotated tag object `5ea00c87f12efa90609f70b721f9138edee18dcb` for `v2-phase4d.3-browser-access-control-accepted` peeled to that commit. Accepted Phase 4D.2 and Phase 4D.3 history was ancestral. No untracked credential, build, test, or device artifact was present.

The branch `phase4d3a-security-acceptance-corrections` was created from that baseline. Identity was established and pushed first in `3936497` (`chore: establish Phase 4D.3a identity`).

## 2. Source-Review Findings

1. Account, role, self-password, AP-secret, and recovery-exit flows wrote `SUCCESS` records with `_reserved` reasons before mutation.
2. AP-secret apply and recovery reboot timers were started before the final response function.
3. Retired `GET /api/logout` remained registered and could reach its 405 handler through STA.
4. The route proof covered only eight security routes and audit GET always returned only the newest 16 records.

## 3. Correction Chronology

- The 168-byte audit record retained schema 1 and repurposed its prior reserved field as a lifecycle identifier.
- `PREPARED` was added as a non-success outcome.
- Required mutation begin/finish APIs now persist prepared then correlated terminal success/failure.
- Terminal-write failure leaves prepared evidence and latches privileged audit finalization degraded.
- Account, role, password, session-revocation, AP-secret, and recovery-exit handlers use the lifecycle.
- A deferred-transition seam now proves response completion before timer arm and one-shot callback claim.
- `GET /api/logout` was removed from the handler and registration tables.
- Production route registrations were consolidated into explicit main and security arrays.
- A 32-entry policy inventory covers 31 active routes and one retired alias.
- Audit GET now implements strict `limit`/`before` parsing and newest-first cursor pages.
- Six focused Phase 4D.3a tests increased the registered suite from 237 to 243 distinct tests.
- Functional and regression source was committed and pushed in `e9b1968` (`fix: close Phase 4D.3 security acceptance gaps`).

## 4. Failures and Corrections

1. The first incremental build exposed `httpd_method_t` through `argus_http_server.h` without including its ESP-IDF declaration. The header boundary was corrected by including `esp_http_server.h`; the next build passed.
2. The first inventory proof used equal counts and one-way membership. Review found that a duplicate registration paired with an omission could evade that shape. The test was strengthened to require bidirectional membership and exactly one registration for every active path/method pair.

No security or machine boundary was weakened to correct either issue.

## 5. Host and Source Validation

- `node tools/test_phase4b5_controls.mjs`: PASS
- `node tools/test_phase4b6_portal.mjs`: PASS
- `node tools/test_phase4d3_browser.mjs`: PASS
- Incremental ESP-IDF no-ccache build: PASS
- `git diff --check`: PASS
- Retired `GET /api/logout` registration: absent
- Browser adapter router dispatch sites: one
- Production MQTT router dispatch sites: one
- Security-administration router dispatch sites: zero
- Basic Auth implementation/challenge path: absent
- Security administration direct motion/state/trajectory/GPIO/authority acquisition: absent

## 6. Authoritative Build

- ESP-IDF: v5.5.3
- Commands: `idf.py --no-ccache fullclean`, `idf.py --no-ccache build`, `idf.py --no-ccache size`
- Result: PASS
- Compiler warnings: 0
- Compiler errors: 0
- Binary: `0x122b00` bytes (1,190,656-byte file)
- Size-tool image total: 1,190,537 bytes
- OTA headroom: `0x1dd500` bytes (62%)
- DIRAM: 189,911 used; 151,849 free
- `.bss`: 85,144 bytes
- `.data`: 20,156 bytes
- Partition overlap: none

## 7. Audit Measurements

- Record: 168 bytes
- Metadata: 40 bytes
- Request object: 264 bytes
- Mutation context: 124 bytes
- Maximum page object: 2,704 bytes
- Retained events: 255 in 256 physical slots
- Partition: 262,144 bytes
- Queue: 8 pointers
- Worker stack: 5,120 bytes
- Static DIRAM delta from Phase 4D.3: +8 bytes

## 8. Hardware Validation

COM5 identified the expected ESP32-S3 QFN56 revision 0.2 controller with MAC ending `6e:c2:d0`. The installed accepted Phase 4D.3 image was confirmed before flash. The motor was physically absent and incapable of powered motion.

The Phase 4D.3a image booted as `v2-phase4d.3a-dev` with healthy encrypted security and audit storage, preserved customer/device identity, preserved STA configuration, `AP_DISCOVERABLE`, `SUPERVISORY/MQTT`, `UNLOCKED`, zero target/output, and disabled driver.

Three genuine interactive controller invocations passed:

| Invocation | Distinct | Internal passes | Executed | Passed | Failed |
|---|---:|---:|---:|---:|---:|
| 1 | 243 | 3 | 729 | 729 | 0 |
| 2 | 243 | 3 | 729 | 729 | 0 |
| 3 | 243 | 3 | 729 | 729 | 0 |
| **Total** | **243** | **9** | **2,187** | **2,187** | **0** |

Each invocation preserved authority generation 3, `AP_DISCOVERABLE`, MQTT `RUNNING`, machine `UNLOCKED`, and task count 22. No panic, reset, watchdog, brownout, assertion, stack-canary failure, heap corruption, task leak, production-state contamination, or motor motion occurred.

## 9. Live Browser and Interface Acceptance

- STA `/login` and `/api/auth/session` returned `403 local_ap_required` with no-store security headers.
- STA retired `/api/logout` returned 404 because the alias is unregistered.
- SoftAP login and recent reauthentication succeeded without disclosing credentials.
- Canonical logout returned to the login page without error or reboot.
- A read-only local probe passed two stable newest-first audit pages, exclusive cursor behavior, no cross-page duplicate, strict rejection of zero/overflow/duplicate/unknown parameters, Basic Auth rejection without a challenge, retired-route 404, and no-store caching.
- The live result was `PHASE_4D_3A_LOCAL_READONLY_PASS`; its first page returned four records with `has_more=true`, and the second bounded page returned successfully.

## 10. Live Audit Lifecycle Acceptance

Two active AP-secret changes and one recovery-exit mutation generated live audit evidence. A read-only probe examined 64 records and returned `PHASE_4D_3A_AUDIT_LIFECYCLE_PASS`.

The probe confirmed:

- at least two correlated `change_ap_secret_prepared` outcome-4 records with terminal `change_ap_secret_succeeded` outcome-1 records;
- at least one correlated `recovery_exit_prepared` outcome-4 record with terminal `recovery_exit_succeeded` outcome-1 record;
- no `_prepared` record encoded as success.

Failure-injection coverage separately proved that mutation failure produces terminal failure, failed prepared reservation prevents mutation, terminal-write failure leaves detectable prepared evidence and latches later privileged mutation refusal, and no audit failure reaches motion or authority code.

## 11. AP Credential Transition Acceptance

The user entered all AP credentials only in the local browser.

1. The original active AP credential authenticated the change.
2. The browser displayed `Accepted. Reconnect to the SoftAP with the new password.` before disconnection.
3. Serial observation then showed `Committed active AP credential applied` and SoftAP/DHCP restart activity.
4. The temporary credential connected successfully.
5. The same response-before-disconnection behavior occurred while restoring the original credential.
6. The original credential reconnected successfully and the controller remained healthy.

The factory AP credential, console verifier, customer identity, and STA configuration were unchanged. Neither credential value entered chat, source, command arguments, Git, logs, screenshots, or documentation.

## 12. Recovery Exit Acceptance

A physical 12-second BOOT/GPIO0 hold entered persistent `SECURITY_RECOVERY_AP_ONLY`. The browser truthfully showed recovery active while authority remained `SUPERVISORY/MQTT`; no machine or authority transition occurred.

Authenticated exit displayed `Recovery exit accepted` before disconnection. The asynchronous reboot completed cleanly and restored:

- firmware `v2-phase4d.3a-dev`;
- network mode `AP_DISCOVERABLE`;
- authority `SUPERVISORY/MQTT`;
- recovery mode inactive;
- preserved commissioned identity and STA configuration;
- zero motor motion and no stale security transition.

Credential-free screenshots are retained:

- [Security recovery mode](Evidence%20Screen%20Shots/Phase%204D.3a/Phase%204D.3a%20Security%20Recovery%20Mode.png)
- [Recovery active](Evidence%20Screen%20Shots/Phase%204D.3a/Phase%204D.3a%20Recovery%20Active.png)
- [Recovery exit accepted](Evidence%20Screen%20Shots/Phase%204D.3a/Phase%204D.3a%20Recovery%20Exit%20Accepted.png)
- [Normal commissioned mode](Evidence%20Screen%20Shots/Phase%204D.3a/Phase%204D.3a%20Normal%20Commissioned%20Mode.png)
- [Recovery inactive](Evidence%20Screen%20Shots/Phase%204D.3a/Phase%204D.3a%20Recovery%20Inactive.png)

## 13. Credential Safety

Any real credential is entered only by the user through the local browser or a non-echoing local terminal. No credential may enter chat, command arguments, tracked files, logs, screenshots, or this record.

## 14. Final Repository and Controller Disposition

Phase 4D.3a is accepted against implementation commit `e9b1968` and the identity-first commit `3936497`, with final documentation, merge, and annotated-tag identifiers recorded by source-control closeout.

The controller remains on the accepted Phase 4D.3a image in normal commissioned operation. Customer/device identity, STA configuration, factory AP credential, console verifier, calibration, machine-client records, machine state, target, E-stop/fault state, and operating-authority policy are unchanged. The original active AP credential is restored and verified. No synthetic account or role remains. The final browser session was logged out, the laptop returned to `Paladin6661`, COM5 was released, and temporary serial helpers were removed.

Phase 4D.4 has not started. No powered-motion, pump, hose, fluid, chemical, pressure, flow, process, endurance, MQTT authentication, TLS, certificate, secure-boot, flash-encryption, eFuse, UART, or JTAG acceptance is implied.
