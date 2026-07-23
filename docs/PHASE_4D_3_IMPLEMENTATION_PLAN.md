# Phase 4D.3 Implementation Plan

**Status:** COMPLETE AND ACCEPTED

**Branch:** `phase4d3-browser-auth-and-administration`

**Accepted baseline:** `5cbfbbfaa513567a66e97919e55f2e9c7158566a`

**Firmware identity:** `v2-phase4d.3-dev`

**Identity-first commit:** `9d5201a` (`chore: establish Phase 4D.3 identity`)

## 1. Scope and Safety Boundary

Phase 4D.3 replaces legacy browser Basic Auth with AP-only human login, bounded RAM-only sessions, CSRF, deny-by-default authorization, human-account and custom-role administration, security audit, protected active AP-password administration, and authenticated exit from `SECURITY_RECOVERY_AP_ONLY`.

This phase does not implement machine-client enrollment, MQTT CONNECT authentication, HTTPS, TLS, certificates, hostile-network acceptance, firmware-management behavior, calibration, or irreversible device protection. It does not add a motion path. No Phase 4D.3 test may issue Start, a nonzero setpoint, Recover as a motion test, or any other powered-motion command.

Authentication, authorization, and operating authority remain separate. The only browser command path remains:

`HTTP handler -> session authentication -> capability authorization -> existing browser command adapter -> argus_cmd_router_dispatch() -> authority manager -> state manager`

## 2. Accepted Foundations

- Phase 4D.2 encrypted `sec_store`, `sec_keys`, and `sec_audit` partitions remain authoritative.
- The console verifier remains distinct from human-account verifiers.
- GPIO0/KEY1 remains the accepted physically local recovery trigger.
- The accepted permission matrix contains 23 capabilities, including the protected `manage_client_admins` amendment.
- Session capacity is eight total and two per account, as fixed by the accepted contract.
- Phase 4C MQTT command session, sequencing, heartbeat, fail-operational behavior, and single router-dispatch path remain unchanged.

## 3. Existing Route Inventory and Disposition

All human pages and browser APIs are SoftAP-only. A route fails before sensitive body parsing or KDF work unless the accepted socket is proven to use the SoftAP local destination and the peer is on the SoftAP subnet.

| Current route | Method | Phase 4D.3 disposition | Required capability |
|---|---|---|---|
| `/` | GET | Public AP-only redirect to `/login` or `/operate` | None |
| `/login` | GET | Public AP-only login page | None |
| `/api/auth/login` | POST | Public AP-only bounded login | None |
| `/api/auth/session` | GET | Authenticated session/capability summary | Valid session |
| `/api/auth/logout` | POST | Replace mutation-by-GET logout | Valid session + CSRF |
| `/operate` | GET | Authenticated operating page | `view_status` |
| `/controls` | GET | Redirect migration alias; no parallel auth path | `view_status` |
| `/commission` | GET | Permission-shaped administration page | Valid session plus at least one administration capability |
| `/api/status` | GET | Authenticated operating state | `view_status` |
| `/api/identity` | GET | Authenticated identity state | `view_status` |
| `/api/service/enter` | POST | Existing service-entry path | `request_authority` + CSRF |
| `/api/service/exit` | POST | Existing service-exit path | `request_authority` + CSRF |
| `/api/command` | POST | Existing command decoder/router path | Per-command capability + CSRF |
| `/api/config` | GET | Permission-filtered configuration | Applicable configuration capability |
| `/api/config/save` | POST | Existing bounded configuration mutation | Per-field capability + CSRF |
| `/config/identity` | GET | Redirect to commission identity panel | `modify_identity` |
| `/config/wifi` | GET | Redirect to commission network panel | `manage_client_network` |
| `/api/network/reconnect` | POST | Existing reconnect operation | `manage_client_network` + CSRF |
| `/api/restart` | POST | Existing controlled restart | `manage_firmware` + CSRF |
| `/api/factory-reset` | POST | Existing configuration reset, security preserved | `commission` + recent reauthentication + CSRF |
| `/change-password` | GET | Redirect to authenticated account/security panel | Valid session |
| `/api/portal-password` | POST | Retired; replaced by scoped account/AP APIs | Denied |
| `/api/logout` | GET | Retired mutation-by-GET route | Denied |
| `/api/security/accounts` | GET/POST | Bounded account list/create | `manage_users` and target ceiling |
| `/api/security/accounts/*` | POST | Update/disable/delete/password/session actions | `manage_users`, target ceiling, CSRF; step-up where required |
| `/api/security/roles` | GET/POST | Bounded role list/create | `manage_roles`, delegation ceiling, CSRF for mutation |
| `/api/security/roles/*` | POST | Update/delete custom role | `manage_roles`, delegation ceiling, CSRF |
| `/api/security/audit` | GET | Redacted bounded pagination | `view_audit` |
| `/api/security/ap-password` | POST | Active AP secret only | `manage_network` + `change_ap_secret` + recent reauthentication + CSRF |
| `/api/security/recovery/exit` | POST | Clear recovery marker and controlled reboot | `invoke_recovery` + recent reauthentication + CSRF |

