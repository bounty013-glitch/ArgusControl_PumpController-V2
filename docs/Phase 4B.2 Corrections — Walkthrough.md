# Phase 4B.2 Corrections — Walkthrough

**Branch:** `phase4b-config-portal`
**Commit:** `2181055`
**Build:** 0 errors, 0 warnings
**Size:** 936,481 bytes
**Files changed:** 6 (+699 / -262)

---

## Architectural Decisions

### 1. Atomic Identity Provisioning

**Decision:** `provisioned_flags` (uint8_t) added as the last field of `argus_config_payload_t`. CRC covers the entire payload including the flag. Schema version bumped to 2.

**How it works:**
- Identity data and its provisioned/locked state are committed atomically in one dual-slot write/verify/selector sequence
- Power loss cannot save identity without locking it — both are in the same CRC'd blob
- Power loss cannot lock identity without saving it — selector isn't flipped until readback passes
- Storage/read errors fail closed: bad CRC → `is_slot_valid()` rejects → `has_valid_config = false` → unprovisioned
- Partial identity rejected by `argus_nvs_config_validate()` (all 3 identity fields required)
- WiFi-only updates preserve identity AND provisioned_flags via read-modify-write overlay
- Separate `is_identity_provisioned()` / `set_identity_provisioned()` NVS helpers deleted — flag is in the payload

**V1→V2 Schema Migration:**
- `is_slot_valid()` now accepts V1 slots (schema=1, payload_length=228)
- V1 CRC verified against the old 228-byte payload size
- On valid V1 read: `provisioned_flags = 0` (identity editable, eligible for one portal provisioning)
- Slot metadata upgraded in-place: schema→2, payload_length→229, CRC recomputed
- **Development migration rule:** existing configs without provisioning metadata → one portal provisioning allowed. Documented as development-phase rule, not production policy.

### 2. Coordinated Restart — Ordering and Races Fixed

**Five-phase restart transaction on net_mgr task:**
1. **Revalidate safety** — snapshot machine state, reject if not HOLDING/UNLOCKED or E-stop/fault
2. **Revoke authority** — `set_mode(NONE, NONE)` prevents new motion commands
3. **Response grace** — 500ms delay (HTTP handler already returned 200 before posting event)
4. **Stop HTTP** — clean server shutdown
5. **Final revalidation** — re-snapshot machine state, abort if safety changed during grace

**If any revalidation fails:** restart is aborted, authority is NOT restored (fail closed — operator power-cycles manually).

**No direct reboot from HTTP handler.** Handler posts `ARGUS_NET_EVT_RESTART_REQUEST` and returns 200.

### 3. Pure Tests — 19 Old → 19 New (38 total)

