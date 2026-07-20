# Phase 4B.3a - Wi-Fi Observability and Recovery

## Acceptance status

Phase 4B.3a is implemented on `phase4b3a-wifi-observability` and is awaiting an independent source review followed by physical runtime verification. A clean compile is not runtime-test evidence. This document does not claim readiness to flash, physical acceptance, or a final passing test count.

## Runtime design

The network manager exposes STA lifecycle state, raw ESP-IDF disconnect reason, translated failure category, consecutive-failure count, retry countdown, operator guidance, and whether manual reconnect is currently permitted.

Wi-Fi configuration apply is a nonblocking generation-tagged transaction with these states:

`IDLE`, `PREPARING`, `WAITING_DISCONNECT`, `APPLYING_CONFIG`, `CONNECTING`, `COMPLETE`, `FAILED`, and `CANCELLED`.

The transaction stops retry/IP timers, invalidates prior timer generations, revokes authority to `NONE/NONE`, stops and verifies the MQTT broker, loads and validates commissioned configuration, and preserves validated credentials across an intentional disconnect. A matching disconnect event resumes configuration application and connection. Successful IP acquisition completes the transaction and clears active failure evidence. All terminal failures and cancellation paths erase staged password material.

Raw disconnect classification remains independent of intent. In particular, raw `WIFI_REASON_ASSOC_LEAVE` remains `AP_UNAVAILABLE`. It bypasses failure accounting only when consumed by the active generation-matched intentional-disconnect transaction.

## Manual reconnect policy

Manual reconnect requires a commissioned-capable network mode and valid commissioned credentials. It is permitted in `ACTION_REQUIRED`, `RETRY_WAIT`, and `IDLE`; `IDLE` additionally depends on valid credentials. It is rejected in `DISABLED`, `CONNECTING`, `ASSOCIATED_WAITING_IP`, `CONNECTED`, `SERVICE_TRANSITION`, and `SERVICE_AP_ONLY`.

A permitted reconnect uses the shared recovery transaction, preserves the prior disconnect reason/category/counters while recovery is pending, and clears active failure evidence only after successful IP acquisition. The AP and HTTP portal remain available because recovery changes the STA lifecycle without leaving `AP_DISCOVERABLE`.

## Authority policy

Only these mode/owner pairs are valid:

- `NONE/NONE`
- `SUPERVISORY/MQTT`
- `SERVICE_TRANSITION/NONE`
- `LOCAL_SERVICE/BROWSER`
- `LOCAL_SERVICE/DIAGNOSTIC_CLI`

The pure validator is enforced before production authority state changes. Invalid requests preserve mode, owner, and generation.

## Test and evidence correction history

Earlier Phase 4B.3a passes contained or reported missing Wi-Fi observability, undefined dashboard fields, invalid embedded JavaScript, a synchronous disconnect/connect race, no durable pending transaction, invalid authority pairing, placeholder tests, a pure test that mutated the production authority singleton, contradictory 97/98/119/120 test counts, ESP-IDF v5.5.1 build-log contamination, and premature physical-readiness language. Those failures remain part of the record; this closure does not rewrite them as first-pass success.

The pure suite is launched manually with diagnostic option `t`; it does not execute automatically during boot. The source-derived registration count is provisional and must not be promoted to a final count or passing result until the corrected suite executes on hardware.

## Build verification

The closure candidate was full-clean built and sized with ESP-IDF v5.5.3:

- Build result: success
- Compiler warnings: 0
- Compiler errors: 0
- Application image: `0xf5a80` bytes
- Smallest application partition: `0x300000` bytes
- OTA headroom: `0x20a580` bytes (68% free)

These are build facts only. They do not establish a pure-suite runtime pass or physical acceptance.

## Remaining acceptance

1. Complete an independent read-only source review.
2. Flash only after that review authorizes physical testing.
3. Run diagnostic option `t` and record the runtime-derived distinct/execution/pass/fail counts.
4. Execute the physical checklist and preserve serial/dashboard evidence.
5. Claim acceptance only after the physical results are reviewed.
