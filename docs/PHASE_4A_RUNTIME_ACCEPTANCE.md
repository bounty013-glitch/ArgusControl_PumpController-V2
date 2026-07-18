# Phase 4A Runtime Acceptance Record

## Status Overview

- **Scenario A (Uncommissioned AP Mode)**: `COMPLETE — PHYSICALLY VERIFIED`
- **Scenario B (STA Commissioning & NVS Persistence)**: `COMPLETE — PHYSICALLY VERIFIED`
- **Scenario C (Service AP Discoverability & Dual-Interface Coexistence)**: `COMPLETE — PHYSICALLY VERIFIED`
- **Scenario D (Coordinated Service AP Transition & MQTT Shutdown)**: `NOT STARTED — AWAITING SHAWN`

---

## Scenario C Verification Evidence

On-device physical verification performed on ESP32-S3 target hardware running `v2-phase4a-dev`.

### 1. Service AP Association Log
```text
wifi:station: ae:49:7b:08:5b:23 join, AID=1, bgn, 20
esp_netif_lwip: DHCP server assigned IP to a client, IP is: 192.168.4.2
```

### 2. Network and Authority Snapshot
```text
Network Mode        : AP_DISCOVERABLE (3)
Wi-Fi Driver Mode   : APSTA
STA Status          : CONNECTED (IP Acquired)
Service AP Status   : ENABLED
MQTT Broker Status  : READY
Authority Mode      : SUPERVISORY (1)
Authority Owner     : MQTT (1)
Authority Generation: 2
```

### 3. Non-Mutating Permission Probes
- **Browser-Source Permission Probe**:
  ```text
  Probing Browser-source authority (gen 2)...
  [REJECTED: ESP_ERR_INVALID_STATE (259)]
  State Invariance Check: PASSED (15/15 fields unchanged)
  ```
- **MQTT-Source Permission Probe**:
  ```text
  Probing MQTT-source authority (gen 2)...
  [ACCEPTED]
  State Invariance Check: PASSED (15/15 fields unchanged)
  ```

---

## Scenario C Acceptance Conclusions

All of the following acceptance requirements have been physically verified on hardware:
1. Service AP was discoverable and visible.
2. WPA2 authentication succeeded.
3. Client phone associated successfully.
4. Service AP DHCP assigned `192.168.4.2`.
5. Wi-Fi driver operated in `APSTA` dual mode.
6. Commissioned STA interface remained connected with an acquired IP.
7. Embedded MQTT broker remained ready and operational.
8. Authority mode remained exclusively `SUPERVISORY/MQTT`.
9. Authority generation remained unchanged at generation 2.
10. AP association did not transfer command authority.
11. Browser-source motion command authority was rejected.
12. MQTT-source command authority remained accepted.
13. Both permission probes were non-mutating.
14. The "one pump, one command authority" doctrine remained 100% intact during AP discoverability.

**Scenario C Status**: `COMPLETE — PHYSICALLY VERIFIED`

---

## Pre-Scenario D Test Isolation & Service Orchestration Hardening

- **Previous Physical Execution Result**:
  - 17/18 distinct Phase 4A cases passed
  - 51/54 total executions passed
  - 1 distinct case (`test_nvs_dual_slot_atomic_write_readback`) failed in all 3 repeat passes due to unassigned active config payload copy in test seam.
- **Hardening & Isolation Applied**:
  - Encapsulated command-dispatch serialization barrier (`argus_cmd_router_lock_dispatch()` / `unlock_dispatch()`).
  - Private authority core encapsulation (`argus_authority_core_t`) and `argus_service_authority_ops_t` seam.
  - Complete 14-step synchronous orchestrator (`argus_net_mgr_orchestrate_service_entry()`) with strict action -> verification ordering, bounded polling timeouts, and fail-closed error preservation used identically in production and unit tests.
  - Full-fidelity 23-field read-only production snapshot comparison (`compare_prod_snapshots()`) with diff logging and explicit `PHASE 4A PURE UNIT TEST SUITE: PASSED/FAILED` result.
  - 100% stack-local caller-owned mock NVS contexts (`mock_nvs_store_t`) and verified payload readback assignment in `argus_nvs_core_commit()`.
- **Final Uncontested Build Evidence**:
  - Binary Size: `0xcfaa0` (850,592 bytes)
  - Status: `HARDENING IMPLEMENTED AND COMPILED — PENDING SHAWN'S RUNTIME TEST`
- **Scenario D Status**: `NOT STARTED — PENDING SHAWN'S RUNTIME TEST`