The exact administration wildcard strategy will use ESP-IDF-supported bounded URI matching or explicit collection/action endpoints. No permissive fallback handler may convert an unknown route into an authorized mutation.

## 4. Browser Command Capability Matrix

| Command | Capability | Existing downstream decision |
|---|---|---|
| `set_target` | `motion` | Browser adapter and command router |
| `start` | `motion` | Browser adapter and command router |
| `stop` | `motion` | Browser adapter and command router |
| `unlock` | `motion` | Browser adapter and command router |
| `recover` | `motion` | Browser adapter and command router |
| `estop` | `software_estop` | Existing authorized safety-preemption route |
| `reset_estop` | `reset_software_estop` | Browser adapter and command router |

Authorization rejection occurs before body dispatch and produces zero router calls. Authorization never grants browser authority and never mutates machine state.

## 5. Component Ownership

| Component | Ownership |
|---|---|
| `argus_auth_service` | Login decoding, canonical login lookup, pre-KDF admission, generic failure behavior, synthetic verifier, and KDF completion |
| `argus_session_manager` | Random opaque tokens, token digests, CSRF secrets, expiry, bounded allocation, account/session revocation |
| `argus_authorization` | Effective capability calculation, immutable role ceilings, route/operation decisions, and deny-by-default policy |
| `argus_security_admin` | Bounded human/custom-role transactions, readback, security epoch changes, password changes, and final-admin safeguards |
| `argus_security_audit` | Bounded `sec_audit` ring, sequence/overflow metadata, coalescing, reservation, persistence, and redacted pagination |
| `argus_http_security` | SoftAP socket proof, cookies, Origin/Host/content-type/CSRF checks, security headers, and handler context |
| `argus_http_server` | Route registration, bounded HTTP translation, and existing lifecycle/command adapters |

Security modules may read bounded snapshots but may not call state-manager mutations, trajectory, step generation, motor GPIO, or acquire authority.

## 6. Login and Credential Contract

`POST /api/auth/login` accepts one exact JSON object containing `username` and `password`. Unknown, duplicate, nested, trailing, malformed, oversized, or truncated input is rejected. Login names are 1 through 32 visible ASCII characters with no leading/trailing whitespace or control characters; canonical comparison is case-insensitive and preserves a separate display name. Existing provisioned identifiers remain valid under their accepted record policy. New human passwords are 12 through 128 bytes, accept spaces and printable characters, and have no arbitrary composition rule. Passwords remain transient and are zeroized.

Successful login returns a bounded sanitized principal/session response and sets the cookie. Wrong password, unknown principal, disabled account, and revoked account share one externally indistinguishable `401` response. Store corruption or unavailability fails closed. Unknown principals use a fixed synthetic PBKDF2 verifier at the same accepted cost.

The console principal authenticates against the console verifier and receives protected Argus Personnel capabilities. It is not converted into a mutable human record. Human accounts remain separately identified and auditable.

## 7. Pre-KDF Admission and Throttling

Throttling runs before verifier lookup and KDF queue admission:

1. Reject non-SoftAP, malformed, oversized, cooldown, and duplicate in-flight attempts without KDF.
2. Track bounded peer and canonical-principal buckets in RAM; no unauthenticated throttle state is persisted.
3. Allow one active KDF and one queued KDF request. Additional requests receive bounded `429` or `503` without allocating unbounded work.
4. Five failures within 60 seconds start a 30-second cooldown. Repeated windows double to 60, 120, and 240 seconds, capped at five minutes.
5. Successful authentication clears the applicable failure window.
6. Bucket eviction uses bounded least-recently-observed replacement without creating permanent lockout.

