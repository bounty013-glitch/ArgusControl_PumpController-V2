# Phase 4D.3a Implementation Plan

**Status:** COMPLETE AND ACCEPTED (July 23, 2026)

**Branch:** `phase4d3a-security-acceptance-corrections`

**Accepted baseline:** `c765825f33f7dbeb1538f256a564ae3901b330cc`

**Accepted baseline tag:** `v2-phase4d.3-browser-access-control-accepted`

**Firmware identity:** `v2-phase4d.3a-dev`

**Identity-first commit:** `3936497` (`chore: establish Phase 4D.3a identity`)

## Purpose and Boundaries

Phase 4D.3a corrects four post-acceptance findings without redesigning the accepted Phase 4D.3 authentication, session, authorization, directory, audit-ring, browser, network, MQTT, authority, or motion architecture:

1. A pre-mutation audit reservation was encoded as final success.
2. AP-secret apply and recovery reboot timers were armed before the final HTTP response.
3. The retired `GET /api/logout` alias remained registered without the SoftAP guard.
4. The route proof covered only eight security routes and the audit API was not truly paginated.

Phase 4D.3a does not implement machine authentication, MQTT CONNECT authentication, HTTPS, TLS, certificates, hostile-network acceptance, irreversible device security, a new browser command, or Phase 4D.4. It authorizes no powered motion.

## Audit Mutation Lifecycle

The persisted audit schema and 168-byte record size remain unchanged. The prior 16-bit reserved field now carries a bounded lifecycle identifier. Existing Phase 4D.3 records have lifecycle identifier zero and remain readable.

The privileged mutation lifecycle is:

1. Complete HTTP interface, authentication, Origin/Host, content-type, CSRF, route-capability, and request validation.
2. Persist a required `PREPARED` record. `PREPARED` is outcome 4 and is never interpreted as success.
3. Execute the transactional security mutation.
4. Persist a correlated terminal `SUCCESS` only after commit/readback succeeds, otherwise persist terminal `FAILED`.
5. Return the mutation result only after terminal audit finalization is attempted.

The lifecycle identifier is derived from the prepared record sequence and is nonzero. Because the retained window is 255 records, the 16-bit folded sequence remains unique throughout any retained correlation window. Reasons are bounded action codes ending in `_prepared`, `_succeeded`, or `_failed`; no request body or secret is retained.

Covered privileged mutation paths are account create, enable, disable, delete, display update, role assignment, password reset, explicit principal-session revocation, custom-role create/delete, self-password change, active AP-secret change, and security-recovery exit. Canonical self-logout remains an immediate session safety action and writes its truthful terminal result best-effort after revocation.

If required prepared evidence cannot be persisted, the mutation is not called. If terminal finalization fails after a mutation, the durable prepared record remains as incomplete evidence, a sanitized diagnostic is emitted, and an in-memory `finalization_degraded` latch blocks later privileged mutations until a controlled reboot. The implementation neither fabricates a terminal result nor rolls back an already committed security mutation. Audit failure never calls motion, state, trajectory, GPIO, authority, or command-router code.

## Response Before Transition

`argus_security_transition` provides a bounded decision seam shared by active AP-secret apply and recovery exit:

1. Confirm the timer resource exists and reserve one transition request without arming it.
2. Complete and audit the security mutation.
3. Revoke affected sessions.
4. Send the bounded `202 Accepted` response and require `ESP_OK`.
5. Arm the 300 ms one-shot timer only after response completion.
6. Let the timer callback claim the transition once and post work to the network owner.

Timer handles are created during security-HTTP initialization, not in a request handler. Resource absence rejects the request before mutation. Duplicate pending requests receive a bounded conflict. Response failure cancels the reservation and prevents timer arming. Timer-arm failure after a completed response emits bounded sanitized diagnostics, leaves the committed state recoverable, and does not attempt a second response or direct restart. A callback claim is one-shot.

The response says the security change is committed and an automatic transition was requested; it does not claim that a restart has already occurred.

## Retired Logout Disposition

No accepted page or client uses `GET /api/logout`. Its handler, URI object, and registration were removed. The only live logout endpoint is `POST /api/auth/logout`, which remains SoftAP-only, authenticated, same-origin, strict-JSON, and CSRF-protected. The retired alias remains in the inventory only as an explicitly unregistered historical classification.

## Human Route Inventory

All active human routes require a proven SoftAP local destination and SoftAP peer. Protected routes require a current RAM-only session. Every POST mutation requires exact `application/json`, same-origin Host/Origin, and CSRF. All responses use the accepted no-store security-header policy. STA requests fail with bounded `local_ap_required` before protected work.

