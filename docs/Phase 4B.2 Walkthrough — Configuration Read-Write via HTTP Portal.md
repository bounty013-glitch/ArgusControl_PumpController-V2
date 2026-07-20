# Phase 4B.2 Walkthrough — Configuration Read/Write via HTTP Portal

## Summary

Phase 4B.2 adds configuration provisioning to the Argus service portal. Identity is provisioned once and locked. WiFi is always user-editable. Restart is coordinated through the net_mgr event queue with motion safety checks.

**Branch:** `phase4b-config-portal`
**Commit:** `de66df2`
**Binary size:** 932,601 bytes

---

## Changes Made

### 1. NVS Commit Gate Removed
**File:** [argus_nvs_config.c](../main/argus_nvs_config.c)

- `is_commissioned()` no longer blocks NVS commit — identity-only configs (empty WiFi) are now saveable
- Mask-string rejection adjusted: empty `sta_pass` is no longer incorrectly rejected
- Commissioning status is logged for diagnostics but does not gate the commit

### 2. Coordinated Restart API
**Files:** [argus_net_mgr.h](../main/argus_net_mgr.h) · [argus_net_mgr.c](../main/argus_net_mgr.c)

- New event type: `ARGUS_NET_EVT_RESTART_REQUEST`
- New API: `argus_net_mgr_request_restart()` — checks machine state via `argus_state_mgr_get_snapshot()`, rejects if motion active (not HOLDING/UNLOCKED) or E-stop latched
- Event handler in `net_mgr_task()`: stops HTTP server, delays 200ms for in-flight responses, calls `esp_restart()`
- HTTP handler can transmit response before shutdown occurs

### 3. HTTP Endpoints (5 new)
**File:** [argus_http_server.c](../main/argus_http_server.c)

| Endpoint | Method | Purpose |
|----------|--------|---------|
| `/api/config` | GET | Current configuration (passwords masked, lock state) |
| `/api/config/save` | POST | Scoped config update (identity or WiFi) |
| `/api/restart` | POST | Coordinated restart via net_mgr |
| `/config/identity` | GET | Identity provisioning page |
| `/config/wifi` | GET | WiFi configuration page |

### 4. Identity Provisioning Lock
- NVS key `id_provisioned` (uint8_t) in `argus_portal` namespace
- Set to 1 atomically after successful identity config commit
- Portal refuses identity modification when locked (403 Forbidden)
- Backend/factory-reset clears the lock (deferred to Phase 4D)

### 5. Config Save Semantics
- **Scope-based overlay:** `"scope":"identity"` or `"scope":"wifi"` — each scope only modifies its own fields
- **Identity requires all 3 fields:** client_id, unit_id, device_name — partial rejected
- **WiFi password preservation:** omitted password on same-SSID save preserves stored password
- **WiFi clear:** empty SSID + empty pass zeros both fields
- **Mask string rejected:** `"********"` as input always returns 400
- **Mode gate:** config writes only allowed in UNCOMMISSIONED_AP or SERVICE_AP_ONLY

### 6. Dashboard Navigation
- Added Identity, WiFi Config, and Restart buttons to portal dashboard
- Restart shows confirmation dialog, then full-screen "Restarting..." message

### 7. Header Documentation
**File:** [argus_http_server.h](../main/argus_http_server.h)

Updated scope docs to reflect Phase 4B.2 endpoints, write gates, and security posture.

---

## Tests

### New Tests (10 added, 29 total)

| # | Test | What it verifies |
|---|------|------------------|
| 20 | `test_nvs_commit_identity_only_payload` | Identity-only config passes validate + commit |
| 21 | `test_identity_provisioned_lock_flag` | NVS portal namespace read/write is functional |
| 22 | `test_identity_partial_provisioning_rejected` | Empty client_id or unit_id rejected by validate |
| 23 | `test_wifi_update_preserves_identity` | WiFi-scope overlay doesn't alter identity fields |
| 24 | `test_identity_update_preserves_wifi` | Identity-scope overlay doesn't alter WiFi fields |
| 25 | `test_omitted_password_preserves_stored` | Same-SSID save preserves existing password |
| 26 | `test_explicit_wifi_clear` | Empty SSID + pass zeros both, identity survives |
| 27 | `test_mask_string_input_rejected_4b2` | Mask string rejected; empty password accepted |
| 28 | `test_restart_rejected_while_motion_active` | State check contract verification |
| 29 | `test_restart_accepted_when_safe` | Safe-state classification consistency |

---

## Verification Evidence

| Check | Result |
|-------|--------|
| `idf.py build` | 0 errors, 0 warnings |
| `idf.py size` | 932,601 bytes |
| `git diff --check` | Clean |
| Credential scan | No secrets in diff |
| 29 tests | Compiled (execution pending on-device) |
| Runtime behavior | Pending operator verification |

> [!IMPORTANT]
> On-device test execution and portal functional testing (identity save/lock, WiFi save/clear/preserve, restart behavior) are pending operator verification.
