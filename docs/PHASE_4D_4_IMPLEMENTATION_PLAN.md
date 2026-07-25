# Phase 4D.4 Implementation Plan

**Status:** IN PROGRESS

**Branch:** `phase4d4-machine-enrollment-mqtt-auth`

**Firmware identity:** `v2-phase4d.4-dev`

**Accepted baseline:** `05d6cde3e00c142beb9f2df6b88f501893254942`

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

## Planned Work

1. Add a dedicated encrypted-NVS machine directory with 16-record capacity,
   dual-slot atomic commits, validation, generation control, and one writer.
2. Add SoftAP-only authenticated machine listing, enrollment, rotation,
   enable/disable, revocation, and deletion routes with route-inventory and
   prepared/terminal audit coverage.
3. Parse and authenticate MQTT CONNECT outside the broker global lock, then bind
   a sanitized machine principal atomically to the connection.
4. Revalidate durable principal state and enforce interface, transport,
   subscription, publish, topic-scope, and capability policy on every packet.
5. Invalidate live connections immediately after relevant machine mutations,
   while retaining per-packet fail-closed revalidation.
6. Add host, pure, broker, storage, HTTP, authorization, isolation, and live
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

## Corrected Automated Validation

The corrected candidate completed an ESP-IDF v5.5.3 full-clean, no-ccache build
with zero compiler warnings and zero compiler errors. The application binary is
`0x12b390` bytes, leaving `0x1d4c70` bytes (61 percent) in the smallest OTA
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

**Live acceptance remains ON HOLD.** Earlier partial live observations are
exploratory only. Enrollment, CONNECT authentication, rotation, revocation,
subscription, publish, and Phase 4C regression acceptance must restart from the
beginning when dual network connectivity is available. This document does not
claim Phase 4D.4 acceptance.

## Acceptance Boundary

Phase 4D.4 remains unaccepted until implementation review, ESP-IDF v5.5.3
full-clean validation, three complete controller-suite executions, browser
regression, Phase 4C MQTT regression, and live enrollment/authentication/
rotation/revocation evidence are complete.

This phase does not provide MQTT TLS, HTTPS, hostile-network security,
physical-extraction resistance, HMI implementation, AI integration, or powered
pump acceptance.