| Path | Method | Class | Auth | CSRF/JSON | Capability | Security mutation | Browser adapter |
|---|---|---|---|---|---|---|---|
| `/login` | GET | Public page | No | No | None | No | No |
| `/api/auth/login` | POST | Public auth | No | Origin + JSON | None | No | No |
| `/api/auth/session` | GET | Authenticated | Yes | No | None | No | No |
| `/api/auth/logout` | POST | Authenticated | Yes | Yes | None | Yes | No |
| `/api/auth/reauth` | POST | Authenticated | Yes | Yes | None | Yes | No |
| `/api/auth/change-password` | POST | Authenticated | Yes | Yes | Self | Yes | No |
| `/operate` | GET | Authenticated page | Yes | No | `view_status` | No | No |
| `/commission` | GET | Administrative page | Yes | No | Any administration capability | No | No |
| `/api/status` | GET | Authenticated | Yes | No | `view_status` | No | No |
| `/api/identity` | GET | Authenticated | Yes | No | `view_status` | No | No |
| `/` | GET | Public redirect | No | No | None | No | No |
| `/controls` | GET | Authenticated redirect | Yes | No | `view_status` | No | No |
| `/change-password` | GET | Authenticated redirect | Yes | No | None | No | No |
| `/api/config` | GET | Administrative | Yes | No | Any config capability | No | No |
| `/api/config/save` | POST | Administrative | Yes | Yes | Identity or client-network capability | No | No |
| `/config/identity` | GET | Administrative redirect | Yes | No | `modify_identity` | No | No |
| `/config/wifi` | GET | Administrative redirect | Yes | No | `manage_client_network` | No | No |
| `/api/network/reconnect` | POST | Administrative | Yes | Yes | `manage_client_network` | No | No |
| `/api/restart` | POST | Administrative | Yes | Yes | `manage_firmware` | No | No |
| `/api/service/enter` | POST | Administrative | Yes | Yes | `request_authority` | No | No |
| `/api/service/exit` | POST | Administrative | Yes | Yes | `request_authority` | No | No |
| `/api/command` | POST | Browser command | Yes | Yes | Per decoded command | No | Yes |
| `/api/factory-reset` | POST | Administrative | Yes | Yes | `commission` | No | No |
| `/api/security/accounts` | GET | Administrative | Yes | No | `manage_users` | No | No |
| `/api/security/accounts` | POST | Administrative | Yes | Yes | `manage_users` | Yes | No |
| `/api/security/accounts/action` | POST | Administrative | Yes | Yes | `manage_users` | Yes | No |
| `/api/security/roles` | GET | Administrative | Yes | No | `manage_roles` | No | No |
| `/api/security/roles` | POST | Administrative | Yes | Yes | `manage_roles` | Yes | No |
| `/api/security/audit` | GET | Administrative | Yes | No | `view_audit` | No | No |
| `/api/security/ap-password` | POST | Administrative | Yes | Yes | `manage_network` + `change_ap_secret` | Yes | No |
| `/api/security/recovery/exit` | POST | Administrative | Yes | Yes | `invoke_recovery` | Yes | No |
| `/api/logout` | GET | Retired/unregistered | No | No | None | No | No |

The main HTTP server and security HTTP module each register through explicit production route arrays. Diagnostic tests compare both arrays bidirectionally with this 32-entry policy inventory: 31 active registrations and one explicitly retired alias. Duplicate registrations, missing inventory entries, active inventory entries without registrations, GET/HEAD security mutations, missing AP/auth/CSRF/JSON classification, absent capabilities, and alternate browser adapters fail the suite.

## Audit Pagination Contract

`GET /api/security/audit` is SoftAP-only, authenticated, and requires `view_audit`. It is read-only and returns newest records first.

- Default and maximum `limit`: 16.
- Optional `before`: nonzero unsigned 64-bit decimal sequence, exclusive.
- Parameters may appear in either order.
- Unknown, empty, duplicate, zero, excessive, overflowing, trailing, or oversized parameters return bounded `400 invalid_pagination`.
- Sequences and `next_before` are JSON strings to preserve exact 64-bit values.
- `has_more` states whether an older valid record exists.
- `next_before` is the oldest returned sequence when more records exist, otherwise null.
- `corruption_gap` truthfully reports skipped wrong-size, unsupported-schema, missing, or CRC-invalid slots.
- Records appended between requests cannot duplicate an earlier page because the cursor is an exclusive sequence boundary.
- A cursor newer than the coherent `next_sequence` fails closed; a stale cursor older than retained history returns an empty terminal page.

No flash offset, raw record, credential, verifier, token, cookie, request body, or security material is exposed.