The KDF remains on the dedicated worker established in Phase 4D.2. Measurements must prove maximum active/queued work, actual KDF invocation count under flood, heap stability, watchdog health, and no control-task starvation.

## 8. Session and Cookie Contract

The RAM-only table holds eight sessions total and two per account. Each record contains a random opaque session-token digest, a random CSRF-token digest, stable principal ID/type, effective capability snapshot metadata, credential version, security epoch, creation time, last activity, recent-reauth time, and revocation state. Raw bearer tokens are returned only to the browser and never logged or persisted. Comparisons are constant time.

Idle expiry is 15 minutes; absolute expiry is eight hours; recent reauthentication lasts five minutes. Expired and revoked entries are zeroized. Reboot revokes every session. At either the per-account or global limit, new login fails explicitly; an active privileged session is never silently evicted to admit another login. The implementation will report measured RAM cost.

Cookie contract:

`Set-Cookie: argus_session=<opaque>; Path=/; HttpOnly; SameSite=Strict`

`Secure` is intentionally absent because the current server is HTTP-only. No `Domain`, `__Host-`, or `__Secure-` prefix is used. This remains vulnerable to an attacker capable of observing or modifying AP traffic and is not represented as HTTPS-grade security.

## 9. CSRF and Browser Request Contract

Every authenticated state-changing browser request requires:

- a valid session cookie;
- `X-Argus-CSRF` matching the per-session synchronizer token;
- exact expected content type;
- same-origin `Origin` and validated `Host`;
- a non-GET mutation method;
- SoftAP local-destination and peer proof.

CORS is denied by default. Missing or cross-origin requests fail before body parsing. GET and HEAD never mutate state. Responses use `Cache-Control: no-store`, `X-Content-Type-Options: nosniff`, a truthful frame policy, bounded referrer policy, and a CSP compatible with the embedded local UI. HSTS is prohibited on HTTP.

## 10. Authorization and Revocation

One centralized API receives a stable principal/session snapshot, capability, target level/scope, and operation flags. Undefined routes and capabilities deny. Effective permissions are the intersection of defined permissions, assigned built-in/custom roles, direct constraints, current security epoch, actor scope, and immutable level ceiling.

Argus Personnel may administer protected access. Ordinary Client Admin administration is downward-only. A Client Admin requires explicitly assigned `manage_client_admins` to create, modify, disable, or delete another Client Admin; that capability is non-delegable by Client Admin and can never affect Argus Personnel. Callers may grant only permissions they possess and may delegate.

Password, enabled-state, role, permission, AP-password, and relevant policy changes increment credential/security epochs as applicable and revoke affected sessions. Password changes require the current password for self-service or an authorized administrator plus target/new password supplied in one non-echoing browser transaction. No temporary reusable password is generated. Disabling or deleting a principal revokes all sessions before completion. The final usable protected Argus administration path cannot be removed.

## 11. Security Administration Storage

Phase 4D.2 metadata is extended with bounded human records and custom-role records under the same encrypted, versioned, atomic storage owner. Limits remain 16 human accounts and 16 total roles, including six built-ins. Account IDs are immutable and distinct from mutable display names. Built-in roles remain protected. A record is usable only after commit and readback succeed.

All mutation APIs use exact schemas, bounded strings/counts, duplicate/unknown-key rejection, and transactional validation. Administrative list APIs expose only sanitized metadata and never verifiers, salts, tokens, AP secrets, or raw storage.

## 12. Active AP Password and Recovery Exit

Active AP-password change requires `manage_network`, `change_ap_secret`, current-credential confirmation, recent reauthentication, and CSRF. The new 12-to-63-character printable ASCII credential is entered twice, committed atomically, read back, and then applied through the network owner. The factory recovery credential is never changed. Affected sessions are revoked, reconnection is required, and no secret is logged, audited, exported, or sent through MQTT.

Recovery exit requires an authenticated Argus Personnel principal or an explicitly authorized Client Admin with `invoke_recovery`, recent reauthentication, and CSRF. It clears only the persistent recovery marker after audit reservation, then requests a controlled reboot. It preserves identity, customer network, machine state, targets, E-stop/fault, authority policy, users, roles, and credentials. Repeated exit requests are idempotent or safely rejected.

