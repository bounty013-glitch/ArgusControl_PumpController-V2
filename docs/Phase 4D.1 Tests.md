# Phase 4D.1 Tests and Audit Record

**Date:** July 22, 2026

**Subphase:** Phase 4D.1 - Security Contract, Permission Model, and Baseline Audit

**Firmware identity:** `v2-phase4d.1-dev`

**Result:** ACCEPTED as a contract and repository-audit subphase. No credential, session, authorization, MQTT-authentication, recovery, or transport-security behavior was implemented.

## 1. Starting Baseline

- Starting branch: `main`
- Starting local and remote commit: `0a9f5f9c6ad48d66282c1a5df2bda4c0c51c1141`
- Accepted tag: annotated `v2-phase4c-mqtt-hardware-verified`, peeled to the starting commit
- Upstream: `origin/main`, synchronized
- Worktree: clean
- Created branch: `phase4d1-security-contract-and-boundaries`
- No unexplained user changes were present.

The accepted Phase 4C history was preserved without amendment, squash, or rewrite.

## 2. Identity-First Change

The first source change established `v2-phase4d.1-dev` through the existing authoritative identity mechanism:

- root `PROJECT_VER`
- truthful fallback identity
- firmware-starting and startup-completed phase labels
- pure-suite phase labels

Identity commit: `c24d7e6` (`chore: establish Phase 4D.1 identity`). No runtime security behavior changed.

The authoritative contract and implementation plan were then committed as `3fe038d` (`docs: define Phase 4D security contract`).

## 3. Repository Audit Method

The audit used bounded source and history searches, direct review of HTTP, network, identity, portal, NVS, MQTT, authority, command-router, state-manager, reset, build configuration, partition, and test-registration code, and ESP-IDF v5.5.3 header/configuration inspection. Searches covered:

- route registration and handler authentication;
- HTTP listener/interface behavior;
- commissioning, Local Service, browser authority, and reset paths;
- all production command-router dispatch sites and direct motion/state calls;
- NVS namespaces, fields, initialization, Wi-Fi storage, and partition capacity;
- portal, AP, STA, MQTT, and build-time credential handling;
- cookies, sessions, CSRF, Origin checks, rate limits, lockouts, and audit storage;
- MQTT CONNECT metadata, topic ownership, connection identity, and Phase 4C admission;
- secure boot, flash encryption, and NVS encryption configuration;
- tracked source, current diff, and history for high-risk credential and private-key patterns.

No discovered credential value was printed or copied into this record.

## 4. HTTP, AP, and STA Findings

- The ESP-IDF HTTP server listens on plain TCP port 80 using the default all-interface binding.
- While commissioned APSTA operation is active, the current application HTTP surface is reachable through both AP and STA.
- Seventeen application routes are registered. They cover status, identity, portal pages, portal-password replacement, configuration, Wi-Fi reconnect, restart, Local Service entry/exit, browser commands, and configuration factory reset.
- Application handlers use one HTTP Basic Authentication check; logout is implemented through another authentication challenge.
- There are no cookie sessions, role or permission enforcement, CSRF tokens, Origin/Host policy, authentication throttles, bounded lockouts, administrative session revocation, or durable security audit records.
- The Phase 4D contract therefore restricts all human browser traffic to the controller AP, with every credential, user, role, permission, identity, recovery, firmware, network, and commissioning mutation AP-only.

Plain HTTP does not protect credentials or sessions from an attacker already present on the AP. AP membership is necessary but is not authentication or authorization.

## 5. Commissioning, Authority, and Recovery Findings

- A healthy uncommissioned controller can be commissioned using power, the local AP, and a browser. Serial is not required for normal commissioning.
- The existing commissioning surface will become `/commission`; `/login` and `/operate` are the other defined logical browser boundaries.
- Browser login must not acquire operating authority. Authority acquisition must not issue a machine command.
- Backup operator interfaces must authenticate, receive permissions, read a coherent snapshot, explicitly request authority, and continue through the existing authority manager, command router, and state manager.
- Current STA reconnect is not AP-access recovery.
- Configuration factory reset erases commissioned identity and STA configuration but intentionally preserves portal authentication.
- Serial factory reset and development flash/NVS tools are disaster-recovery mechanisms, not the normal field workflow.
- The repository defines only motion-output GPIOs and does not establish a dedicated physical recovery input. Selection, debounce, local-presence proof, and hardware validation of a recovery trigger remain a required decision before Phase 4D.2.

No recovery action, browser takeover, credential entry, configuration mutation, or motor command was performed.

## 6. Credential and Storage Findings