## Audit Measurements

Measured from the Phase 4D.3a ELF:

| Item | Measurement |
|---|---:|
| Encoded/in-memory audit record | 168 bytes |
| Audit metadata record | 40 bytes |
| Mutation correlation context | 124 bytes |
| One queued request object | 264 bytes |
| Maximum 16-record page buffer | 2,704 bytes |
| Logical retained capacity | 255 records |
| Physical ring slots | 256 |
| Audit partition | 0x40000 bytes (262,144 bytes) |
| Audit queue depth | 8 pointers |
| Maximum queued request payload RAM | 2,112 bytes plus allocator overhead |
| Queue pointer payload | 32 bytes plus FreeRTOS queue control overhead |
| Audit worker stack allocation | 5,120 bytes |
| Audit module fixed BSS | 114 bytes in diagnostic build |

The 255-record logical cap is deliberately below physical partition capacity. A record write is committed before the metadata pointer advances; interrupted writes leave the old metadata authoritative. On full storage, the oldest logical record is overwritten and `overwritten` increments. Sequence is monotonic 64-bit and skips zero. CRC, size, and schema are checked on reads. Page reads skip bad/unsupported slots and report a gap. A required write failure rejects a pre-mutation reservation or leaves a durable prepared/incomplete event after terminal failure.

Unauthenticated login failures remain coalesced to at most one audit attempt per 60 seconds in the HTTP layer. Privileged administration is human-rate and writes two records per attempted mutation. At one privileged mutation per minute continuously, two NVS record commits plus metadata commits occur per minute; this is outside expected local administrative usage but remains finite and visible. The partition and NVS wear behavior are not a substitute for flash endurance testing.

Static DIRAM after correction is 189,911 bytes, eight bytes above accepted Phase 4D.3. `.bss` is 85,144 bytes, also eight bytes above Phase 4D.3; `.data` remains 20,156 bytes. Audit page memory is request-stack bounded and is not retained between requests.

## Ownership and Locks

- One audit task owns writes to `sec_audit`; callers enqueue bounded request pointers.
- The audit mutex protects metadata, coherent reads, and the finalization-degraded latch.
- The HTTP server task performs authentication, authorization, body decoding, and transition ordering.
- `esp_timer` callbacks only claim one pending transition and post to the network owner.
- No audit, authentication, administration, pagination, or transition helper acquires motion, state, trajectory, step-generator, GPIO, or operating-authority locks.

## Acceptance Validation

Validation completed against functional commit `e9b1968`:

- Existing Phase 4B controls, Phase 4B.6 portal, and Phase 4D.3 browser host tests passed.
- ESP-IDF v5.5.3 no-ccache fullclean/build/size passed with zero warnings and zero errors.
- COM5 identified the expected ESP32-S3 and the accepted Phase 4D.3 image before flash.
- The stationary Phase 4D.3a image booted cleanly with preserved customer identity, STA configuration, security store, audit store, machine state, zero setpoint, and disabled driver.
- Three controller suite invocations each passed 729/729, for 2,187/2,187 executions.
- STA requests to `/login` and `/api/auth/session` returned `403 local_ap_required`; retired `/api/logout` returned 404.
- SoftAP login, reauthentication, canonical logout, bounded audit pagination, malformed-query rejection, and Basic Auth rejection passed.
- Two active AP-secret mutations returned their final accepted response before asynchronous AP reconfiguration. The temporary credential worked, and the original credential was restored and verified without disclosure.
- Physical BOOT hold entered `SECURITY_RECOVERY_AP_ONLY`; authenticated recovery exit returned `Recovery exit accepted` before the clean reboot restored `AP_DISCOVERABLE`.
- A live read-only audit probe examined 64 records and confirmed at least two correlated AP-secret prepared/success pairs and one recovery-exit prepared/success pair. No prepared record was encoded as success.
- No command dispatch, authority acquisition, motor motion, panic, watchdog, brownout, stack/heap failure, task leak, or reset loop occurred.

Accepted controller suite:

- 243 distinct tests
- 3 internal passes
- 729 executions per invocation
- 2,187 executions across three final invocations

No test may issue a motion-capable command or change operating authority.

## Remaining Threats

HTTP remains plaintext. A hostile peer already on the local AP can observe traffic. Software-stored XTS keys do not resist physical flash extraction. Secure boot, flash encryption, UART/JTAG restriction, MQTT CONNECT authentication, TLS, certificates, hostile-network operation, and broad penetration acceptance remain deferred. Phase 4D.3a does not imply pump, fluid, pressure, process, endurance, or powered-motion acceptance.
