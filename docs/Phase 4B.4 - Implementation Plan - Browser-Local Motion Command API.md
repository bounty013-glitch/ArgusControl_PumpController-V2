# Phase 4B.4 - Implementation Plan: Browser-Local Motion Command API

**Status:** STEP 1 ACCEPTED - STEP 2 SOFTWARE-AND-AUTOMATED-RUNTIME-READY-FOR-REVIEW

## Autonomous execution status

| Field | Status |
|---|---|
| Accepted baseline | Step 1 corrective commit `eb1a6cc19ac0621a76b3b7996b8e89a976cab966` |
| Current authorized step | Phase 4B.4 Step 2 - authenticated HTTP admission and exclusive router dispatch |
| Current state | SOFTWARE-AND-AUTOMATED-RUNTIME-READY-FOR-REVIEW |
| Allowed autonomous actions | Source changes within Step 2; ESP-IDF v5.5.3 build; COM5 chip verification, flash, reboot, serial monitor, and automated on-controller tests with motor disconnected |
| Hard stops | Baseline conflict; unsafe or unidentified COM5 device; abnormal electrical/reset behavior; out-of-scope architecture or accepted safety-semantics change; connected-motor or human-observation requirement |
| Last validated commit | `99413f8` - Step 2 implementation; clean build and three controlled on-controller suite runs passed |
| Next required human action | Independent supervisory review of Phase 4B.4 Step 2 |
| Later human gate | Connected-motor and physical bench acceptance; no mechanical acceptance is authorized in this run |

## Objective

Add a browser-local motion command API without creating a second control path. Phase 4B.4 will expose `POST /api/command`, translate a strictly validated request into an `argus_command_envelope_t`, and submit that envelope only through the existing command router.

## Binding architecture

- Accept motion commands only while authority is exactly `LOCAL_SERVICE/BROWSER` and network mode is exactly `SERVICE_AP_ONLY`.
- Set the envelope source to `ARGUS_CMD_SRC_LOCAL_SERVICE_PORTAL`.
- Read and attach the current authority generation at dispatch time.
- Dispatch exclusively through `argus_cmd_router_dispatch()`.
- Do not call any motion-changing `argus_state_mgr_*` API from HTTP code.
- Preserve router serialization, authority validation, generation rejection, state-machine safety, and E-stop priority.
- Keep unrelated and deferred Wi-Fi enhancements outside Phase 4B.4.

## Implemented endpoint

`POST /api/command` will use bounded request handling and strict JSON parsing. The accepted schema must reject malformed JSON, unknown or duplicate fields, wrong JSON types, missing required fields, out-of-range values, trailing data, and unsupported commands. Error responses must be truthful and must not dispatch a partial or defaulted command.

The command vocabulary will cover:

- Setpoint
- Start
- Normal stop
- Unlock
- E-stop
- Reset E-stop
- Recovery

The request contract is frozen as follows. The registered handler uses the existing portal Basic Auth mechanism, bounded body reception, the accepted strict decoder, live network and authority snapshots, server-owned source and generation fields, and exactly one router-dispatch call for admitted requests.

Commands without arguments accept exactly one field:

```json
{"command":"start"}
{"command":"stop"}
{"command":"unlock"}
{"command":"estop"}
{"command":"reset_estop"}
{"command":"recover"}
```

Set-target accepts exactly three fields, in any order:

```json
{"command":"set_target","target_rpm_milli":8000,"forward":true}
```

`target_rpm_milli` is a JSON integer from 0 through 200000 inclusive, matching the authoritative state-manager range. Fractional, exponent-form, quoted, negative, and overflowing values are rejected. `forward` is a genuine JSON Boolean. Keys and command names use JSON string semantics and command names remain case-sensitive.

The decoder requires one complete top-level object; rejects missing, duplicate, unknown, extra, malformed, nested, array, null, and wrong-type values; detects escaped-key equivalence; and rejects trailing data. Leading and trailing JSON whitespace are accepted. Input is length-aware, capped at 192 bytes, and need not be NUL-terminated.

The browser must not provide source, authority generation, `pump_id`, `channel_id`, axis, or routing information. The future live admission layer owns `ARGUS_CMD_SRC_LOCAL_SERVICE_PORTAL` and the current authority generation. This remains a single-controller-per-pump architecture.

## Required pure verification

Before physical testing, pure tests must cover:

- Strict request parsing and rejection without dispatch
- Correct envelope construction for every supported command
- `ARGUS_CMD_SRC_LOCAL_SERVICE_PORTAL` source assignment
- Current authority-generation capture and stale-generation rejection
- Exact `SERVICE_AP_ONLY` plus `LOCAL_SERVICE/BROWSER` enforcement
- Rejection of every wrong mode, owner, or source combination
- Setpoint bounds and direction handling
- Start, normal stop, unlock, E-stop, reset E-stop, and recovery routing
- One router dispatch for each accepted request and zero dispatches for each rejected request
- Proof that the HTTP layer performs no direct motion-state mutation
- Production-state isolation for all pure tests

Step 1 added nine distinct decoder tests to the inherited 142-test baseline. Step 2 adds 12 endpoint, admission, envelope, response, and isolation groups. The registered suite now contains 163 distinct tests and performs 489 executions per diagnostic-option `t` invocation because the suite repeats all distinct tests three times.

