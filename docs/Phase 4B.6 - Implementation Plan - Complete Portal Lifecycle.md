# Phase 4B.6 - Implementation Plan: Complete Portal Lifecycle

**Status:** COMPLETE AND ACCEPTED - July 22, 2026

## Accepted Baseline and Implementation

- Accepted starting main: `0ab8cbee5aa64d3ded695bbc75ad09c1f0583439`
- Accepted starting tag: `v2-phase4b.5-hardware-verified`
- Feature branch: `phase4b6-portal-lifecycle-acceptance`
- Firmware identity: `v2-phase4b.6-dev`
- Identity commit: `75ebd88`
- Factory-reset implementation: `512f433`
- Lifecycle tests: `9549dde`
- Full evidence: [Phase 4B.6 Tests.md](Phase%204B.6%20Tests.md)

Phase 4B.5 browser controls, E-stop orchestration, and powered UI-to-motor evidence remain accepted and were not repeated. Phase 4B.6 issued no motion command.

## Completed Scope

Phase 4B.6 completed the technician service portal as one coherent lifecycle:

1. Preserved identity and Wi-Fi configuration behavior.
2. Proved identity's permanent-until-configuration-reset lock.
3. Proved same-SSID omitted-password preservation.
4. Proved coordinated restart.
5. Proved explicit Local Service entry and exclusive browser authority.
6. Proved Local Service exit and normal supervisory recovery.
7. Added authenticated `POST /api/factory-reset`.
8. Proved truthful uncommissioned reboot with portal credentials preserved.
9. Restored the exact identity once and proved it relocked.
10. Restored the exact STA configuration through secure local transfer.
11. Completed a final Local Service round trip and stable commissioned recovery.

## Accepted Endpoint Contract

The configuration contract is `POST /api/config/save` with `scope` equal to `identity` or `wifi`. Same-SSID omitted-password saves preserve the existing password, mask strings are never accepted as passwords, identity locks after first provisioning, and uncommissioned Wi-Fi save queues runtime apply. Obsolete draft stage/validate/apply routes were not introduced.

`POST /api/factory-reset` requires valid Basic authentication, exact JSON confirmation `{"confirm":"FACTORY_RESET"}`, `application/json`, `SERVICE_AP_ONLY`, `LOCAL_SERVICE/BROWSER`, no conflicting transition, and a stationary restart-safe machine. Missing, malformed, duplicated, incorrect, oversized, truncated, timed-out, or uncertain requests reject without queueing reset.

## Accepted Lifecycle Architecture

The HTTP handler performs bounded authentication, body handling, decoding, policy evaluation, queueing, and response only. One accepted request queues one event and returns HTTP 202 before disconnect. The network-manager task owns lifecycle serialization, safety and authority revalidation, safe output establishment, HTTP shutdown, authority revocation, durable NVS reset, and reboot.

The production handler has no direct NVS erase, HTTP stop, or reboot call. The network event path contains exactly one live factory-reset executor call. Failure paths remain truthful and fail closed.

## Factory-Reset Scope

Configuration factory reset erases both dual-slot configuration records, selector metadata, the identity provisioning high-water marker, identity configuration, and STA SSID/password. It preserves the reset-pending namespace for durable recovery and preserves the dedicated `argus_portal` authentication namespace.

After reset, the controller boots `UNCOMMISSIONED_AP` with no authority, stopped broker, safe stationary output, no STA configuration, and identity eligible for first provisioning. Hardware identity and truthful effective display defaults remain available. Unified portal-credential recovery remains deferred to Phase 4D hardening.

## Browser Experience

The dashboard now provides authoritative state-eligible controls for identity, Wi-Fi, coordinated restart, Local Service entry/exit, configuration factory reset, motion-controls navigation, password change, and logout. Factory reset requires a button click and acceptance of the exact scope warning. Pending lifecycle actions suppress duplicates; transport loss is presented as unknown until authoritative state is reacquired.

## Validation Summary

- ESP-IDF v5.5.3 full-clean no-ccache build: PASS, 0 warnings, 0 errors.
- Binary: `0x10a750`; OTA headroom: `0x1f58b0` bytes (65%).
- Host/source boundary tests: PASS.
- Three genuine-ConPTY diagnostic invocations: 177 distinct tests, 531/531 each, 1,593/1,593 aggregate.
- Live restart, service entry/exit, production UI factory reset, uncommissioned recovery, portal-password preservation, identity restoration/relock, Wi-Fi restoration, and final service round trip: PASS.
- Panic/reset-loop/watchdog/brownout/assertion/canary/heap/task-leak audit: PASS.
- Final state: commissioned, `AP_DISCOVERABLE`, `SUPERVISORY/MQTT`, broker running, `UNLOCKED`, zero output, driver disabled.

## Safety and Exclusions

No motion command or powered motor test occurred. Existing Phase 4B.5 powered evidence was incorporated without repetition. No pump head, hose, tubing, fluid, chemical, pressure, flow calibration, loaded performance, endurance, Phase 4C MQTT contract, or Phase 4D security redesign is included.

## Commit Progression

1. `75ebd88` - `chore: establish Phase 4B.6 identity`
2. `512f433` - `feat: add coordinated portal factory reset`
3. `9549dde` - `test: cover Phase 4B.6 portal lifecycle`
4. Final acceptance documentation - this closure pass

The Phase 4B stop gate is satisfied. Phase 4B is complete and accepted, and the repository is ready to begin Phase 4C after merge and annotated acceptance tag.
