# Phase 4B.3 — Browser Service-Entry and Authority Ownership

## Summary

Phase 4B.3 adds two HTTP endpoints that allow the browser portal operator to explicitly
enter and exit local service mode, transferring pump command authority from MQTT to the
browser and back.

The core orchestration (`argus_net_mgr_request_service` and
`argus_net_mgr_request_service_exit`) is already fully implemented and physically
verified. Phase 4B.3 wires the HTTP handlers.

---

## Prerequisite: App Partition Size Increase

> [!IMPORTANT]
> The current 1MB app partition has only **6% free** (62,528 bytes).
> Phase 4B.3 adds new handlers, and Phase 4B.4/4B.5 will add motion commands
> and the full embedded UI. We will exceed 1MB.

### Proposed Change

Increase the app partition from **1MB → 2MB** by updating `partitions.csv`.
This requires:
1. Update `partitions.csv` — double the `factory` partition size
2. Verify the flash chip is large enough (ESP32-P4 typically has 4MB+ flash)
3. Full clean build + `idf.py size` to confirm
4. Flash with the new partition table (requires full erase or `idf.py flash`)

### Open Question

> [!IMPORTANT]
> **Shawn:** What is the total flash size on the target ESP32-P4 board?
> If 4MB, we can safely go to 2MB app. If 8MB or 16MB, even more headroom.
> Also — do you want OTA (dual app partitions) in the future? If so, we need
> to plan the partition layout now rather than use all available space for a
> single factory partition.

---

## Proposed Changes

### Handler Architecture (A1 Compliance)

Per architectural amendment A1, HTTP handlers **must not** call blocking orchestration
directly — `argus_net_mgr_request_service()` takes `s_net_mutex`, executes 10 blocking
transition steps, and can take seconds. Calling it from an httpd handler task risks:

- HTTP recv/send timeout (5s) expiring before the transition completes
- The HTTP server's handler thread being blocked for the entire transition duration

**Two design options:**

#### Option A: Direct Call from Handler (Simpler)

The HTTP handler calls `argus_net_mgr_request_service()` synchronously. The handler
blocks until the transition completes (typically 2-3 seconds). This works because:
- `request_service` already releases `s_net_mutex` before calling `httpd_stop()`
- The httpd server has a 5s recv/send timeout — the transition fits within this
- ESP-IDF's httpd dispatches handlers on a dedicated thread

**Risk:** If the transition exceeds 5s (unusual), the HTTP response may not reach the
browser. The transition still completes (owned by net_mgr, not the HTTP session per A2).

#### Option B: Event-Driven (Async)

The HTTP handler posts an event to the net_mgr task queue, returns `202 Accepted`
immediately, and the browser polls `/api/status` for the transition result.

**Risk:** More complex. Requires a poll mechanism. The browser doesn't get immediate
confirmation.

**Recommendation:** Option A (direct call). The transition is bounded (~3s) and fits
within the HTTP timeout. The browser gets a definitive result. A2 already protects
against browser disconnect during transition.

---

### `POST /api/service/enter` Handler

**File:** [argus_http_server.c](file:///c:/Users/bount/Dev/Argus/ArgusControl_PumpController-V2/main/argus_http_server.c)

**Mode gate:** `AP_DISCOVERABLE` only (per endpoint contract §10)

**Sequence:**
1. Verify Basic Auth credentials
2. Verify request method is POST (405 otherwise)
3. Get current network snapshot — reject if not `AP_DISCOVERABLE` (403)
4. Call `argus_net_mgr_request_service(ARGUS_AUTH_OWNER_BROWSER)`
5. On success: return `200 OK` with JSON `{"status":"ok","mode":"SERVICE_AP_ONLY","owner":"BROWSER"}`
6. On `ESP_ERR_INVALID_STATE`: return `409 Conflict` with JSON error (already transitioning, wrong mode, or unsafe machine)
7. On `ESP_ERR_INVALID_ARG`: return `400 Bad Request`
8. On other error: return `503 Service Unavailable`

**Response schema:**
```json
// Success
{"status":"ok","mode":"SERVICE_AP_ONLY","owner":"BROWSER"}

// Already in service mode (idempotent check before calling)
{"status":"ok","mode":"SERVICE_AP_ONLY","owner":"BROWSER","note":"already_in_service"}

// Error
{"status":"error","code":409,"reason":"mode_not_ap_discoverable"}
{"status":"error","code":409,"reason":"transition_in_progress"}
{"status":"error","code":503,"reason":"transition_failed","detail":"<esp_err_name>"}
```

**Idempotency:** Before calling the orchestrator, check if already in `SERVICE_AP_ONLY`
with `LOCAL_SERVICE/BROWSER` authority. If so, return success immediately (no duplicate
transition).

---

### `POST /api/service/exit` Handler

**Mode gate:** `SERVICE_AP_ONLY` only (per endpoint contract §10)

**Sequence:**
1. Verify Basic Auth credentials
2. Verify request method is POST (405 otherwise)
3. Get current network snapshot — reject if not `SERVICE_AP_ONLY` (403)
4. Get current authority snapshot — reject if not `LOCAL_SERVICE/BROWSER` (403)
5. Call `argus_net_mgr_request_service_exit(ARGUS_AUTH_OWNER_BROWSER)`
6. On success: return `200 OK` with JSON `{"status":"ok","action":"rebooting"}`
7. On error: return `503 Service Unavailable` with JSON error
8. Service exit triggers a reboot — the browser will lose connection (expected)

**Response schema:**
```json
// Success (browser will lose connection after this response)
{"status":"ok","action":"rebooting"}

// Error
{"status":"error","code":403,"reason":"not_in_service_mode"}
{"status":"error","code":403,"reason":"not_browser_owner"}
{"status":"error","code":503,"reason":"exit_failed","detail":"<esp_err_name>"}
```

**Note:** The service exit performs a controlled reboot. The HTTP response must be sent
before the reboot occurs. The existing `argus_restart_execute()` pattern (200ms grace)
should be used for the reboot delay.

---

### URI Registration

Add two new `httpd_uri_t` entries and register them in `argus_http_server_start()`.
Update `max_uri_handlers` if needed (currently 16, we'll use 13 after this).

---

## Files Changed

| File | Changes |
|------|---------|
| [MODIFY] [argus_http_server.c](file:///c:/Users/bount/Dev/Argus/ArgusControl_PumpController-V2/main/argus_http_server.c) | Add `service_enter_handler`, `service_exit_handler`, URI registrations |
| [MODIFY] [partitions.csv](file:///c:/Users/bount/Dev/Argus/ArgusControl_PumpController-V2/partitions.csv) | Increase app partition to 2MB (pending flash size confirmation) |

---

## Verification Plan

### Static Verification
1. `git diff --check` — clean
2. Full clean build — zero errors, zero warnings
3. Binary size within new partition budget
4. Test count unchanged or legitimately increased
5. Zero production singleton calls in tests

### Physical Verification
1. Flash to device
2. Connect phone to Service AP
3. Browse to portal
4. Verify `POST /api/service/enter` transitions to `SERVICE_AP_ONLY` / `LOCAL_SERVICE/BROWSER`
5. Verify `/api/status` reflects new authority state
6. Verify `POST /api/service/exit` triggers clean reboot
7. Verify device recovers to `AP_DISCOVERABLE` or `COMMISSIONED_STA` after reboot
8. Verify duplicate service-enter is idempotent
9. Verify service-enter rejected in wrong mode
