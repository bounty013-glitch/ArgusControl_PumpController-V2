# Phase 4B.2 Corrections — Implementation Plan

Proceeding through all 7 items. No approval wait needed per operator directive.

## Architectural Decisions

### 1. Atomic Identity Provisioning
**Decision:** Embed `provisioned_flags` (uint8_t) as the last field in `argus_config_payload_t`. Bump `ARGUS_CONFIG_SCHEMA_VERSION` to 2. Add schema migration in `is_slot_valid()`.

**Why this architecture:**
- The CRC covers the entire payload → lock state is covered by the same CRC as the identity data
- The dual-slot atomic write/verify/selector sequence makes identity + lock durable in one operation
- Power loss can never save identity without the flag or vice versa
- Read errors fail closed because `is_slot_valid()` rejects bad CRC → `has_valid_config = false` → unprovisioned
- No separate NVS key to go out of sync

**Schema migration:**
- `is_slot_valid()` currently rejects `schema_version != 1` and `payload_length != sizeof(payload)`. For v1→v2 migration: if schema_version == 1 and payload_length == old size (225 bytes), compute CRC over just the old payload bytes, and if valid, treat `provisioned_flags = 0` (unprovisioned, eligible for one portal save).
- Old-size: `sizeof(argus_config_payload_t_v1)` = 33 + 33 + 65 + 33 + 64 = 228 bytes (packed, no padding)
- New-size: 228 + 1 = 229 bytes

**Development migration rule:** Existing configs without `provisioned_flags` → `provisioned_flags = 0` → identity eligible for one portal provisioning. Documented as development migration, not production policy.

### 2. Coordinated Restart
**Decision:** Restart is a two-phase transaction on the net_mgr task:

1. HTTP handler: advisory preflight check, posts `RESTART_REQUEST` event, returns 200 immediately
2. net_mgr task receives event:
   a. Take dispatch lock, revalidate machine state (HOLDING/UNLOCKED, no E-stop/fault)
   b. If unsafe → log warning, release lock, do NOT reboot
   c. Set transitioning flag (prevents new motion start)
   d. Delay 500ms (HTTP response grace period — response was already sent)
   e. Stop HTTP server
   f. Revalidate machine state one more time
   g. If still safe → `esp_restart()`
   h. If unsafe → clear transitioning flag, fail closed

**Testable seam:** `argus_restart_ops_t` with injectable operations (check_safe, set_transitioning, delay, stop_http, revalidate, do_restart). Pure tests inject mocks and verify call order.

### 3. Pure Tests
Replace all 10 Phase 4B.2 tests with actually-proving tests:
- Config overlay seam: `argus_config_overlay_apply()` pure function
- Restart transaction seam: `argus_restart_transaction_execute()` with injected ops
- No production NVS access from any test

### 4. HTTP Body Reading
Bounded `recv_full_body()` helper with content_len check, recv loop, timeout/disconnect handling, NUL termination. Used by config_save_handler and password_handler.

### 5. Always-Available Service AP Policy
**Boot change:** Commissioned devices boot to `AP_DISCOVERABLE` (APSTA) with HTTP on, not `COMMISSIONED_STA`.
**Mode gate:** Config writes allowed in UNCOMMISSIONED_AP, SERVICE_AP_ONLY, AND AP_DISCOVERABLE.
**Authority:** AP visibility does not grant local motor authority. Portal is read/config only.

### 6. Lifecycle Tests
Pure tests using injectable ops for mode transitions. Verify AP visibility, authority gates, config staging access.

### 7. Verification
Full build, size, diff check, credential scan, static searches, docs, commit, push.

---

## File Changes

| File | Changes |
|------|---------|
| `argus_nvs_config.h` | Add `provisioned_flags` to payload, bump schema version, add overlay/restart seams |
| `argus_nvs_config.c` | Schema migration in `is_slot_valid()`, config overlay function |
| `argus_http_server.c` | HTTP body helper, overlay calls, remove separate id_provisioned, update mode gate |
| `argus_net_mgr.h` | Restart ops seam types |
| `argus_net_mgr.c` | Boot to AP_DISCOVERABLE, restart transaction, HTTP always on |
| `argus_tests_4a.c` | Replace 10 tests with pure proving tests + lifecycle tests |
| Docs | Deferred hardening register, walkthrough |
