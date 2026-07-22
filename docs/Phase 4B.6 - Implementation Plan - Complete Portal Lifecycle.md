# Phase 4B.6 - Implementation Plan: Complete Portal Lifecycle

**Status:** IN PROGRESS - IDENTITY ESTABLISHED; IMPLEMENTATION AND ACCEPTANCE PENDING

## Accepted Baseline

- Accepted main commit: `0ab8cbee5aa64d3ded695bbc75ad09c1f0583439`
- Accepted tag: `v2-phase4b.5-hardware-verified`
- Working branch: `phase4b6-portal-lifecycle-acceptance`
- Phase identity: `v2-phase4b.6-dev`

Phase 4B.5 browser controls, authoritative status, recovery correction, E-stop request orchestration, isolated controller evidence, and powered UI-to-motor evidence remain accepted. Phase 4B.6 does not repeat motor testing or issue motion commands.

## Scope

Phase 4B.6 completes the technician service portal as one coherent lifecycle:

1. Preserve and validate identity and Wi-Fi configuration behavior.
2. Preserve identity's permanent-until-configuration-reset lock.
3. Validate coordinated restart, Local Service entry, and Local Service exit.
4. Add authenticated configuration factory reset through `POST /api/factory-reset`.
5. Reboot truthfully into uncommissioned operation after reset.
6. Reprovision identity once, restore Wi-Fi, and prove the identity lock again.
7. Complete a final Local Service round trip and restore normal supervisory operation.

## Endpoint Contract

The planned endpoint is `POST /api/factory-reset` with exact JSON confirmation `{"confirm":"FACTORY_RESET"}` and `application/json`. Admission requires valid Basic authentication, `SERVICE_AP_ONLY`, `LOCAL_SERVICE/BROWSER`, no conflicting transition, and a restart-safe stationary machine. Missing, malformed, duplicated, incorrect, oversized, truncated, timed-out, or otherwise uncertain requests reject without queueing reset. One accepted request queues one lifecycle event and returns bounded HTTP 202 before the connection disappears.

The accepted configuration contract remains `POST /api/config/save` with `scope` equal to `identity` or `wifi`. Obsolete draft stage/validate/apply routes will not be introduced.

## Factory-Reset Policy

Configuration factory reset erases both dual-slot records, selector metadata, provisioning high-water state, identity fields, and STA SSID/password while preserving the durable reset-pending transaction and boot recovery. Portal authentication in `argus_portal` remains outside reset scope. Unified credential recovery remains deferred.

## Lifecycle Architecture

The HTTP handler performs bounded validation and queueing only. The network-manager task owns destructive execution, safety and authority revalidation, safe output establishment, authority revocation, network and HTTP shutdown in established lock order, durable NVS reset, and reboot. The handler never stops its own server, performs the reset transaction synchronously, or calls `esp_restart()`.

## Lock and Task Boundaries

- HTTP task: authenticate, decode, evaluate policy, queue once, respond 202.
- Network manager: serialize lifecycle events and own reset/reboot.
- Restart/state seams: perform preflight and final stationary safety checks.
- NVS reset core: preserve pending-marker durability and fail truthfully.
- Pure tests: use caller-owned state and injected operations; no production singleton mutation.

## Browser Experience

The dashboard will expose authoritative state-eligible navigation and actions for identity, Wi-Fi, restart, service entry/exit, factory reset, motion controls, password change, and logout. Factory reset requires two deliberate actions and an exact scope warning. Pending lifecycle actions suppress duplicates, transport loss is not presented as controller failure, and authoritative APIs remain the only displayed state source.

## Single-Adapter Strategy

A temporary runner outside the repository will preserve the existing Windows `Paladin6661` profile, switch the detected Wi-Fi adapter to the controller Service AP for bounded checks, capture sanitized evidence, and reconnect to `Paladin6661` in guaranteed cleanup. It will not print, export, alter, or persist credentials.

## Safety Boundary

The motor may be connected and unattended. Every live mutation requires stationary `UNLOCKED` or eligible safe state, zero applied/generated speed, idle ramp, and no unsafe fault. No motion command or powered motor test is authorized. Existing Phase 4B.5 evidence is incorporated without repetition.

## Validation Plan

- Host tests for strict factory-reset decoding, handler decisions, lifecycle policy, durable reset transaction, and UI behavior.
- Static registration, dependency, isolation, secret, and diff audits.
- ESP-IDF v5.5.3 full-clean no-ccache build with zero warnings/errors.
- COM5 flash with hash verification and three complete pure-suite invocations.
- Live commissioned preflight, password-preserving Wi-Fi save, restart, service entry/exit, factory reset, uncommissioned recovery, reprovisioning, Wi-Fi restoration, and final round trip.

## Exclusions

No motor command, powered motor campaign, pump head, hose, tubing, fluid, chemical, pressure, flow calibration, loaded performance, endurance, Phase 4C MQTT contract, or Phase 4D security redesign is included.

## Commit Progression

1. Identity establishment: pending this first commit.
2. Factory-reset lifecycle implementation: pending.
3. Portal lifecycle UX and tests: pending.
4. Runtime and live acceptance evidence: pending.
5. Final documentation, merge, and tag: pending.
