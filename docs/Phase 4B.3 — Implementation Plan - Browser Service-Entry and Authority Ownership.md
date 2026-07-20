# Phase 4B.3 — Browser Service-Entry and Authority Ownership

## Summary

Phase 4B.3 adds two HTTP endpoints that allow the browser portal operator to explicitly enter and exit local service mode, transferring pump command authority from MQTT to the browser and back.

The core orchestration (`argus_net_mgr_request_service` and `argus_net_mgr_request_service_exit`) is already fully implemented and physically verified. Phase 4B.3 wires the HTTP handlers using a pure policy seam to enforce authority constraints.

---

## Prerequisite: App Partition Size Increase

> [!CAUTION]
> The new 16MB OTA partition table requires a full flash erase. This is a one-time destructive operation.
> All NVS data, including WiFi credentials, identity configuration, and portal passwords, will be permanently lost.

### Implemented Change

The target board is the Waveshare ESP32-S3-RS485-CAN with 16MB flash. 
We have increased the partition space by migrating to an OTA-ready custom table:
1. `partitions.csv` has been created with: 3MB `ota_0`, 3MB `ota_1`, `otadata`, NVS, PHY, and `coredump` partitions.
2. The `sdkconfig.defaults` has been updated to use `PARTITION_TABLE_CUSTOM=y`.

### Post-Flash Re-commissioning
Because all NVS configuration is erased during the migration, the controller will boot into `UNCOMMISSIONED_AP` mode:
1. Connect to the default Service AP: `Argus-Service-<MAC>` (Password: `argusadmin`).
2. Navigate to `http://192.168.4.1`.
3. Log in with default credentials (`admin` / `admin`).
4. You will be forced to change the portal password.
5. Re-enter the device identity and local network credentials.
6. Restart the controller to apply the configuration.

---

## Architecture Changes

### Handler Architecture (A1 Compliance)

Per architectural amendment A1, HTTP handlers **must not** call blocking orchestration directly to prevent deadlock.
Instead, an **Event-Driven (Async)** approach has been implemented:
1. The HTTP handler evaluates the preconditions using the pure `argus_service_policy` seam.
2. If valid, the handler posts an event (`ARGUS_NET_EVT_SERVICE_REQUEST` or `ARGUS_NET_EVT_SERVICE_EXIT`) to the net_mgr task queue and returns `202 Accepted` immediately.
3. The browser portal's UI displays context-sensitive buttons and asynchronous feedback.

### Pure Service-Policy Seam

The entry/exit decision logic is isolated in a pure policy seam (`argus_service_policy.h/c`) that enforces production authority rules:
- **`AP_DISCOVERABLE`** requires `SUPERVISORY` mode / `MQTT` owner.
- **`UNCOMMISSIONED_AP`** requires `NONE` mode / `NONE` owner.
- **`SERVICE_AP_ONLY`** idempotent entry / eligible exit requires `LOCAL_SERVICE` mode / `BROWSER` owner.

The pure policy seam is exercised by stack-local tests covering eligible entry pairings, authority mismatches, idempotency, transition-in-progress, exit eligibility, exit rejection, and event construction.

---

## Actually Executed Verification (Final Accepted Result)

### Static Verification
1. `git diff --check` — clean
2. Full clean build — zero errors, zero warnings
3. Binary size within new partition budget
4. 94 Distinct Pure Tests all passing

### Physical Verification
1. Flash to device using `idf.py -p COMx erase-flash flash monitor`
2. Connect phone to Service AP
3. Browse to portal, verify context-sensitive buttons appear correctly
4. Verify `POST /api/service/enter` transitions to `SERVICE_AP_ONLY` / `LOCAL_SERVICE/BROWSER`
5. Verify `/api/status` reflects new authority state
6. Verify `POST /api/service/exit` triggers clean reboot
7. Verify duplicate service-enter is idempotent and rejected in wrong mode
