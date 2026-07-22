# Phase 4B.5 - Implementation Plan: Browser Controls and Live Status

**Status:** COMPLETE AND ACCEPTED - Powered UI-to-motor integration passed July 22, 2026

## Authorized Baseline

- Accepted Phase 4B.4 record head: `4e7d46c428b20c6e4a1ffa15ed1da59f3280e8ee`
- Accepted Phase 4B.4 implementation: `1b701e5ffdf820a468070dc1f1a54d129a9537d0`
- Phase 4B.5 firmware identity: `v2-phase4b.5-dev`
- Working branch: `phase4b5-browser-controls-live-status`

## Current Step

Implement the real technician-oriented browser motion controls on a dedicated authenticated page and project continuously refreshed authoritative controller status from `GET /api/status`.

The page must submit only the seven accepted command schemas to `POST /api/command`. The browser supplies no source, authority owner, or authority generation and owns no machine state. The accepted command router remains the sole production path to the state and motion system.

Implementation commit: `594445b42d66ada780fd1e34f5084b0a2bab96ac`

Recovery deadlock correction: `efcc8a3eb2ede7279242a848936408287cd03f7b`

Browser E-stop pending-command correction: `666c1b0ee610c8041f8afd11bd41b3230e1eee5a`

Formal test and acceptance record: [Phase 4B.5 Tests.md](Phase%204B.5%20Tests.md)

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

Ordinary controls require fresh status confirming `SERVICE_AP_ONLY` and `LOCAL_SERVICE/BROWSER` authority. Start and setpoint controls are disabled for stale, disconnected, unauthorized, E-stop-latched, faulted, or otherwise ineligible states. Stop, unlock, reset, and recovery follow the current authoritative state. E-stop remains prominent and attemptable during stale or disconnected status while authentication remains valid. Separate ordinary and E-stop in-flight lanes permit one E-stop request while an ordinary request is pending, suppress repeated E-stop clicks, and prevent an older ordinary response from overwriting the newer E-stop result.

The page distinguishes current, fetching, stale, disconnected, unauthorized, and invalid status; preserves last-known values only with explicit stale treatment; and reports command pending, accepted, rejected, malformed-response, timeout, and transport-failure outcomes separately from controller state. HTTP 200 is described as API admission, not physical-motion proof.

## Software Validation Record

Validated on July 21, 2026 against implementation commit `594445b42d66ada780fd1e34f5084b0a2bab96ac`:

- Host controls test: PASS, including exact milli-RPM conversion, all documented command response classes, duplicate-submission suppression, invalid-input non-dispatch, no optimistic state, stale gating, and E-stop availability behavior.
- Rendered browser review: PASS at 1280 x 800 and 390 x 844; no horizontal overflow or incoherent overlap was observed, and the prominent E-stop remained in the first control viewport.
- Source boundaries: one existing production router dispatch call; no controls-page call to router, state manager, trajectory, pulse engine, motor, or GPIO APIs; no browser-supplied source, owner, or generation.
- ESP-IDF: v5.5.3.
- Build: full-clean, ccache disabled, PASS with zero compiler warnings and zero compiler errors.
- Final corrected application binary: `0x1073e0` bytes.
- Smallest OTA slot: `0x300000` bytes; headroom `0x1f8c20` bytes (66%).
- Diff/credential/temp-artifact audit: PASS.

Four registered Phase 4B.5 pure tests increase the previous 163-test baseline to 167 distinct tests.

## Motor-Disconnected Controller Validation

Validated on July 21-22, 2026. Before live motion-capable commands, the operator confirmed that the motor was fully isolated and physically absent from the bench. No motor, driver load, pump, or mechanical assembly was operated.

- COM5/chip verification: ESP32-S3 QFN56 revision 0.2, USB-Serial/JTAG, MAC `3c:dc:75:6e:c2:d0`.
- Firmware identity: `v2-phase4b.5-dev`.
- Final corrected binary SHA-256: `503B74BF873E0D6F2B6261FCF1DF7895A8E9D2F90E0FFA2BB97EB3844C37F378`.
- Flash: PASS with hash verification.
- Final pure-suite runs: three complete genuine Windows ConPTY-backed invocations, each 167 distinct tests, three internal repeat passes, 501/501 executions passed, zero failed; aggregate 1,503/1,503.
- Production isolation on every final invocation: authority generation, network state, broker state, machine state, and task count unchanged.
- Stability: no panic, unexpected reset, watchdog, brownout, assertion, stack-canary failure, heap corruption, or task leak.
- Browser preflight: authenticated `/controls` loaded with live automatic status, correct identity, `AP_DISCOVERABLE`, `SUPERVISORY/MQTT`, zero speeds, disabled driver, and ordinary browser commands correctly disabled.
- Local Service entry: `SERVICE_AP_ONLY`, `LOCAL_SERVICE/BROWSER`, STA disabled, broker stopped, and browser commands admissible.
- Isolated command checks: accepted target without output, forward and reverse controller-state runs, normal stop to `HOLDING`, unlock, E-stop priority, reset without automatic restart, and recovery to `UNLOCKED`; responses remained distinct from authoritative status and did not claim physical motion.
- Controlled service exit: PASS; the reboot restored `AP_DISCOVERABLE`, `SUPERVISORY/MQTT`, `UNLOCKED`, zero outputs, disabled driver, and the running broker without a reset loop.

## Recovery Deadlock Finding And Correction

The first live browser recovery request exposed a pre-existing self-deadlock in `argus_trajectory_recover()`: recovery held the non-recursive trajectory mutex and called the public `argus_trajectory_clear_error()`, which attempted to take the same mutex again. The API request could not complete, status polling became stale, and the HTTP task remained blocked. No panic, reset, unsafe motor behavior, or physical motion occurred.

Commit `efcc8a3eb2ede7279242a848936408287cd03f7b` introduced a lock-held error-clear helper. Recovery calls that helper while already holding the mutex, while the public clear function retains its existing lock-taking contract. A host regression fails if recovery again calls the public lock-taking function. The ESP-IDF v5.5.3 full-clean no-ccache build passed with zero compiler warnings and zero compiler errors, and the final three controller suites passed before the corrected live browser reproduction.

The exact corrected reproduction started from stationary `HOLDING` with zero generated and applied speed. One browser `Recover` action returned `Recover accepted by API`; authoritative status then reported `UNLOCKED`, zero generated/applied speed, disabled driver, clear E-stop, preserved `SERVICE_AP_ONLY` and `LOCAL_SERVICE/BROWSER`, and continued healthy polling.

## Safety Boundary

The earlier software, automated-runtime, and isolated browser validation remained motor-disconnected. On July 22, 2026, Shawn later completed the bounded connected-motor procedure through the final authenticated `/controls` page against firmware commit `666c1b0ee610c8041f8afd11bd41b3230e1eee5a`. Setpoint-without-motion, forward start and smooth ramp, normal stop, unlock, reverse direction, E-stop halt with truthful latch, and reset without automatic restart all passed. While E-stop was latched, disabled motion controls admitted no command and caused no movement. The completed record is [Phase 4B.5 Tests.md](Phase%204B.5%20Tests.md).

This grants Phase 4B.5 acceptance only for the implemented browser/UI-to-controller-to-motor boundary. No pump head, hose, tubing, fluid, chemical, pressure, flow-accuracy, calibration, loaded-torque, process, or mechanical-endurance acceptance is claimed.

## Step 0 Identity Record

Before functional implementation, the project version, firmware fallback identity, startup phase labels, pure-suite labels, README status, master Phase 4B plan, and this active implementation record identify Phase 4B.5.
