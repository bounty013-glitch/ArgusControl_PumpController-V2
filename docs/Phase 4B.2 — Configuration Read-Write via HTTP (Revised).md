# Phase 4B.2 — Configuration Read/Write via HTTP (Revised)

## Background

Phase 4B.1 delivered the credential-protected service portal. Phase 4B.2 adds configuration read/write through the portal.

**Operator decisions (this session):**
- Identity fields (client_id, unit_id, device_name) are **one-time configurable** — once set, they are locked from portal changes. Changes require backend access by Argus personnel or factory reset (deferred).
- WiFi config (sta_ssid, sta_pass) is a **user setting** — always editable via portal.
- Factory reset is **deferred** to a later phase. The operator wants to "feel the system" before deciding how identity modifications are ultimately handled.
- **No automatic restart** on config save. A manual Restart button is provided on the dashboard.
- Future plan: identity modification will become password-protected rather than permanently locked.

---

## Proposed Changes

### NVS Config Module

#### [MODIFY] [argus_nvs_config.c](file:///c:/Users/bount/Dev/Argus/ArgusControl_PumpController-V2/main/argus_nvs_config.c)

**Lines 544-547:** Remove `is_commissioned()` gate from `commit()`. Replace with diagnostic log. Identity-only configs (no WiFi) become saveable.

**Lines 478-481:** Adjust mask-string rejection — only reject if `sta_pass` is non-empty AND equals mask string. Empty `sta_pass` passes through cleanly.

---

### HTTP Server

#### [MODIFY] [argus_http_server.c](file:///c:/Users/bount/Dev/Argus/ArgusControl_PumpController-V2/main/argus_http_server.c)

**New endpoints:**

| Endpoint | Method | Auth | Purpose |
|----------|--------|------|---------|
| `GET /api/config` | GET | Basic Auth | Return current config as JSON (sta_pass masked) |
| `POST /api/config/save` | POST | Basic Auth | Read-modify-write: load current → overlay → validate → commit |
| `GET /config/identity` | GET | Basic Auth | Identity configuration page (one-time setup, read-only if already set) |
| `GET /config/wifi` | GET | Basic Auth | WiFi configuration page (always editable) |
| `POST /api/restart` | POST | Basic Auth | Coordinated reboot via net_mgr event post |

**Identity lock logic (server-side enforcement):**
- `POST /api/config/save`: Before overlaying identity fields from the request, check if they already exist in the current NVS config. If `client_id`, `unit_id`, and `device_name` are all non-empty, reject any attempt to modify them with a 403 and clear error message: `"Identity fields are locked after initial configuration"`
- This is enforced at the handler level, not in the NVS module — keeps the NVS module neutral for future backend/factory-reset changes.

**Identity config page behavior:**
- **First boot (fields empty):** Editable form with client_id, unit_id, device_name inputs. Save button active.
- **After configuration:** Fields displayed as read-only text (not editable inputs). Save button hidden. Message: "Identity is locked. Contact Argus support or perform factory reset to modify."
- Page queries `GET /api/config` on load to determine state.

**WiFi config page behavior:**
- Always editable. SSID pre-filled from current config. Password field empty (placeholder shows `••••••••` if configured).
- "Clear WiFi" option: sending empty `sta_ssid` and empty `sta_pass` clears WiFi config (device returns to UNCOMMISSIONED_AP on next boot).
- Save → success banner with "Restart required for changes to take effect."

**Restart button on dashboard:**
- Added to the dashboard alongside existing Change Password and Log Out buttons.
- Styled distinctly (warning color). Shows confirmation: "Are you sure? The controller will restart."
- Calls `POST /api/restart`, which posts `ARGUS_NET_EVT_SERVICE_EXIT` to net_mgr (in SERVICE_AP_ONLY mode) or calls `esp_restart()` via a FreeRTOS deferred call (in UNCOMMISSIONED_AP mode where there's no service to exit).

**Dashboard nav buttons:**
- Add "Identity" and "WiFi Config" navigation buttons to portal dashboard.

**max_uri_handlers:** 10 → 16.

**Mode gates:**
- Config read: all HTTP-active modes
- Config write: UNCOMMISSIONED_AP and SERVICE_AP_ONLY only
- Restart: all HTTP-active modes

---

### HTTP Server Header

#### [MODIFY] [argus_http_server.h](file:///c:/Users/bount/Dev/Argus/ArgusControl_PumpController-V2/main/argus_http_server.h)

Update scope documentation to reflect config and restart endpoints.

---

### Tests

#### [MODIFY] [argus_tests_4a.c](file:///c:/Users/bount/Dev/Argus/ArgusControl_PumpController-V2/main/argus_tests_4a.c)

Add: `test_nvs_commit_identity_only_payload` — verify that a payload with valid identity but empty WiFi passes `validate()` and `commit()` succeeds (the old `is_commissioned()` gate no longer blocks it).

Test count: 19 → 20.

---

### Documentation

#### [MODIFY] [PHASE_4B_IMPLEMENTATION_PLAN.md](file:///c:/Users/bount/Dev/Argus/ArgusControl_PumpController-V2/docs/PHASE_4B_IMPLEMENTATION_PLAN.md)

Update 4B.2 status, reflect operator audible.

---

## Summary of Scope

| Item | In Scope | Notes |
|------|----------|-------|
| `GET /api/config` | ✅ | Masked password in response |
| `POST /api/config/save` | ✅ | Read-modify-write, identity lock enforcement |
| `GET /config/identity` | ✅ | One-time configurable, read-only after set |
| `GET /config/wifi` | ✅ | Always editable user setting |
| `POST /api/restart` | ✅ | Manual reboot via dashboard |
| Dashboard nav buttons | ✅ | Identity, WiFi Config, Restart |
| NVS commit gate fix | ✅ | Remove `is_commissioned()` requirement |
| New test | ✅ | Identity-only commit validation |
| Factory reset | ❌ Deferred | Operator wants to feel the system first |
| Identity password protection | ❌ Deferred | Future: password-protected identity changes |

## Verification Plan

### Automated
1. `idf.py fullclean && idf.py build` — 0 errors
2. `idf.py size` — binary size
3. `git diff --check` — clean
4. Credential scan — no secrets in diff

### Operator Physical Tests
1. Flash and run `t` — 20 tests pass
2. Identity Config page on first boot — editable, save works
3. Identity Config page after save — fields locked, read-only
4. WiFi Config page — set credentials, save, see "restart required" banner
5. Restart button — controller reboots, new config active
6. Verify identity persisted after reboot
7. Verify WiFi connects to configured network after reboot
8. Clear WiFi and save — verify controller stays in UNCOMMISSIONED_AP
9. Verify identity-only config (no WiFi) saves and persists across reboot
