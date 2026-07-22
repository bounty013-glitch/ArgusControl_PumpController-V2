# Phase 4C - MQTT Contract and Fail-Operational Supervisory Control

**Status:** COMPLETE AND ACCEPTED on July 22, 2026

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

- Phase identity established before functional implementation: PASS.
- Host and pure validation complete with strict warnings: PASS. ASan/UBSan host execution was unavailable because no compatible host C toolchain was present.
- ESP-IDF v5.5.3 full-clean no-ccache build with zero warnings and zero errors: PASS.
- Three genuine interactive controller-suite invocations with production isolation intact: PASS, 591/591 each and 1,773/1,773 aggregate.
- Stationary live MQTT contract and adversarial protocol tests: PASS, 72 checks.
- Powered low-speed fail-operational tests after explicit operator confirmation: PASS, 50 assertions and 12 correlated command results at 500 mRPM maximum.
- Documentation identifies the exact implementation and evidence history: PASS.
- Feature branch review and preservation: PASS. The non-fast-forward merge and annotated acceptance tag are repository closeout actions recorded by Git history and the final report.

## Accepted Design

The accepted implementation separates responsibilities as follows:

- `argus_mqtt_broker`: bounded packet metadata, non-reusable connection identity, duplicate-client rejection, publication policy before retention or delivery, retained storage, and connection lifecycle notifications.
- `argus_mqtt_contract`: dynamic topic construction, strict heartbeat and command decoding, broker-session state, lease ownership, serial arithmetic, replay and duplicate decisions, and cached results.
- `argus_mqtt_runtime`: bounded worker queue, the single production MQTT-to-router dispatch call, result publication, retained baseline, and 1 Hz health publication.
- Existing authority, router, state-manager, trajectory, step-generator, network, and identity modules remain authoritative in their established domains.

The broker callback copies bounded message metadata before application use. MQTT work is serialized through the protocol worker; broker client tasks do not dispatch commands. Topic and retained tables use firmware-lifetime static storage rather than the network task stack. Twenty-five retained topics fit within 32 deliberately provisioned slots.

The command session is renewed on each boot and successful broker lifecycle. The heartbeat interval is two seconds and the lease becomes stale after six seconds. Loss of the supervisor updates link truth only; it does not synthesize Stop, clear the target, disable the driver, or mutate machine state. Reconnection republishes the current authoritative baseline.

## Acceptance Evidence

- Accepted source candidate: `ccedf51` (`fix: republish MQTT baseline on reconnect`)
- Firmware identity: `v2-phase4c-dev`
- Application binary: 1,107,536 bytes (`0x10e650`)
- OTA headroom: 2,038,192 bytes (`0x1f19b0`, 65%)
- Static-memory change from Phase 4B.6: `.bss` increased 26,600 bytes for bounded topic, retained, connection, queue, and session storage.
- Final machine state: `UNLOCKED`, driver `DISABLED`, E-stop `CLEAR`, fault `0`, and all target/output values zero.

The authoritative protocol is documented in `PHASE_4C_MQTT_CONTRACT.md`. Chronological build, runtime, stationary, powered, failure, and correction evidence is recorded in `Phase 4C Tests.md`.

## Acceptance Boundary

Phase 4C accepts the MQTT supervisory software contract, automated runtime, live protocol behavior, and bounded unloaded low-speed motor tests. It does not accept a pump head, hose, tubing, fluid, chemical, hydrogen peroxide, pressure, flow, calibration, loaded torque, process operation, or endurance. Generated output is not physical feedback, and MQTT E-stop is not a safety-rated physical E-stop.