- The AP join credential and Argus console credential are separate domains even if product policy initially assigns the same human-readable value.
- The AP secret must be recoverable by the Wi-Fi owner; the console credential must use a salted one-way verifier and must not require recoverable plaintext.
- The existing replacement console password is plaintext in a dedicated NVS namespace.
- Commissioned identity and STA configuration, including the STA credential, use dual configuration slots in the existing NVS partition.
- The AP secret comes from untracked local build configuration and is compiled into the image.
- ESP-IDF Wi-Fi NVS storage is enabled and the application does not explicitly select RAM-only Wi-Fi storage, so additional runtime Wi-Fi records may exist.
- The partition table provides one 24 KiB NVS partition and no `nvs_keys` partition.
- Existing NVS initialization can erase the complete NVS partition for selected initialization failures. Later protected security storage must fail closed without unrelated or fleet-bootstrap erasure.
- Browser sessions are specified as bounded RAM-only records. Human verifiers, recoverable AP material, machine credentials, metadata, and audit records have separate storage classes.

ESP-IDF v5.5.3 supplies PBKDF2-HMAC, SHA-256, runtime random generation, constant-time comparison, and NVS encryption/HMAC facilities. The contract selects measured PBKDF2-HMAC-SHA-256 parameters, a unique salt of at least 16 bytes, a 32-byte verifier, and constant-time comparison. The exact iteration count must be benchmarked on the target for a 250-500 ms verification budget before implementation.

## 7. Device-Protection Findings

- Secure boot: not enabled
- Flash encryption: not enabled
- NVS encryption: not enabled
- NVS key partition: absent

Physical flash extraction is therefore not claimed to be mitigated. No eFuse mutation or destructive device-protection experiment was performed.

## 8. MQTT Security Findings

- The embedded broker listens on plaintext port 1883 on all active interfaces.
- CONNECT parsing accepts a client ID and parses but does not authenticate supplied username/password data.
- Subscriptions are not permission-filtered by an authenticated machine identity.
- Phase 4C exact topic ownership, non-reusable connection identity, broker command session, heartbeat lease, sequence/replay checks, duplicate handling, command results, and one router path remain intact.
- These mechanisms provide freshness and deterministic ownership, not cryptographic publisher authentication.
- Exactly one production MQTT call to `argus_cmd_router_dispatch()` remains, in `argus_mqtt_runtime.c`.
- No production legacy `argus/peristaltic/cmd/...` command path exists.
- Twenty Phase 4C tests remain registered, and the complete suite still registers 197 distinct tests.

Later MQTT machine credentials must be independently revocable and enforce per-client publish and subscribe permissions before Phase 4C admission. Without TLS, MQTT credentials and payloads are exposed to an attacker on the assumed trusted local network.

## 9. Accepted Roles and Permissions

The authoritative contract defines six distinct levels: Argus Personnel, Client Admin, Supervisor, Operator, Viewer, and Machine Identity. Its 22-row matrix covers operating visibility and control, E-stop operations, alarm acknowledgement, user/role/machine administration, audit access, network and AP credential management, client and MQTT configuration, protected identity/configuration, commissioning, calibration, firmware, recovery, and full security reset.

Authorization is deny-by-default. Argus Personnel is the protected product ceiling. Client Admin can administer only below that ceiling and delegate only permissions both possessed and marked delegable. Custom roles cannot exceed their creator's ceiling. Machine identities are independently enrolled, scoped, audited, revoked, and replaced; they are not human accounts.

Authentication, authorization, and operating authority remain separate decisions. UI visibility never grants a permission, and all protected operations must enforce authorization in firmware at their operation boundary.

## 10. Browser, AP Password, and Audit Contracts

- Browser sessions use an unpredictable 256-bit bearer value in a bounded RAM table, an `HttpOnly` and `SameSite=Strict` cookie, no URL or localStorage token, a 15-minute idle limit, an 8-hour absolute limit, and explicit/relevant-change revocation.
- The `Secure` cookie attribute cannot be truthfully used over this release's HTTP transport.
- Browser mutation requires a synchronizer token header, same-origin Origin/Host checks, exact content type, session authorization, and applicable recent reauthentication.
- Authentication throttling is bounded: five failures in 60 seconds, starting with a 30-second delay and escalating to at most five minutes without permanent field lockout.
- AP-password change requires Manage Network Access, recent reauthentication, current-credential confirmation, validated new-entry confirmation, protected recoverable storage, affected-session revocation, bounded network restart, and audit without either secret.
- AP password, web-account password, Argus console credential, network recovery, and full security reset are distinct operations.
- Audit uses a bounded durable ring and never stores reusable credentials. Until trustworthy wall time exists, records use truthful boot/session-relative time with synchronized wall time only when actually available.

