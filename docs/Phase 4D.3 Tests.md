# Phase 4D.3 Chronological Test Record

**Status:** COMPLETE AND ACCEPTED

**Branch:** `phase4d3-browser-auth-and-administration`

**Firmware identity:** `v2-phase4d.3-dev`

## 1. Starting Repository State

Phase 4D.3 began from clean, synchronized `main` at `5cbfbbfaa513567a66e97919e55f2e9c7158566a`. The annotated `v2-phase4d.2-security-foundation-accepted` tag peels to that commit. The accepted Phase 4D.1 and Phase 4C tags remain ancestors. No unexplained user changes were present.

The feature branch was created as `phase4d3-browser-auth-and-administration`. Identity was established first in `9d5201a` (`chore: establish Phase 4D.3 identity`) and pushed before functional implementation.

## 2. Contract Reconciliation

The pre-implementation audit found two accepted Phase 4D.2 facts that the Phase 4D.1 contract still described as future work: encrypted `sec_store` is active with software-stored, physically extractable XTS keys, and GPIO0/KEY1 is the physically validated recovery input. The contract was corrected without weakening its physical-extraction limitation.

The accepted Client Admin amendment makes `manage_client_admins` the twenty-third stable capability. Session capacity remains the contract-defined eight total and two per account.

## 3. Route and Capability Inventory

The complete existing route inventory, SoftAP-only classification, migration disposition, administration routes, and command capability matrix are recorded in `PHASE_4D_3_IMPLEMENTATION_PLAN.md`. Every human route is deny-by-default and must prove both SoftAP local destination and SoftAP peer before parsing sensitive bodies or entering KDF work.

## 4. Implementation Chronology

- `argus_auth_service` implements strict login decoding, canonical lookup, synthetic unknown-principal verification, pre-KDF admission, generic failure behavior, and bounded throttling.
- `argus_session_manager` implements eight RAM-only sessions, two per principal, 256-bit random session and CSRF secrets, 15-minute idle expiry, eight-hour absolute expiry, five-minute recent reauthentication, and explicit revocation.
- `argus_http_security` proves the accepted SoftAP socket destination and peer before sensitive work, applies security headers, parses the `HttpOnly; SameSite=Strict` cookie, and enforces Origin, Host, content type, and CSRF.
- `argus_authorization` centralizes deny-by-default capability and target-ceiling decisions. Authentication does not acquire operating authority.
- `argus_security_directory` and `argus_security_admin` implement bounded human-account and custom-role storage, transactional readback, Client Admin delegation ceilings, password replacement, and affected-session revocation.
- `argus_security_audit` implements a 255-record persistent ring with a bounded eight-entry queue, redacted reads, wrap metadata, and nonblocking failure behavior.
- `/login`, `/operate`, and `/commission` replaced the legacy Basic-Auth browser flow. `Authorization: Basic` no longer authenticates and no `WWW-Authenticate` challenge is emitted.
- Active AP-password administration changes only the active SoftAP secret after current-secret confirmation, recent reauthentication, CSRF, capability checks, atomic commit/readback, and controlled AP restart. The factory recovery secret is not writable through the browser.
- Authenticated recovery exit clears only the recovery marker and performs a controlled reboot. Identity, STA configuration, users, roles, credentials, machine state, and command architecture are preserved.

## 5. Failures, Diagnosis, and Corrections

1. The first live SoftAP login was rejected because accepted ESP-IDF IPv4 sockets were reported as IPv4-mapped IPv6. The guard was corrected to normalize native IPv4 and IPv4-mapped IPv6 while rejecting native IPv6 and ambiguous addresses. Regression vectors were added without weakening the AP-only decision.
2. A real login then exposed a `StoreProhibited` panic. The exact matching ELF (`df27048bd`) and disassembly showed that `login_post_handler` plus `argus_auth_service_authenticate` exceeded the 6,144-byte HTTP-task stack because a 4,392-byte directory snapshot was automatic storage. Directory snapshots in login, account, role, and administration paths were moved to bounded heap allocations with explicit zeroization and release. Final live login high-water margins remained positive; the minimum observed after repeated administration use was 972 bytes.
3. Existing host browser models still assumed Basic Auth and the prior static portal. They were converted to session bootstrap, CSRF, protected-route, logout, commission, and factory-reset behavior. A dedicated Phase 4D.3 browser contract test was added.
4. One controller-suite invocation completed all 711 assertions but correctly failed production isolation because an absent upstream AP changed asynchronous STA retry bookkeeping during the run. The evidence was retained as production churn. The controller was placed in the accepted stationary `SERVICE_AP_ONLY / DIAGNOSTIC_CLI` state and the three final invocations passed with unchanged production state.
5. Early no-ccache build attempts used a losing CMake override while `idf.py` appended `CCACHE_ENABLE=1`. The final build used ESP-IDF's authoritative `idf.py --no-ccache` option and CMake recorded `-DCCACHE_ENABLE=0`.

## 6. Host and Pure Validation

- `node tools/test_phase4b5_controls.mjs`: PASS
- `node tools/test_phase4b6_portal.mjs`: PASS
- `node tools/test_phase4d3_browser.mjs`: PASS
- Three final genuine Windows ConPTY-backed controller invocations passed.
- Each invocation registered 237 distinct tests, ran three internal repeat passes, and completed 711/711 with zero failures.
- Final controller total: 2,133/2,133 executions.
- Each final invocation preserved authority generation 14, `SERVICE_AP_ONLY`, broker `STOPPED`, machine `UNLOCKED`, and 21 tasks.
- No panic, reset, watchdog, brownout, assertion, stack-canary failure, heap corruption, task leak, motor movement, or production-state contamination occurred.

