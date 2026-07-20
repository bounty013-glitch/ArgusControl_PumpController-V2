# Phase 4B.3a - Implementation Plan: Wi-Fi Failure Observability and Recovery

## Objective

Provide truthful Wi-Fi failure telemetry and recovery while preserving the always-on Service AP, exclusive authority, nonblocking event handling, pure-test isolation, and operator evidence until recovery is proven.

## Runtime transaction

Configuration apply and manual reconnect share a caller-owned, generation-tagged transaction core. Production owns one transaction instance; pure tests own stack-local instances with injected operations.

The apply sequence is:

1. Reject a duplicate active transaction.
2. Stop retry and IP-timeout timers and invalidate old generations.
3. Revoke authority to `NONE/NONE`.
4. Stop the MQTT broker and verify it stopped.
5. Load and validate commissioned configuration, including identity invariants, SSID bounds, WPA2 password bounds, and mask-sentinel rejection.
6. If connected, stage the validated configuration, mark the disconnect intentional, request disconnect, and return to the event loop.
7. Resume only from a generation-matched disconnect event.
8. Apply STA configuration, erase staged password material, and request connection.
9. Complete only after a generation-matched IP-acquisition event.
10. On every failure or cancellation, report the originating error and erase staged secrets.

## Manual reconnect policy

Manual reconnect is allowed in `ACTION_REQUIRED`, `RETRY_WAIT`, and `IDLE` when valid commissioned credentials exist and the network mode is `COMMISSIONED_STA`, `AP_DISCOVERABLE`, or `NETWORK_FAULT`. It is rejected in `DISABLED`, `CONNECTING`, `ASSOCIATED_WAITING_IP`, `CONNECTED`, `SERVICE_TRANSITION`, and `SERVICE_AP_ONLY`.

For permitted states the STA is already disconnected, so the transaction connects without issuing a redundant disconnect. It does not reapply credentials. Previous failure reason, category, counts, and guidance remain visible while recovery is pending and clear only after IP acquisition.

## Timer policy

Each timer captures the active generation when scheduled. Callback events carry that captured value, and consumers reject stale values. Apply, reconnect, service entry, successful connection, and cancellation invalidate prior generations. Timer command failures are observable and a retry state is not claimed if scheduling fails.

## Pure verification

All Phase 4B.3a tests use caller-owned authority, transaction, evidence, and callback-trace state. Tests must cover valid/invalid authority pairs, asynchronous apply ordering, stale events, all required callback failures, configuration validation, secret clearing, successful completion, duplicate requests, reconnect policy, evidence retention, mixed failure streaks, countdown boundaries, queue truthfulness, service cancellation, and AP/HTTP preservation.

Every test is registered through `RUN_TEST()`. The source registration count remains provisional until diagnostic option `t` executes on hardware. Compilation does not establish a runtime pass.
