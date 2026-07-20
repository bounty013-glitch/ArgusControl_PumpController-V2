# Phase 4B.3 — Browser Service Entry/Exit + OTA Partition Table

**Source-correction baseline:** `f0f3b0e` on `phase4b-config-portal`
**Board:** Waveshare ESP32-S3-RS485-CAN (16MB flash)
**Build:** ESP-IDF 5.5.3 incremental build — 992,149 bytes (68% free on 3MB partition)

---

## What Changed

### 1. OTA-Ready Partition Table

Migrated from the default 1MB single-app partition (6% free, dangerously tight) to a custom OTA-ready layout:

| Partition | Type | Offset | Size | Purpose |
|-----------|------|--------|------|---------|
| `nvs` | data, nvs | 0x9000 | 24KB | Config, portal credentials, reset marker |
| `otadata` | data, ota | 0xF000 | 8KB | OTA slot selector (A/B) |
| `phy_init` | data, phy | 0x11000 | 4KB | WiFi RF calibration |
| `ota_0` | app, ota_0 | 0x20000 | **3MB** | Primary app — current binary |
| `ota_1` | app, ota_1 | 0x320000 | **3MB** | OTA target (future Atlantis delivery) |
| `coredump` | data, coredump | 0x620000 | 256KB | Crash diagnostics |
| *(free)* | — | — | **~9.6MB** | Future use |