## 13. Audit Contract

`sec_audit` stores a fixed-capacity ring with monotonic sequence, boot identifier, boot-relative monotonic time, optional truthful wall time, event type, outcome, actor ID/type/scope, target ID, interface, bounded reason code, and security/credential generations. Phase 4D.3a measured the accepted representation as 168 bytes per record, 255 logical retained records in 256 physical slots, and a 262,144-byte partition. The audit queue contains eight pointers, the worker stack is 5,120 bytes, and the maximum queued request payload is 2,112 bytes plus allocator overhead.

Events never contain credentials, verifiers, salts, session/CSRF tokens, Authorization headers, raw bodies, pointers, or memory addresses. Unauthenticated failures are coalesced/rate-limited. Privileged security mutations reserve and persist their required audit event before commit; inability to reserve fails that mutation closed without affecting machine state. A bounded asynchronous queue handles noncritical events. Authorized pagination is redacted and bounded.

**Phase 4D.3a correction:** the pre-mutation record is explicitly `PREPARED`, never success; a correlated terminal record reports success or failure after mutation. Audit pagination is implemented as strict newest-first pages with an exclusive non-secret sequence cursor and maximum limit 16. See `PHASE_4D_3A_IMPLEMENTATION_PLAN.md`.

## 14. Tasks, Queues, Locks, and Resources

- The HTTP task owns request parsing and response transmission only.
- The Phase 4D.2 KDF worker owns PBKDF2; admission is one active plus one queued.
- The security-store writer remains the only `sec_store` mutation owner.
- One audit worker owns `sec_audit`; queue depth and stack are fixed and measured.
- Session and throttle tables use short nonrecursive mutex sections and never hold locks across KDF, NVS, HTTP send/receive, network lifecycle calls, or command dispatch.
- Security and audit work uses bounded stack buffers and queues; no credential survives request completion.
- Failure of authentication/audit/storage may reject new security operations but may not stop the pump, mutate motion state, or block real-time tasks.

## 15. Browser Integration and Basic-Auth Retirement

The existing embedded HTML/JavaScript technology is retained. `/login`, `/operate`, and `/commission` are local assets with no CDN dependency. Controls are shaped by sanitized capabilities but every server operation independently authorizes. Login never claims authority. The UI truthfully labels HTTP-only limitations and does not display secrets after entry.

All `Authorization: Basic` parsing, `WWW-Authenticate` challenges, hardcoded `admin` comparison, plaintext portal-password compatibility, Basic-auth browser prompts, and mutation-by-GET logout are removed. Migration aliases redirect only after the new session middleware; they never retain Basic Auth as a fallback.

## 16. Validation and Acceptance

Validation includes strict host/pure tests, browser JavaScript syntax and functional tests, static boundary audits, secret-pattern review, full-clean ESP-IDF v5.5.3 build, three controller pure-suite executions, SoftAP success/STA rejection proof, login/throttle/session/CSRF/authorization/admin/audit/AP-password/recovery tests, browser/MQTT regressions, and stationary resource observations.

Hardware credential interactions are local and non-echoing. Real values never enter chat, command-line arguments, tracked files, logs, screenshots, or reports. The active AP credential is restored to its pre-test value before final disposition.

Acceptance completed July 23, 2026. Three final controller invocations passed 2,133/2,133 executions with production isolation intact. SoftAP login, STA rejection, bounded throttling, logout, CSRF rejection, protected administration, active AP credential change/restoration, physical recovery entry, authenticated recovery exit, and clean normal-mode reboot passed on hardware. The accepted implementation remains a plain-HTTP trusted-local-network release and provides no physical-extraction resistance.

## 17. Phase 4D.4 Seams and Remaining Threats

The machine-record schema and authorization vocabulary remain available for later machine enrollment and MQTT CONNECT authentication, but no Phase 4D.3 human session can authenticate an MQTT socket. HTTPS/TLS, certificates, secure-cookie transport, hostile-network operation, eFuse/HMAC-derived NVS keys, secure boot, flash encryption, UART/JTAG restriction, broad penetration testing, and irreversible provisioning remain deferred.

Plain HTTP permits credential/session interception by an attacker already on the AP. Software-stored NVS encryption keys do not resist physical extraction. Bounded tables and throttles reduce but do not eliminate denial of service. Phase 4D.3 acceptance must state these limitations exactly.
