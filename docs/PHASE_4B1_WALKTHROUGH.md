# Phase 4B.1 — Complete Walkthrough

## Summary

Phase 4B.1 delivers a credential-protected HTTP service portal for the Argus pump controller. The portal is accessible when the operator places the unit in Service/AP Discoverable mode (or from the commissioned LAN — see DHR-001), and provides real-time machine status, authority state, network diagnostics, and identity information through a dark-themed mobile-first web interface. The portal includes an NVS-mutating password-change endpoint and is therefore not entirely read-only.

**Closeout status: COMPLETE WITHIN OPERATOR-ACCEPTED DEVELOPMENT RISK SCOPE**

See `DEFERRED_HARDENING_REGISTER.md` for the authoritative record of accepted security limitations.

---

## Commits (chronological)

| Commit | Description |
|--------|-------------|
| `3ae9b22` | Initial HTTP server lifecycle and read-only portal |
| `47e8403` | First independent review — deadlock fix, AP filter, HTML escaping, URI rollback |
| `493ab7a` | Second independent review — coherent snapshot, stop_server_locked, test purity |
| `8e20299` | Fix json_escape truncation test assertion |
| `b187511` | Attempt AP-interface enforcement via getpeername (lwip returns 0.0.0.0) |
| `b5d4caf` | Move AP-subnet check to per-handler level (lwip still returns 0.0.0.0) |
| `92468a9` | Remove AP-subnet enforcement entirely (lwip platform limitation documented) |
| `e4a600e` | **Add HTTP Basic Auth credential protection** (final solution) |

---

## Architecture

### Endpoints

| Endpoint | Method | Auth | Description |
|----------|--------|------|-------------|
| `/` | GET | Basic Auth | Portal dashboard (or password-change page if default password active) |
| `/api/status` | GET | Basic Auth | Machine state, speeds, authority, network, broker status as JSON |
| `/api/identity` | GET | Basic Auth | Hardware UID, client/unit ID, firmware version, service SSID |
| `/api/portal-password` | POST | Basic Auth | Change portal password (JSON body: `{"new_password":"..."}`) — NVS mutation |
| `/change-password` | GET | Basic Auth | Voluntary password change page |
| `/api/logout` | GET | None | Always returns 401 to clear browser auth cache |

### Credential Protection (Doc Block 8)

- **Mechanism:** HTTP Basic Auth (`WWW-Authenticate: Basic realm="Argus Service Portal"`). This is interim access control, not encrypted transport (see DHR-002).
- **Default credentials:** `admin` / `admin`
- **Forced password change:** When the default password is active (NVS `pw_set != 1`), the portal handler serves a password-change page instead of the dashboard. All API endpoints remain accessible with default credentials during this window — the forced-change is a UI-level gate, not an API-level block. After successful password replacement, the bootstrap credential is disabled during normal operation (defense-in-depth check in `check_auth()`).
- **Bootstrap credential after replacement:** The compiled `admin` default remains in firmware as the first-use bootstrap credential. After a successful password change, `portal_get_credentials()` uses fail-closed logic: if `pw_set == 1` but the NVS password read fails, no valid password exists (empty string). NVS read errors other than not-found also fail closed. The bootstrap credential is not re-enabled by credential-storage errors.
- **Storage:** NVS namespace `argus_portal`, keys `pw` (plaintext password string — see DHR-003) and `pw_set` (bool flag)
- **Validation:** Min 4 chars (see DHR-005), max 64 chars, cannot reuse "admin"
- **Re-authentication:** After password change, the browser's cached Basic Auth credentials become invalid, triggering the native browser re-authentication prompt
- **Logout:** `GET /api/logout` always returns 401, clearing cached browser credentials
- **Stack hygiene:** All credential buffers zeroed after use

### Security Properties

| Property | Status |
|----------|--------|
| All endpoints require authentication | Yes |
| Default password forced to change (UI gate) | Yes |
| Bootstrap credential disabled after replacement | Yes |
| No secrets in API responses (passwords, AP credentials) | Yes |
| JSON escape on all device-supplied values (prevents JSON injection) | Yes |
| HTML escaping via JavaScript `h()` function (prevents DOM XSS) | Yes |
| `X-Content-Type-Options: nosniff` on all responses | Yes |
| `Cache-Control: no-store` on all responses | Yes |
| Credential buffers zeroed on stack | Yes |
| NVS password stored plaintext | Accepted for development (see DHR-003) |
| Basic Auth over unencrypted HTTP | Accepted for development (see DHR-002) |
| No authentication throttling | Accepted for development (see DHR-006) |
| No CSRF tokens | Accepted for development (see DHR-007) |

