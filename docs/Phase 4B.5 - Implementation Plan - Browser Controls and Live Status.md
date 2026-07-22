# Phase 4B.5 - Implementation Plan: Browser Controls and Live Status

**Status:** IMPLEMENTED - SOFTWARE VALIDATED; CONTROLLER VALIDATION BLOCKED ON ISOLATION EVIDENCE

## Authorized Baseline

- Accepted Phase 4B.4 record head: `4e7d46c428b20c6e4a1ffa15ed1da59f3280e8ee`
- Accepted Phase 4B.4 implementation: `1b701e5ffdf820a468070dc1f1a54d129a9537d0`
- Phase 4B.5 firmware identity: `v2-phase4b.5-dev`
- Working branch: `phase4b5-browser-controls-live-status`

## Current Step

Implement the real technician-oriented browser motion controls on a dedicated authenticated page and project continuously refreshed authoritative controller status from `GET /api/status`.

The page must submit only the seven accepted command schemas to `POST /api/command`. The browser supplies no source, authority owner, or authority generation and owns no machine state. The accepted command router remains the sole production path to the state and motion system.

Implementation commit: `594445b42d66ada780fd1e34f5084b0a2bab96ac`

## Implemented Surface

- Authenticated route: `GET /controls`
- Portal navigation: visible `Motion Controls` link from the authenticated dashboard
- Commands: `set_target`, `start`, `stop`, `unlock`, `estop`, `reset_estop`, and `recover`
- Command transport: one same-origin `POST /api/command` per accepted user action
- Status transport: one same-origin `GET /api/status` request in flight at a time
- Identity transport: authenticated `GET /api/identity`
- Normal poll interval: 750 ms
- Hidden-document poll interval: 5,000 ms
- Request timeout: 2,000 ms
- Stale threshold: 3,000 ms

Polling starts when the page loads, slows while the document is hidden, resumes immediately when visible, and stops on page unload. Command completion requests an immediate status reconciliation without overlapping an in-flight poll. No command response mutates displayed machine state optimistically.

The status schema now includes authoritative requested and applied direction, fault code, command generation, feedback availability, and bounded escaped rejection detail in addition to the existing machine state, target, applied/generated speed, driver, E-stop, ramp, network, broker, and authority fields. Controller and firmware metadata are projected separately from `/api/identity`. No credential or secret field is exposed.

## Eligibility And Failure Behavior

Ordinary controls require fresh status confirming `SERVICE_AP_ONLY` and `LOCAL_SERVICE/BROWSER` authority. Start and setpoint controls are disabled for stale, disconnected, unauthorized, E-stop-latched, faulted, or otherwise ineligible states. Stop, unlock, reset, and recovery follow the current authoritative state. E-stop remains prominent and attemptable during stale or disconnected status while authentication remains valid.

The page distinguishes current, fetching, stale, disconnected, unauthorized, and invalid status; preserves last-known values only with explicit stale treatment; and reports command pending, accepted, rejected, malformed-response, timeout, and transport-failure outcomes separately from controller state. HTTP 200 is described as API admission, not physical-motion proof.

## Software Validation Record

Validated on July 21, 2026 against implementation commit `594445b42d66ada780fd1e34f5084b0a2bab96ac`:

- Host controls test: PASS, including exact milli-RPM conversion, all documented command response classes, duplicate-submission suppression, invalid-input non-dispatch, no optimistic state, stale gating, and E-stop availability behavior.
- Rendered browser review: PASS at 1280 x 800 and 390 x 844; no horizontal overflow or incoherent overlap was observed, and the prominent E-stop remained in the first control viewport.
- Source boundaries: one existing production router dispatch call; no controls-page call to router, state manager, trajectory, pulse engine, motor, or GPIO APIs; no browser-supplied source, owner, or generation.
- ESP-IDF: v5.5.3.
- Build: full-clean, ccache disabled, PASS with zero compiler warnings and zero compiler errors.
- Application binary: `0x107410` bytes.
- Smallest OTA slot: `0x300000` bytes; headroom `0x1f8bf0` bytes (66%).
- Diff/credential/temp-artifact audit: PASS.

Four registered Phase 4B.5 pure tests increase the previous 163-test baseline to 167 distinct tests. The expected controller result is three internal repeat passes, 501 total executions, 501 passed, and zero failed. Those numbers are registration expectations only; no Phase 4B.5 controller execution is claimed in this record.

## Controller Validation Gate

No COM port was opened, no image was flashed, and no live browser command or diagnostic option was sent during this pass. Existing records establish that the motor was disconnected during accepted Phase 4B.4 automated work, but they do not positively establish the current required electrical isolation of STEP, DIR, and ENA from the driver. The unavailable power rig and a disconnected motor are not substitutes for that evidence.

Motor-disconnected controller validation remains pending until STEP/DIR/ENA isolation is physically verified and recorded. At that point the corrected image must receive boot/stability observation, three complete 167-test/501-execution pure-suite invocations, authenticated controls-page and automatic-status checks, and only then isolated live command/state-machine checks. No motion-capable browser command may be sent before that verification.

## Safety Boundary

Development and any later automated controller validation are motor-disconnected. No connected-motor, pump, hose, chemical, pressure, process, flow-accuracy, or mechanical-endurance acceptance is claimed. The narrow powered UI-to-motor confirmation remains a later Phase 4B.5 acceptance dependency and must use the final controls implemented by this phase.

## Step 0 Identity Record

Before functional implementation, the project version, firmware fallback identity, startup phase labels, pure-suite labels, README status, master Phase 4B plan, and this active implementation record identify Phase 4B.5.
