# Phase 4B.3a — Implementation Plan: Wi-Fi Failure Observability and Recovery

The goal of this micro-phase is to add detailed Wi-Fi failure classification, bounded automatic retries, and a manual reconnection API to the network manager (`argus_net_mgr`), and to expose this state on the controller dashboard (`argus_http_server`).

## Open Questions
- Should the `/api/network/reconnect` endpoint be available immediately upon entering the `ACTION_REQUIRED` state, or should it be bounded by a rate-limit (e.g. no more than 1 request per 5 seconds) to prevent operator spamming?
- Do you want to expose a separate HTTP endpoint like `GET /api/network/status` to poll network state specifically, or should the existing `GET /api/status` endpoint carry the new Wi-Fi telemetry? (Assuming we will append to `GET /api/status` to avoid adding another polling request).

## Proposed Changes

### Network Manager

#### [MODIFY] [argus_net_mgr.h](file:///C:/Users/bount/Dev/Argus/ArgusControl_PumpController-V2/main/argus_net_mgr.h)
- Introduce `argus_sta_state_t` enum (`DISABLED`, `IDLE`, `CONNECTING`, `ASSOCIATED`, `CONNECTED`, `WAIT_RETRY`, `ACTION_REQUIRED`).
- Introduce `argus_wifi_failure_t` enum (`NONE`, `AUTH_FAILED`, `AP_NOT_FOUND`, `IP_TIMEOUT`, `UNKNOWN`).
- Add `sta_state`, `last_wifi_failure`, `consecutive_failures`, `retry_countdown_s`, and `operator_action_required` to `argus_net_snapshot_t`.
- Expose a new API `argus_net_mgr_request_reconnect()` to queue a manual reconnect event.
- Add `ARGUS_NET_EVT_WIFI_RETRY` and `ARGUS_NET_EVT_MANUAL_RECONNECT` to the event queue.

#### [MODIFY] [argus_net_mgr.c](file:///C:/Users/bount/Dev/Argus/ArgusControl_PumpController-V2/main/argus_net_mgr.c)
- Modify Wi-Fi event handlers to classify disconnect reasons (e.g., `WIFI_REASON_AUTH_EXPIRE`, `WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT`, `WIFI_REASON_NO_AP_FOUND`).
- Create an internal bounded timer (`s_wifi_retry_timer`) triggered by disconnects or IP timeouts.
- Implement the retry logic: 15s retry for transient/unknown/not-found. Max 3 attempts for Auth failures, then transition to `ACTION_REQUIRED` and suppress the timer.
- Allow `ARGUS_NET_EVT_MANUAL_RECONNECT` to bypass the `ACTION_REQUIRED` state and immediately trigger a reconnection attempt.
- Ensure all timers are properly stopped and state is reset upon entering Local Service or transitioning to AP-only mode.
- Maintain existing authority integrity: MQTT broker and Authority are only granted when IP is fully acquired, and immediately revoked on disconnect.

### HTTP Server & Dashboard

#### [MODIFY] [argus_http_server.c](file:///C:/Users/bount/Dev/Argus/ArgusControl_PumpController-V2/main/argus_http_server.c)
- **API `GET /api/status`**: Append `sta_state`, `sta_failure_reason`, `retry_in_s`, `action_req`, and consecutive failure count to the JSON response.
- **API `POST /api/network/reconnect`**: Add a new authenticated endpoint returning `202 Accepted` if a reconnect event is successfully queued, `409 Conflict` if already connected or in local service, and `503 Service Unavailable` if the queue is full.
- **`PORTAL_HTML`**: Expand the Network rows to show human-readable connection status, failure reasons, countdowns, and an actionable "Reconnect Wi-Fi" button that calls the new POST endpoint.

### Pure Testing

#### [MODIFY] [argus_tests_4a.c](file:///C:/Users/bount/Dev/Argus/ArgusControl_PumpController-V2/main/argus_tests_4a.c)
- Add new test cases to explicitly verify:
  - Disconnect reason classification sets the correct internal failure enum.
  - 15-second retry timer is initialized and triggers correctly.
  - 3-attempt limit for authentication failures properly enters the `ACTION_REQUIRED` state and stops the timer.
  - Manual reconnect API transitions state back to `CONNECTING` and resets counters.
  - Retry logic is correctly suppressed during Local Service (`SERVICE_AP_ONLY`).
  - Authority and Broker state remain safely decoupled until IP acquisition.

## Verification Plan

### Automated Tests
- Build with ESP-IDF v5.5.3 (`idf.py fullclean`, `idf.py build`, `idf.py size`).
- Execute `idf.py size` to confirm footprint bounds and OTA-slot headroom.
- Ensure the pure-test suite remains fully green, with dynamically derived test counts, verifying all new state transitions and timer boundaries without mutating production state.

### Manual Verification
- Produce `docs/Phase 4B.3a Physical Tests.md` specifying the 10 manual verification steps.
- Present implementation for physical acceptance (providing the commit SHA, no rewriting of history, and awaiting explicit operator confirmation before completing).