---

## lwip Socket-Level Filtering — Platform Finding

### Problem

The independent review required AP-interface enforcement to prevent STA-network clients from accessing the portal in APSTA mode.

### Approaches Attempted

Three socket-level approaches were attempted in this project configuration. The observed behavior may differ in other lwip configurations, ESP-IDF versions, or socket usage patterns.

1. **`open_fn` + `getsockname()`** — lwip returned `0.0.0.0` (INADDR_ANY) for the local address on accepted sockets when the listener was bound to INADDR_ANY.

2. **`open_fn` + `getpeername()`** — lwip returned `0.0.0.0` for the peer address on the socket fd passed to the `open_fn` callback.

3. **Per-handler `httpd_req_to_sockfd()` + `getpeername()`** — lwip returned `0.0.0.0` on the fully-established request socket.

### Root Cause

In this project configuration (ESP-IDF v5.5.3, ESP32-S3, lwip, `esp_http_server` with `INADDR_ANY` listener), lwip did not populate `sockaddr_in` peer/local addresses on accepted TCP sockets. Whether this applies universally to all ESP32/lwip configurations was not investigated.

### Resolution

Socket-level interface filtering was not successfully achieved in this project configuration. HTTP Basic Auth credential protection was added instead. Credential-based access control now applies regardless of whether the portal is reached through the AP or STA address.

### Operator Acceptance

The operator accepted this approach, noting that LAN-accessible portal access is useful as a secondary access path during development, and credential protection is the preferred long-term solution over interface filtering.

---

## Lifecycle and Lock Safety

### Lock Hierarchy

```
s_lifecycle_mutex (HTTP server start/stop)
  s_net_mutex (network state) — taken ONLY by handlers via get_snapshot()

Rule: HTTP start/stop occurs OUTSIDE s_net_mutex.
      Handlers take s_net_mutex for coherent snapshots.
      httpd_stop() waits for active handlers to finish.
      No circular dependency.
```

### Unified Stop Rule (`stop_server_locked()`)

All HTTP server stop paths go through one private helper:
- `s_server` cleared only after confirmed `httpd_stop()` success
- Handle preserved on failure (prevents orphaned server / duplicate start)
- Both public `stop()` and startup rollback use this function

### URI Registration Rollback

If any `httpd_register_uri_handler()` call fails during `start()`, the server is rolled back via `stop_server_locked()`. Error propagated through `s_lifecycle_mutex`-protected path.

---

## Pure Test Suite

### Count: 19 (18 Phase 4A + 1 Phase 4B.1)

| # | Test | Phase |
|---|------|-------|
| 1 | test_identity_mac_uid_derivation | 4A |
| 2 | test_identity_field_sanitization | 4A |
| 3 | test_nvs_schema_validation | 4A |
| 4 | test_nvs_open_sta_rejection | 4A |
| 5 | test_nvs_dual_slot_atomic_write_readback | 4A |
| 6 | test_nvs_lkg_rollback_on_crc_mismatch | 4A |
| 7 | test_password_masking_in_telemetry | 4A |
| 8 | test_mask_string_write_rejection | 4A |
| 9 | test_nvs_generation_wrap_comparison | 4A |
| 10 | test_authority_snapshot_coherence | 4A |
| 11 | test_browser_cli_owner_conflict_rejection | 4A |
| 12 | test_command_router_dispatch_gate | 4A |
| 13 | test_console_verbosity_policy_and_toggling | 4A |
| 14 | test_oneshot_status_non_mutation | 4A |
| 15 | test_nvs_commit_readback_verification_and_lkg_preservation | 4A |
| 16 | test_network_truthfulness_and_broker_ordering | 4A |
| 17 | test_console_input_validation | 4A |
| 18 | test_two_stage_service_entry_and_fail_closed_abort | 4A |
| 19 | test_http_json_escape_safety | 4B.1 |

### Tests Removed During Review (Integration, Not Pure)

- `test_http_lifecycle_observation` — tautological `bool` assertion, reads live HTTP singleton
- `test_http_secret_exclusion` — reads live NVS, tests `argus_nvs_config_mask()` which the handler does not use

