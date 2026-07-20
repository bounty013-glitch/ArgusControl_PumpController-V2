# Phase 4B.3 — Browser Service Entry/Exit + OTA Partition Table

**Source-correction baseline:** `f0f3b0e` on `phase4b-config-portal`
**Board:** Waveshare ESP32-S3-RS485-CAN (16MB flash)
**Build:** ESP-IDF 5.5.1 incremental build — 992,149 bytes (68% free on 3MB partition)

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

## Verification

| # | Item | Result |
|---|------|--------|
| 1 | Incremental build | 1090 objects, zero errors, zero warnings (previous warning count was a false positive) |
| 2 | Binary size | 992,149 bytes (0xf2395) |
| 3 | Partition free | 68% (2,153,579 bytes on 3MB) |
| 4 | Test count | 83 distinct, 83 RUN_TEST |
| 5 | sdkconfig.defaults | `PARTITION_TABLE_CUSTOM=y`, `partitions.csv` |
| 6 | Partition table parsed | Confirmed by build output |
| 7 | OTA data partition | Generated `ota_data_initial.bin` |
| 8 | Flash command | Includes `ota_data_initial.bin` at 0xF000 |
| 9 | **Physical flash** | **Pending operator** |
| 10 | **Service entry/exit** | **Pending operator browser test** |

> [!IMPORTANT]
> **Flashing note:** The new partition table requires a full erase flash.
> Use `idf.py -p COMx erase-flash flash monitor` for the first flash
> after the partition change. Subsequent flashes can use normal `idf.py flash`.