## Implementation boundary

Step 1 adds only the frozen request contract, pure decoder, and pure tests. It does not add or register `POST /api/command`, read live authority or network state, attach source or generation, construct a production envelope, dispatch a command, call the state manager, or mutate motor, trajectory, timer, GPIO, network, or production singleton state.

Step 2 implements the admission gate, authentication integration, server-owned source and generation attachment, envelope construction, exclusive router dispatch, HTTP handler, and exact POST registration. Polished browser controls remain deferred to Phase 4B.5. Independent supervisory review, connected-motor testing, and physical acceptance remain pending.

## Step 0 static verification

The identity-only candidate was full-clean built and sized with ESP-IDF v5.5.3. All 1,096 build commands completed with zero compiler warnings, zero compiler errors, and zero failed commands. The application image is `0xfab30` bytes in the `0x300000`-byte smallest application partition, leaving `0x2054d0` bytes (67%) OTA headroom.

This build verifies only the Phase 4B.4 identity baseline. Diagnostic option `t` has not been executed for this identity candidate, no Phase 4B.4 functional tests exist yet, and no physical or acceptance result is claimed.

## Step 1 static verification

The final Step 1 source was full-clean built and sized with ESP-IDF v5.5.3 using the serial, ccache-disabled build method. All 1,098 build commands completed with zero compiler warnings, zero compiler errors, and zero failed commands. The application image is `0xfce20` bytes in the `0x300000`-byte smallest application partition, leaving `0x2031e0` bytes (67%) OTA headroom. `idf.py size` reported a total image size of 1,035,685 bytes.

Source registration reports 151 distinct tests and 453 expected executions across three passes. The decoder object has unresolved references only to `memcmp`, `memset`, and `strcmp`; the decoder-test object references only the decoder plus standard memory/string routines. Source and object audits found no HTTP, authority, router, state-manager, FreeRTOS, network, motor, trajectory, timer, or GPIO dependency, and no `/api/command` endpoint registration exists.

Because this step explicitly prohibits flashing, the updated on-device diagnostic suite and production-isolation snapshot have not yet executed for the Step 1 binary. The inherited 142-test baseline remains physically verified, but 151/151 and 453/453 are not claimed. Runtime execution remains a required gate before physical acceptance.

## Step 2 implementation and automated-runtime evidence

Implementation commit `99413f8` registers exactly `POST /api/command`. The production path checks existing portal authentication before body processing; receives at most 192 bytes without requiring NUL termination; decodes through the accepted Step 1 contract; requires exact `SERVICE_AP_ONLY` and `LOCAL_SERVICE/BROWSER` snapshots; captures the controller authority generation; zero-initializes the envelope; sets source to `ARGUS_CMD_SRC_LOCAL_SERVICE_PORTAL`; and invokes one live `argus_cmd_router_dispatch()` call. Rejected requests invoke no dispatch. Responses map to bounded JSON at 200, 400, 401, 403, 409, or 500 and carry `Cache-Control: no-store`.

The final ESP-IDF v5.5.3 full-clean build completed with zero compiler warnings and zero compiler errors. The application image is `0xfe410` bytes in the `0x300000`-byte smallest application partition, leaving `0x201bf0` bytes (67%) OTA headroom; `idf.py size` reported 1,041,313 bytes total image size.

After COM5 was verified as the intended ESP32-S3 QFN56 revision 0.2 controller with 8 MB PSRAM and USB-Serial/JTAG, the motor-disconnected controller was flashed and diagnostic option `t` was entered through a genuine Windows ConPTY-backed `idf.py monitor` session. Three final post-correction invocations each reported 163 distinct tests, three internal repeat passes, 489 executions, 489 passed, and 0 failed. Each invocation preserved authority generation 3, `AP_DISCOVERABLE`, broker `RUNNING`, machine `UNLOCKED`, and 16 tasks, then returned normally to the diagnostic prompt. No panic, post-input reset, watchdog, brownout, assertion, stack-canary, heap-corruption, or task-leak evidence appeared.

Recovery analysis preserved and decoded two earlier panic captures against their exact flashed ELF. The first was a BREAK/double-exception immediately after malformed duplicate fabricated ANSI cursor responses. The second was a LoadProhibited scheduler failure at `prvSelectHighestPriorityTaskSMP` with `EXCVADDR=0x44`; `0xA5A5A5A5` appeared only in unused caller-saved registers and is consistent with FreeRTOS stack-fill residue, not a dereferenced fault pointer. No suite-entry evidence preceded the second capture. Six later clean genuine-ConPTY runs, including the three final post-correction runs, support final classification as a headless terminal-emulation artifact that induced real scheduler corruption, not a Step 2 implementation or test-runner defect.

Adversarial review found and corrected one in-scope issue: oversized requests were rejected before decode but then drained for the client-declared length. The final handler sends a bounded 400 response, requests session closure, and performs no unbounded drain. The review also noted that real socket/auth and live-router execution are not part of the pure endpoint seam. Those paths retain the accepted production authentication and router implementations; inherited authority, router, E-stop, and state-core tests remain in the complete suite, while connected HTTP and motor behavior remain explicit later acceptance gates.