**Files:**
- [NEW] [partitions.csv](file:///c:/Users/bount/Dev/Argus/ArgusControl_PumpController-V2/partitions.csv)
- [MODIFY] [sdkconfig.defaults](file:///c:/Users/bount/Dev/Argus/ArgusControl_PumpController-V2/sdkconfig.defaults) — `PARTITION_TABLE_CUSTOM=y`

---

### 2. Service Entry/Exit HTTP Handlers

Two new POST endpoints wire the browser portal to the existing service orchestration. The pure policy seam is exercised by stack-local tests covering eligible entry pairings, authority mismatches, idempotency, transition-in-progress, exit eligibility, exit rejection, and event construction.

#### `POST /api/service/enter`

- **Mode gate:** `AP_DISCOVERABLE` or `UNCOMMISSIONED_AP`
- **Idempotent:** Returns `200 OK` if already `SERVICE_AP_ONLY` / `BROWSER`
- **A1 compliance:** Posts `ARGUS_NET_EVT_SERVICE_REQUEST` to the net_mgr event queue. Cannot call `request_service()` directly because it calls `httpd_stop()`, which would deadlock from within an httpd handler.
- **Response:** `202 Accepted` — browser reconnects to AP after transition completes

#### `POST /api/service/exit`

- **Mode gate:** `SERVICE_AP_ONLY`
- **Authority gate:** `LOCAL_SERVICE` / `BROWSER`
- **A1 compliance:** Posts `ARGUS_NET_EVT_SERVICE_EXIT` to the net_mgr event queue. Cannot call `request_service_exit()` directly because it calls `esp_restart()`, which would execute before the HTTP response is sent.
- **Response:** `202 Accepted` — device reboots shortly after

Both handlers use the existing event queue (`argus_net_mgr_post_event`) and existing production orchestration. No new locks, mutexes, or task contexts introduced.

**File:** [MODIFY] [argus_http_server.c](file:///c:/Users/bount/Dev/Argus/ArgusControl_PumpController-V2/main/argus_http_server.c) — handlers, URI definitions, registrations

---

### A1 Deadlock Avoidance — Design Decision

The implementation plan initially proposed direct synchronous calls (Option A). During implementation, I identified that:

1. **`request_service()`** calls `argus_http_server_stop()` internally. `httpd_stop()` waits for active handlers to complete. Calling it from within a handler → deadlock.
2. **`request_service_exit()`** calls `esp_restart()` directly. The reboot happens before the handler returns → HTTP response never reaches the browser.

Both handlers use an event-driven architecture to avoid deadlocks:

1. Capture snapshots and evaluate the pure policy.
2. Construct the browser-owned network event.
3. Attempt to enqueue the event.
4. Return `503 Service Unavailable` if enqueueing fails.
5. Return `202 Accepted` after successful enqueue.
6. The network-manager task executes the transition from its own context.
7. Physical completion remains unverified until the operator reconnects and checks controller state.

---

### 3. Fresh-NVS Observation Correction

The production NVS driver returns `ESP_ERR_NVS_NOT_FOUND` when the configuration namespace or slots do not yet exist, which is a legitimate state for a freshly erased uncommissioned device. Previously, `argus_nvs_config_get_observation_snapshot()` misinterpreted this as a snapshot failure.

**Corrections:**
1. **Pure Helper:** Extracted observation logic into a pure helper `argus_nvs_core_get_observation_snapshot(const argus_nvs_driver_t *drv, argus_nvs_observation_t *out_obs)`.
2. **Missing-Value Normalization:** Both `ESP_ERR_NOT_FOUND` and `ESP_ERR_NVS_NOT_FOUND` are now successfully normalized as valid absence states (setting `present = false`, `valid = false`).
3. **Regression Tests:** Added 6 new pure stack-local test cases validating generic missing, NVS missing, mixed missing, unexpected selector/slot errors, and successful observations.
4. **Failure Diagnostics:** Improved test runner output to print exact error names and broken-down statuses for initial and final snapshots if they fail.

---

## Registered Endpoints (13/16 slots used)

| Phase | Endpoint | Method | Handler |
|-------|----------|--------|---------|
| 4B.1 | `/` | GET | Portal HTML |
| 4B.1 | `/api/status` | GET | System snapshot |
| 4B.1 | `/api/identity` | GET | Device identity |
| 4B.1 | `/password` | GET | Password change page |
| 4B.1 | `/api/portal-password` | POST | Change portal password |
| 4B.1 | `/logout` | GET | Logout trigger |
| 4B.2 | `/api/config` | GET | Masked config read |
| 4B.2 | `/api/config/save` | POST | Config write + restart |
| 4B.2 | `/identity` | GET | Identity config page |
| 4B.2 | `/wifi` | GET | WiFi config page |
| 4B.2 | `/api/restart` | POST | Coordinated restart |
| **4B.3** | **`/api/service/enter`** | **POST** | **Service entry** |
| **4B.3** | **`/api/service/exit`** | **POST** | **Service exit** |

---

## Phase 4B.3 — Uncommissioned Browser Service-Entry Correction

### Physical Failure Evidence
During physical testing, the browser requested service entry from the `UNCOMMISSIONED_AP` baseline but failed during the transition:
```text
argus_auth_mgr: authority: NONE/NONE -> SERVICE_TRANSITION/NONE
argus_net_mgr: network: UNCOMMISSIONED_AP -> SERVICE_TRANSITION
argus_http: HTTP server stopped
argus_net_mgr: Service entry failed during transition: ESP_FAIL
argus_auth_mgr: Aborting service transition -> setting authority NONE/NONE
```
The HTTP server did not spontaneously crash; it was intentionally stopped, but upon transition failure, the abort path failed to restore HTTP service, leaving the browser stranded.

### Root Cause
1. **Non-idempotent operations**: The uncommissioned state already satisfies several target conditions (machine unlocked, broker stopped, STA disconnected, AP-only). The state-convergence operations blindly requested actions and treated "already in target state" as hard failures.
2. **Missing HTTP restoration**: The `abort_transition` block did not restore the HTTP server.
3. **Generic error reporting**: The abort block logged the generic error but did not identify the exact step that failed.

### Correction Architecture
1. **Idempotent Convergence**:
   - `prod_request_normal_stop`, `prod_stop_broker`, `prod_disconnect_sta`, and `prod_set_wifi_ap_only` were updated to perform state checks prior to acting and return `ESP_OK` if the target state is already met.
2. **HTTP Restoration**:
   - The `abort_transition` sequence conditionally restores the HTTP service if it was successfully stopped during the transition attempt.
3. **Stage-Specific Error Identification**:
   - A `fail_stage` tracking string was introduced to uniquely identify which of the 10 blocking steps failed.
4. **Pure Orchestration Refactoring**:
   - The `argus_service_transition_ops_t` and `argus_net_mgr_orchestrate_service_entry` seams were expanded to cover the complete transition sequence, including network locks, dispatch revalidation, and HTTP lifecycle.
   - `argus_net_mgr_request_service` was refactored to wrap and delegate to this 100% pure orchestrator, allowing complete stack-local verification without singleton pollution.

---

### Second Physical Failure (STA Disconnect)
During retesting, a second failure occurred during the `ensure STA disconnected` stage. `esp_wifi_disconnect()` was called unconditionally and returned `ESP_FAIL` because the STA interface was already entirely absent (UNCOMMISSIONED_AP state). The transition failed closed safely, and HTTP was correctly restored (Authority: NONE/NONE, Network: NETWORK_FAULT).

### Second Correction Architecture (Evidence-Driven Convergence)
1. **Pure Helper Evaluation**:
   - Created `argus_net_mgr_eval_sta_disconnect_req` as a pure, testable helper to evaluate if a disconnect is necessary.
   - It cross-references `wifi_mode`, `sta_started`, `sta_connected`, and `sta_ip_acquired`.
   - Redundant disconnect driver calls are safely bypassed (`ESP_OK`) if STA is already disconnected.
2. **Contradictory State Protection**:
   - The pure helper rejects contradictory states (e.g. `WIFI_MODE_AP` but `sta_connected == true`), never silently normalizing flags to manufacture success.
3. **Pure Tests Expanded**:
   - Added test 82 (`test_sta_disconnect_eval`) covering 6 pure permutations (AP-only absent, APSTA connected, APSTA disconnected, and contradictory states).

## Verification

| # | Item | Result |
|---|------|--------|
| 1 | Full build | PASSED |
| 2 | Binary size | Verified |
| 3 | Partition free | 69% (2,176,992 bytes on 3MB) |
| 4 | Test count | 91 distinct cases (273 total executions) |
| 5 | sdkconfig.defaults | `PARTITION_TABLE_CUSTOM=y`, `partitions.csv` |
| 6 | Partition table parsed | Confirmed by build output |
| 7 | OTA data partition | Generated `ota_data_initial.bin` |
| 8 | Flash command | Includes `ota_data_initial.bin` at 0xF000 |
| 9 | **Physical test execution** | **PASSED (All tests)** |
| 10 | **Production isolation** | **Confirmed by operator** (Authority generation=2, Network=UNCOMMISSIONED_AP, Broker=STOPPED, Machine=UNLOCKED, Task count=14) |
| 11 | **Service entry/exit** | **PASSED** |

> [!IMPORTANT]
> **Flashing note:** The new partition table requires a full erase flash.
> Use `idf.py -p COMx erase-flash flash monitor` for the first flash
> after the partition change. Subsequent flashes can use normal `idf.py flash`.
