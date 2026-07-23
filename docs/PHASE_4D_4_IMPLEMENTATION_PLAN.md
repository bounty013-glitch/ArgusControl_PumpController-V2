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

## Acceptance Boundary

Phase 4D.4 remains unaccepted until implementation review, ESP-IDF v5.5.3
full-clean validation, three complete controller-suite executions, browser
regression, Phase 4C MQTT regression, and live enrollment/authentication/
rotation/revocation evidence are complete.

This phase does not provide MQTT TLS, HTTPS, hostile-network security,
physical-extraction resistance, HMI implementation, AI integration, or powered
pump acceptance.