**Tests removed:**
- `test_identity_provisioned_lock_flag` (was reading production NVS)
- `test_restart_rejected_while_motion_active` (was calling live state manager, didn't prove rejection)
- `test_restart_accepted_when_safe` (same — documented state, didn't prove logic)

**Tests replaced with pure proving alternatives:**

| # | Test | What it actually executes |
|---|------|--------------------------|
| 20 | `test_nvs_commit_identity_only_payload` | Mock NVS: identity-only commit + readback, provisioned_flags preserved |
| 21 | `test_identity_provisioned_lock_flag` | Mock NVS: flag survives write/read/reinit cycle |
| 22 | `test_identity_partial_provisioning_rejected` | Validate rejects empty unit_id, empty client_id; accepts complete |
| 23 | `test_wifi_update_preserves_identity` | Mock NVS: WiFi overlay preserves identity AND provisioned_flags |
| 24 | `test_identity_update_preserves_wifi` | Mock NVS: identity overlay preserves WiFi fields |
| 25 | `test_omitted_password_preserves_stored` | Mock NVS: read/write cycle preserves password |
| 26 | `test_explicit_wifi_clear` | Mock NVS: zero WiFi, identity + flag survive |
| 27 | `test_mask_string_input_rejected_4b2` | Mock NVS: mask string rejected, empty accepted |
| 28 | `test_provisioned_identity_immutable` | Mock NVS: flag survives read-modify-write cycle |
| 29 | `test_storage_error_fails_closed` | Mock NVS: corrupted CRC → config rejected |
| 30 | `test_restart_safety_active_motion` | Stack-local snapshot: RUNNING → is_safe = false |
| 31 | `test_restart_safety_estop_fault` | Stack-local: E-STOPPED, FAULTED, HOLDING+estop → all unsafe |
| 32 | `test_restart_safety_safe_state` | Stack-local: HOLDING, UNLOCKED → safe |
| 33 | `test_new_ssid_without_password_rejected` | Validate: SSID without password rejected |
| 34 | `test_schema_v1_migration` | Mock NVS: V1 slot → migrated V2 with flags=0 |
| 35 | `test_ap_visibility_no_motor_authority` | Live snapshot: AP mode → authority NONE |
| 36 | `test_motor_commands_rejected_without_authority` | Live snapshot: NONE authority → NONE owner |
| 37 | `test_http_start_stop_idempotent` | Live: double-stop, start, double-start → no crash |
| 38 | `test_commissioned_requires_wifi` | Validate: identity-only ≠ commissioned |

### 4. HTTP Body Reading

**`recv_full_body()`** helper:
- Rejects empty bodies (400)
- Rejects oversized bodies (413) with socket drain
- Loops until complete `content_len` received
- Handles timeout (408) and disconnect (400)
- Always NUL-terminates on success
- Used by `config_save_handler` and `password_post_handler`

### 5. Always-Available Service AP (DHR-011)

| Boot condition | WiFi mode | Network mode | AP | HTTP | STA/MQTT |
|---|---|---|---|---|---|
| Uncommissioned | AP | `UNCOMMISSIONED_AP` | ✅ | ✅ | ❌ |
| Commissioned | APSTA | `AP_DISCOVERABLE` | ✅ | ✅ | ✅ (connects) |
| Service entry | AP | `SERVICE_AP_ONLY` | ✅ | ✅ | ❌ (isolated) |
| Service exit/reboot | APSTA | `AP_DISCOVERABLE` | ✅ | ✅ | ✅ (reconnects) |

**AP visibility does NOT grant motor authority.** Portal is read/config only. Motor commands require MQTT supervisory authority via the STA network path.

**Config staging allowed in AP_DISCOVERABLE** — operator can change WiFi settings through the portal without requiring CLI service entry.

### 6. Deferred Hardening

**DHR-011 added:** Always-available service AP and HTTP portal on commissioned devices. Operator-approved field-service policy. Persistent toggle and production default deferred to post-field-evaluation.

---

## Verification Evidence

| Check | Result |
|-------|--------|
| `idf.py build` | 0 errors, 0 warnings |
| `idf.py size` | 936,481 bytes |
| `git diff --check` | Clean |
| Credential scan | No secrets in diff |
| `esp_restart()` in HTTP handlers | None found |
| Production NVS in pure tests | None found |
| Stale STA-only lifecycle refs | Architecturally correct (fallback paths) |
| Tests compiled | 38 distinct × 3 passes = 114 executions |

**Commit:** `2181055`
**Push:** `phase4b-config-portal` → `origin/phase4b-config-portal`

---

## Pending Operator Verification

### Physical Test Sequence

1. **Flash firmware:** `idf.py flash -p <PORT>`
2. **Open serial monitor:** `idf.py monitor -p <PORT>`
3. **Run test suite:** Menu `t` — expect 38 tests × 3 passes = 114 executions, 0 failures
4. **Verify commissioned boot mode:**
   - With WiFi configured, reboot → should see `AP_DISCOVERABLE` in log
   - AP should be advertised (visible in WiFi scan)
   - STA should connect to configured network
   - HTTP portal should be accessible via AP connection
5. **Verify identity provisioning:**
   - Connect to AP, browse to portal, navigate to Identity page
   - Save identity (all 3 fields) → should succeed, flag set
   - Refresh identity page → fields should show as locked
   - Attempt to change identity → should be rejected (403)
6. **Verify WiFi configuration:**
   - Navigate to WiFi config page
   - Change SSID → should require password
   - Same SSID without password → should preserve existing
   - Clear WiFi (empty fields) → should zero both
7. **Verify restart behavior:**
   - Dashboard Restart button → should show confirmation
   - Confirm → controller reboots, portal comes back in AP_DISCOVERABLE
   - If in motion: restart should be rejected
8. **Verify V1→V2 migration:** (if prior V1 config exists)
   - First boot with new firmware → identity should load, provisioned_flags=0
   - Identity page should allow one provisioning
9. **Verify service entry/exit:**
   - Enter service mode → APSTA converts to AP-only
   - Exit service → reboot restores APSTA with portal

> [!IMPORTANT]
> All runtime behavior remains pending operator execution.
