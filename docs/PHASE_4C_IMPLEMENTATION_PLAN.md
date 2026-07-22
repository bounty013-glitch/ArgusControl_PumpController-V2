# Phase 4C - MQTT Contract and Fail-Operational Supervisory Control

**Status:** IN PROGRESS

**Branch:** `phase4c-mqtt-contract-and-fail-operational`

**Starting baseline:** `1bab5b464bc0fa55320232f7a5f76fa75598a6eb`

**Required baseline tag:** `v2-phase4b.6-hardware-verified`

**Firmware identity:** `v2-phase4c-dev`

## Purpose

Phase 4C replaces the preliminary MQTT interface with one production-used supervisory contract. MQTT transports bounded intent and controller information; it does not own machine state. Authority, command routing, state management, trajectory generation, and STEP generation remain authoritative in their existing layers.

## Implementation Scope

1. Construct the bounded topic root `argus/<client_id>/<unit_id>` from one coherent commissioned-identity snapshot per broker lifecycle.
2. Enforce exact external publication ownership for canonical command and heartbeat topics before retained storage, subscriber delivery, parsing, or dispatch.
3. Add broker connection identity and bounded message metadata sufficient for supervisory lease ownership and deterministic disconnect handling.
4. Create a fresh non-persistent command session on every successful broker start.
5. Implement strict heartbeat, command-envelope, sequence, replay, duplicate, and result-publication contracts.
6. Route every admitted MQTT motion command through `argus_cmd_router_dispatch()` with server-owned source and authority generation.
7. Publish a coherent retained metadata, state, status, and truthful open-loop telemetry baseline, with bounded periodic refresh.
8. Preserve fail-operational behavior: MQTT disconnect or heartbeat loss changes link observability only and does not mutate motion state or output.
9. Add pure, seam, broker-policy, production-isolation, and live protocol evidence.

## Safety Boundary

Software, build, controller-suite, and stationary MQTT validation may proceed autonomously. No nonzero setpoint, Start, Recover-as-motion test, or other command capable of producing motion may be issued until the operator gives the required explicit powered-test confirmation.

Phase 4C does not accept pump-head, hose, tubing, fluid, chemical, hydrogen peroxide, pressure, flow, calibration, loaded-torque, process, or endurance behavior. MQTT software E-stop is not a safety-rated physical E-stop.

## Acceptance Gates

- Phase identity established before functional implementation.
- Host and pure validation complete with strict warnings and supported sanitizers.
- ESP-IDF v5.5.3 full-clean no-ccache build completes with zero warnings and zero errors.
- Three genuine interactive controller-suite invocations pass with production isolation intact.
- Stationary live MQTT contract and adversarial protocol tests pass.
- Powered low-speed fail-operational tests pass only after explicit operator confirmation.
- Documentation identifies exact implementation and evidence commits.
- Feature branch is independently reviewable before non-fast-forward merge and annotated acceptance tag.

No implementation or acceptance is claimed by this initial plan.