## 11. Host and Static Validation

| Validation | Result |
|---|---|
| Phase 4B.5 browser-control JavaScript tests | PASS |
| Phase 4B.6 portal JavaScript tests | PASS |
| Phase 4C contract/source-boundary audit | PASS |
| Exactly one production MQTT router dispatch | PASS |
| No new direct motion path | PASS |
| No legacy command path restored | PASS |
| Accepted Phase 4C registrations preserved | PASS: 20 Phase 4C, 197 total |
| Documentation consistency checks | PASS |
| Sanitized tracked/history/diff secret-pattern audit | PASS: no high-risk tracked secret or private key found |
| `git diff --check` | PASS |

The active untracked build configuration contains expected nonempty secret fields. It remained untracked; no value was printed, copied, or committed.

## 12. Full-Clean ESP-IDF Build

Environment and commands:

```powershell
. C:\esp\v5.5.3\esp-idf\export.ps1
idf.py --version
idf.py fullclean
$env:CCACHE_ENABLE = "0"
idf.py --no-ccache reconfigure
ninja -C build -j 1 all
idf.py size
```

Results:

- ESP-IDF: v5.5.3
- Compiler warnings: 0
- Build errors: 0
- Application binary: 1,107,536 bytes (`0x10e650`)
- OTA slot: 3,145,728 bytes (`0x300000`)
- OTA headroom: 2,038,192 bytes (`0x1f19b0`, 65%)
- Binary SHA-256: `e9870ad20e56c9ec8da1c3514938f23d39fa8f5cef6cea240e519080e28abc76`
- Static memory: 180,583 bytes used, 161,177 bytes free
- Identity-only binary-size change from accepted Phase 4C: 0 bytes
- Identity-only static-RAM change from accepted Phase 4C: 0 bytes

The Phase 4D.1 image was built but was not flashed. Compilation was not counted as runtime test execution.

## 13. Controller Pure-Suite Attempts

The accepted Phase 4C firmware remained on the controller. Two non-motion invocations were attempted through genuine Windows ConPTY-backed `idf.py -p COM5 monitor`; no fabricated terminal responses, browser takeover, credential entry, flash, or motion input was used.

### Attempt 1

- Registered distinct tests: 197
- Internal repeat passes: 3
- Test executions: 591 passed, 0 failed
- Final suite disposition: FAIL, correctly, because an actual STA disconnect/retry changed production network state, disconnect evidence, retry-timer state, and authority generation while the suite was running.

### Attempt 2

After a 30-second boot-settle delay and status snapshot:

- Registered distinct tests: 197
- Internal repeat passes: 3
- Test executions: 591 passed, 0 failed
- Final suite disposition: FAIL, correctly, because the real STA retry timer completed during the suite, changing STA and retry-timer state.

The controller reported the commissioned STA unavailable and remained in its genuine reconnect lifecycle with the broker stopped. Because each retry window could mutate production state, a third speculative invocation could not produce valid isolation evidence without changing network conditions, entering Local Service, supplying credentials, altering configuration, or flashing. Those actions were outside this subphase. The conditional three-invocation validation was therefore excluded after two truthful failed-isolation attempts; no controller-suite PASS is claimed.

Both attempts showed zero test assertion failures, no motion, `UNLOCKED` machine state, and zero generated/applied output. The failures were environmental production-isolation failures, not weakened or ignored tests. Every failure and diagnosis is preserved here.

## 14. Documentation and Disposition

Created:

- `docs/PHASE_4D_SECURITY_CONTRACT.md`
- `docs/PHASE_4D_1_IMPLEMENTATION_PLAN.md`
- `docs/Phase 4D.1 Tests.md`

Updated only where needed:

- `README.md`
- `docs/V2_CONTROLLER_ARCHITECTURE.md`
- `docs/V2_IMPLEMENTATION_PLAN.md`
- `docs/DEFERRED_HARDENING_REGISTER.md`
- `docs/MQTT_STANDARDS.md`

Phase 4D.1 accepts an implementation-ready security contract, permission model, and sanitized baseline audit. It does not accept credential implementation, HTTPS, MQTT TLS, certificates, hostile-network operation, physical extraction resistance, denial-of-service resistance, recovery hardware, penetration testing, powered motion, pump/process behavior, or any Phase 4D.2 implementation.

The final audit, validation, and living-document reconciliation is recorded by the repository commit named `docs: record Phase 4D.1 audit and validation`; its immutable hash is reported from Git after commit creation.
