# Phase 4D.4 Implementation Plan

**Status:** COMPLETE AND ACCEPTED - July 25, 2026

**Branch:** `phase4d4-machine-enrollment-mqtt-auth`

**Firmware identity:** `v2-phase4d.4-dev`

**Accepted baseline:** `05d6cde3e00c142beb9f2df6b88f501893254942`

**Accepted implementation candidate:** `cea28c2c8476f8f991f8337699e24cb16ea217e8`

## Purpose

Phase 4D.4 establishes durable machine-client enrollment and administration,
strict MQTT 3.1.1 CONNECT authentication, connection-bound machine principals,
per-packet authorization and immediate credential invalidation while preserving
the accepted Phase 4C command, authority, and fail-operational architecture.

## Architectural Boundaries

- Authentication identifies a machine; it does not grant operating authority.
- Authorization evaluates transport, receiving interface, scope, topic, and
  capability before Phase 4C protocol admission.
- The existing command router remains the sole normal MQTT motion dispatch path.
- Security and enrollment code must not call the authority manager, state
  manager, trajectory, step generator, motor driver, or GPIO.
- Machine records are separate from human identities and browser sessions.
- Credentials are controller-generated, verifier-only at rest, and disclosed
  exactly once after a successful enrollment or rotation.

## Delivered Work

1. Added a dedicated encrypted-NVS machine directory with 16-record capacity,
   dual-slot atomic commits, validation, generation control, and one writer.
2. Added SoftAP-only authenticated machine listing, enrollment, rotation,
   enable/disable, revocation, and deletion routes with route-inventory and
   prepared/terminal audit coverage.
3. Parses and authenticates MQTT CONNECT outside the broker global lock, then binds
   a sanitized machine principal atomically to the connection.
4. Revalidates durable principal state and enforces interface, transport,
   subscription, publish, topic-scope, and capability policy on every packet.
5. Invalidates live connections immediately after relevant machine mutations,
   while retaining per-packet fail-closed revalidation.
6. Added host, pure, broker, storage, HTTP, authorization, isolation, and live
   stationary acceptance coverage.

## Supervisory Correction Record

Independent review paused live acceptance after the initial implementation.
The corrected candidate must:

- bind disconnect ownership to the selected slot, principal, connection ID,
  and socket through shutdown so descriptor reuse cannot affect another client;
- coordinate authentication and mutation invalidation with a bounded
  invalidation generation so stale authentication cannot bind;
- make an invalidated connection policy-inert even when socket shutdown fails;
- attempt live disconnect after every committed rotate, disable, revoke, or
  delete mutation even when terminal audit finalization fails;
- keep all administrative capabilities prohibited through ordinary machine
  enrollment; and
- preserve the Phase 4C shared `command_result` subscription behavior as an
  explicitly documented inherited contract.

All live evidence collected before these corrections is exploratory only. Live
acceptance restarts from the beginning after corrected source, build, and
controller-suite validation.

## Final Corrected Validation

The accepted candidate completed an ESP-IDF v5.5.3 full-clean, no-ccache build
with zero compiler warnings and zero compiler errors. The application binary is
`0x12b3b0` bytes, leaving `0x1d4c50` bytes (61 percent) in the smallest OTA
partition.

COM5 identified the expected ESP32-S3 and the corrected image flashed with hash
verification. Three complete controller-suite invocations then passed from a
stable diagnostic service baseline. Each invocation reported:

- 268 distinct tests;
- three internal repeat passes;
- 804 executions;
- 804 passed and zero failed;
- unchanged authority generation, network mode, broker state, machine state,
  and task count; and
- no panic, reset, watchdog, brownout, assertion, stack-canary failure, heap
  corruption, or task leak.

The three invocations therefore produced 2,412 passing automated executions.
Coordinated service exit completed and rebooted cleanly afterward.

The final diagnostic boot confirmed the expected ESP32-S3, firmware identity,
stationary `UNLOCKED` state, zero configured/applied/generated output, disabled
driver, `AP_DISCOVERABLE` network state, and `SUPERVISORY/MQTT` authority. The
machine-directory writer retained 5,344 bytes of its 6,144-byte stack after the
final correction. No runtime fault was observed.

## Live Stationary Acceptance

The complete live sequence restarted from the beginning after all supervisory
corrections. A temporary Node-RED-class machine was enrolled through the
authenticated SoftAP console with bounded controller/topic scope and only
`view_status` plus `request_authority`. Its 43-character credential was disclosed
once, used only in volatile test tooling, and removed after use.

The accepted live results were:

- missing credentials rejected with MQTT 3.1.1 CONNACK code 2;
- an incorrect secret rejected with code 4;
- a correct secret rejected on the disallowed STA interface with code 4;
- the same correct secret accepted on the allowed SoftAP interface with code 0;
- exact authorized status and shared command-result subscriptions accepted;
- a broad root wildcard subscription rejected with SUBACK `0x80`;
- authentication alone left supervisor link `OFFLINE` and did not mutate authority;
- one current-session, non-retained QoS 1 heartbeat received PUBACK, made the
  supervisor link `ONLINE`, then naturally `STALE` without changing authority,
  sequence, target, output, driver, or machine state;
- publication to controller-owned state was policy-dropped before mutation or
  retention, and the connection remained transport-usable;
- a duplicate simultaneous MQTT client ID was rejected deterministically;
- credential rotation closed the exact live connection, rejected the old secret,
  accepted the new secret, and preserved the stable machine ID;
- revocation closed the exact live connection and rejected subsequent reconnect;
- unrelated human browser sessions remained valid throughout;
- all temporary machine records were revoked and deleted; and
- Overview and Operations remained truthful and stationary, with all ordinary
  browser motion controls disabled under supervisory authority.

No Start, setpoint, Recover, or other motion-capable command was issued. The motor
was physically disconnected. The live work therefore accepts machine enrollment,
MQTT authentication/authorization, lifecycle invalidation, and stationary
Phase 4C regression only.

One harmless repeated delete request occurred after a browser-automation timeout.
The first delete had already committed, the repeated request truthfully recorded
`delete_failed`, and no machine record or credential remained.

## Acceptance Boundary

Phase 4D.4 is accepted for the reviewed implementation, automated controller
runtime, browser regression, and stationary live MQTT machine-client lifecycle.
The exact evidence is recorded in `Phase 4D.4 Tests.md`.

This phase does not provide MQTT TLS, HTTPS, hostile-network security,
physical-extraction resistance, HMI implementation, AI integration, or powered
pump acceptance.