### Truncation Test Fix (`8e20299`)

`json_escape()` reserves 2 bytes at the buffer end for worst-case escape pairs. With `dst_size=4`, the loop writes indices 0-1 and NUL at index 2. The original assertion `dst[3] == '\0'` checked a byte the function never writes. Fixed to `strlen(dst) < 4` and `dst[strlen(dst)] == '\0'`.

---

## Files Changed (cumulative from `main`)

| File | Lines Changed | Purpose |
|------|--------------|---------|
| `main/argus_http_server.c` | +700 (new file) | HTTP server, portal, auth, all endpoints |
| `main/argus_http_server.h` | +30 (new file) | Public API declarations |
| `main/argus_net_mgr.c` | ~30 | HTTP start/stop outside s_net_mutex, lock-graph comments |
| `main/argus_tests_4a.c` | ~20 | json_escape test, removed non-pure tests, updated counts |
| `main/CMakeLists.txt` | +2 | Added argus_http_server.c source, mbedtls dependency |
| `docs/` | Multiple | Phase 4A closeout, 4B plan, walkthroughs |

---

## Build Evidence

| Metric | Value |
|--------|-------|
| Build | ESP-IDF v5.5.3 |
| Errors | **0** |
| Warnings | **0** |
| Binary | 0xddbb0 (888 KB) |
| Partition free | 0x22450 (13%) |

---

## Runtime Acceptance Evidence (Operator-Supplied)

### Pure Test Suite
- 19 distinct tests x 3 passes = 57 executions
- 57 passed, 0 failed
- Production state: UNCHANGED across all dimensions

### Portal Access Flow (Operator-Verified, All Evidence Operator-Supplied)
1. Phone connected to Argus Service AP, DHCP assigned IP `192.168.4.2`
2. Browsed to `http://192.168.4.1`, browser prompted for credentials
3. Entered `admin` / `admin`, password-change page displayed
4. Changed password, page reloaded, re-authenticated with new credentials
5. Dashboard loaded with live machine status, authority, network, identity data
6. `/api/status` returned valid JSON — no configuration passwords or AP credentials observed
7. `/api/identity` returned valid JSON — no passwords observed
8. Portal intentionally accessible from controller's STA/LAN address (credential-protected)
9. Dashboard loaded successfully from both phone (AP) and LAN-connected laptop (STA)
10. Old browser authentication became invalid after password change
11. Re-authentication with changed password succeeded

---

## Operator Instructions

### Running Pure Tests
From the main diagnostic menu, press lowercase `t`.

### Enabling AP Discoverability
From the main diagnostic menu, press `N` then `4`.

### Portal Access
1. Connect phone/laptop to `Argus-Service-XXYYZZ` WiFi AP
2. Browse to `http://192.168.4.1`
3. Enter credentials when prompted (default: `admin` / `admin`)
4. Change password on first access (required before portal loads)

### Password Reset
The portal password is stored in NVS namespace `argus_portal`. To reset to defaults:
- **Full NVS erase:** `idf.py erase-flash` or `nvs_flash_erase()` at boot — resets ALL NVS namespaces including portal credentials and commissioning config.
- **Selective erase:** Not currently available. The portal credential namespace is separate from the commissioning namespace. `--erase-otadata` does NOT erase the portal credential namespace.
- **Unified credential/factory-reset:** Not yet implemented (see DHR-008).

---

## What Remains for Phase 4B.2+

| Item | Phase | Description |
|------|-------|-------------|
| Configuration read/write via HTTP | 4B.2 | Read and write commissioning config through portal |
| Service entry via HTTP POST | 4B.3 | `LOCAL_SERVICE`/`BROWSER` authority transfer |
| Local motion controls | 4B.3 | Start, stop, speed, E-stop via portal |
| Security hardening | 4D | HTTPS, hashed credentials, throttling, CSRF, audit |

See `DEFERRED_HARDENING_REGISTER.md` for the complete list of deferred security items.

---

## Closeout

**Phase 4B.1 status: COMPLETE WITHIN OPERATOR-ACCEPTED DEVELOPMENT RISK SCOPE**

This does not mean:
- Production security approved
- Internet exposure approved
- Phase 4D hardening complete
- Portal motion controls approved
- Configuration secrets approved for transmission over untrusted networks
