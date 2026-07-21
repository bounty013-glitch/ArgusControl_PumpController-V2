# Phase 4B.4 - Implementation Plan: Browser-Local Motion Command API

**Status:** STEP 0 IDENTITY ESTABLISHED - FUNCTIONAL IMPLEMENTATION NOT STARTED

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

The exact request and response schema will be fixed during implementation review before the handler is registered.

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

The Phase 4B.4 test count is not established yet. The inherited baseline remains 142 distinct tests repeated three times, 426 executions, pending the first Phase 4B.4 test additions.

## Implementation boundary

This Step 0 commit changes identity and planning records only. It does not add or register `POST /api/command`, parse motion requests, construct command envelopes, dispatch browser commands, or claim Phase 4B.4 functional, runtime-suite, physical, or acceptance results.

## Step 0 static verification

The identity-only candidate was full-clean built and sized with ESP-IDF v5.5.3. All 1,096 build commands completed with zero compiler warnings, zero compiler errors, and zero failed commands. The application image is `0xfab30` bytes in the `0x300000`-byte smallest application partition, leaving `0x2054d0` bytes (67%) OTA headroom.

This build verifies only the Phase 4B.4 identity baseline. Diagnostic option `t` has not been executed for this identity candidate, no Phase 4B.4 functional tests exist yet, and no physical or acceptance result is claimed.