## 7. Browser and JavaScript Validation

- The final image served working `/login`, `/operate`, and `/commission` pages. Authenticated refresh and normal page/API use remained valid and capability-shaped.
- Unauthenticated session access returned bounded `401 authentication_required`; protected pages redirected to `/login`.
- A legacy Basic header remained unauthenticated, returned no challenge, and did not enable a compatibility path.
- Explicit logout invalidated the session and returned protected navigation to login.
- Six fabricated-principal attempts produced generic authentication failures followed by the expected bounded "Too many attempts" response. After the 30-second cooldown, real login succeeded.
- A session-bearing same-origin POST without the CSRF header returned `403 request_protection_failed`.
- Active AP-password change revoked the browser session. The temporary credential worked after AP restart, the original credential was restored, and a second fresh login succeeded.
- No password, AP secret, session token, CSRF token, Authorization value, or raw protected body entered this record or repository evidence.

## 8. MQTT and Command-Boundary Regression

- Phase 4C MQTT startup, topic root, broker lifecycle, and authority acquisition remained intact after normal reboot.
- Exactly one production MQTT call site dispatches through `argus_cmd_router_dispatch()`.
- The browser retains one production command-adapter dispatch site; authentication and administration code has none.
- Security modules contain no direct state-manager mutation, trajectory, step-generator, motor-GPIO, or authority-acquisition call.
- Authentication, throttling, session, audit, AP-password, and recovery workflows generated zero browser or MQTT command dispatches.

## 9. Build Evidence

- Toolchain: ESP-IDF v5.5.3.
- Command: `idf.py --no-ccache fullclean` followed by `idf.py --no-ccache build` and `idf.py --no-ccache size`.
- CMake evidence: `-DCCACHE_ENABLE=0`.
- Compiler warnings: 0.
- Compiler errors: 0.
- Application binary: `0x1207b0` bytes (1,181,616-byte file; size tool total 1,181,501 bytes).
- OTA headroom: `0x1df850` bytes (1,964,112 bytes, 62%).
- DIRAM: 189,903 bytes used and 151,857 bytes remaining. `.bss` is 85,136 bytes and `.data` is 20,156 bytes.
- The RAM-only session table occupies 2,240 static bytes.
- The final flashed ELF SHA-256 prefix is `038dcac19`.

## 10. Hardware Validation

- COM5 identified the intended ESP32-S3 QFN56 revision 0.2, MAC `3c:dc:75:6e:c2:d0`, with the motor physically absent.
- The final no-ccache image booted as `v2-phase4d.3-dev` with healthy encrypted security and audit storage, preserved customer identity, preserved STA configuration, zero setpoint, `UNLOCKED`, and driver disabled.
- SoftAP login succeeded and all authorized pages were usable. The login path remained stable through repeated login, logout, reauthentication, AP restart, credential restoration, and recovery.
- From the commissioned STA address `192.168.1.22`, `/login` and `/api/auth/session` returned bounded `403 local_ap_required`, while the same routes worked through SoftAP `192.168.4.1`.
- Active AP password changed to a user-local temporary value and back. Both reconnects succeeded, each change required a fresh login, and the pre-test active credential was verified after restoration.
- A physical 12-second BOOT/GPIO0 hold entered `SECURITY_RECOVERY_AP_ONLY`, stopped STA and MQTT, and left machine and authority state untouched.
- The recovery page truthfully showed recovery active. Authenticated, recently reauthenticated recovery exit was accepted, rebooted cleanly, restored `AP_DISCOVERABLE`, rejoined the commissioned STA, restarted MQTT, and showed recovery inactive.
- Final normal-mode login succeeded with the restored active AP credential. No powered-motion command was issued.

No powered-motion command is authorized by this test record.

## 11. Credential-Safe Human Interaction

Any real credential is entered only through a local browser or non-echoing terminal prompt. No real credential may appear in chat, command-line arguments, tracked files, logs, screenshots, or this record.

## 12. Final Repository and Device Disposition

Phase 4D.3 is accepted against the commits and annotated tag recorded at final source-control closeout. The controller runs the accepted image, commissioned identity and STA configuration are unchanged, the active AP credential is restored, the factory AP credential and console verifier are unchanged, no synthetic account or role remains, and no irreversible device-security action occurred.

The controller finished in normal commissioned operation: `AP_DISCOVERABLE`, STA connected, MQTT running, machine `UNLOCKED`, zero setpoint, and no Phase 4D.3 browser session intentionally retained. COM5 and all local test processes are released during final disposition.

## 13. Deferred Phase 4D.4 Work

Machine enrollment, MQTT CONNECT authentication, HTTPS, TLS, certificates, hostile-network acceptance, and irreversible device-security provisioning remain deferred.

## 14. Subsequent Phase 4D.3a Correction

This accepted Phase 4D.3 record is historical and remains unchanged in its conclusions. A later source review identified four narrow acceptance-contract discrepancies: prepared audit records used success semantics, AP/recovery transitions were armed before final response completion, retired `/api/logout` was still registered, and route inventory/audit pagination were incomplete. Phase 4D.3a corrected and independently validated those items without implying that an earlier failed candidate passed. See `PHASE_4D_3A_IMPLEMENTATION_PLAN.md` and `Phase 4D.3a Tests.md`.
