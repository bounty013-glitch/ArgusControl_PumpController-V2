# Phase 4B.4 - Implementation Plan: Browser-Local Motion Command API

**Status:** STEP 1 STRICT REQUEST CONTRACT AND PURE DECODER IMPLEMENTED - RUNTIME EXECUTION PENDING

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

## Planned endpoint

`POST /api/command` will use bounded request handling and strict JSON parsing. The accepted schema must reject malformed JSON, unknown or duplicate fields, wrong JSON types, missing required fields, out-of-range values, trailing data, and unsupported commands. Error responses must be truthful and must not dispatch a partial or defaulted command.

The command vocabulary will cover:

- Setpoint
- Start
- Normal stop
- Unlock
- E-stop
- Reset E-stop
- Recovery

The request contract is frozen as follows. The response schema remains future work and will be fixed before the handler is registered.

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

Step 1 adds nine distinct decoder tests to the inherited 142-test baseline. The registered suite now contains 151 distinct tests repeated three times, for 453 expected executions. Runtime results remain pending until diagnostic option `t` executes on the controller.

## Implementation boundary

Step 1 adds only the frozen request contract, pure decoder, and pure tests. It does not add or register `POST /api/command`, read live authority or network state, attach source or generation, construct a production envelope, dispatch a command, call the state manager, or mutate motor, trajectory, timer, GPIO, network, or production singleton state.

Remaining Phase 4B.4 work includes the admission gate, authentication integration, server-owned source and generation attachment, envelope construction, exclusive router dispatch, HTTP handler and registration, browser integration, runtime suite execution, and physical acceptance.

## Step 0 static verification

The identity-only candidate was full-clean built and sized with ESP-IDF v5.5.3. All 1,096 build commands completed with zero compiler warnings, zero compiler errors, and zero failed commands. The application image is `0xfab30` bytes in the `0x300000`-byte smallest application partition, leaving `0x2054d0` bytes (67%) OTA headroom.

This build verifies only the Phase 4B.4 identity baseline. Diagnostic option `t` has not been executed for this identity candidate, no Phase 4B.4 functional tests exist yet, and no physical or acceptance result is claimed.

## Step 1 static verification

The final Step 1 source was full-clean built and sized with ESP-IDF v5.5.3 using the serial, ccache-disabled build method. All 1,098 build commands completed with zero compiler warnings, zero compiler errors, and zero failed commands. The application image is `0xfce20` bytes in the `0x300000`-byte smallest application partition, leaving `0x2031e0` bytes (67%) OTA headroom. `idf.py size` reported a total image size of 1,035,685 bytes.

Source registration reports 151 distinct tests and 453 expected executions across three passes. The decoder object has unresolved references only to `memcmp`, `memset`, and `strcmp`; the decoder-test object references only the decoder plus standard memory/string routines. Source and object audits found no HTTP, authority, router, state-manager, FreeRTOS, network, motor, trajectory, timer, or GPIO dependency, and no `/api/command` endpoint registration exists.

Because this step explicitly prohibits flashing, the updated on-device diagnostic suite and production-isolation snapshot have not yet executed for the Step 1 binary. The inherited 142-test baseline remains physically verified, but 151/151 and 453/453 are not claimed. Runtime execution remains a required gate before physical acceptance.
