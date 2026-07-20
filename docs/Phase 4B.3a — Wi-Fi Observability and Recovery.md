# Phase 4B.3a — Wi-Fi Observability and Recovery

The Wi-Fi failure classification and recovery sequence has been successfully implemented on the `phase4b3a-wifi-observability` branch. The changes from the legacy WIP branch were audited and safely integrated into the current `main` baseline without breaking the isolation and safety guarantees established in Phase 4B.3.

## What was implemented

### 1. Network Manager Lifecycle and Failure Classification
*   Introduced `argus_sta_state_t` to track detailed connection stages (`IDLE`, `CONNECTING`, `ASSOCIATED_WAITING_IP`, `CONNECTED`, `RETRY_WAIT`, `ACTION_REQUIRED`).
*   Introduced `argus_disconnect_category_t` to categorize cryptic ESP32 Wi-Fi reasons (e.g., classifying `WIFI_REASON_AUTH_EXPIRE` as `AUTHENTICATION`).
*   Implemented a bounded auto-retry policy that gives up after 3 consecutive authentication failures, preventing infinite loops and transitioning the system to an `ACTION_REQUIRED` state.
*   Added IP acquisition timeouts (10 seconds) to detect networks that associate but fail to provide DHCP leases.

### 2. HTTP Portal Observability
*   The `GET /api/status` endpoint now exposes the full connection state:
    *   STA lifecycle state
    *   Last failure category and raw reason code
    *   Retry countdown timer
    *   Clear operator guidance (e.g. "Check Wi-Fi SSID/password")
*   The dashboard's Network panel automatically displays these fields.

### 3. Manual Recovery Action
*   Added the `POST /api/network/reconnect` endpoint.
*   Added a "Reconnect Wi-Fi" button to the dashboard that becomes visible when the STA reaches the `ACTION_REQUIRED` state. This safely queues an `ARGUS_NET_EVT_MANUAL_RECONNECT_REQUEST` event to the `net_mgr` task without requiring a full device restart.

### 4. Pure Tests
*   Added 3 new pure tests to the test suite to prove classification, retry evaluation, and manual reconnect authorization logic.
*   Total registered pure test count updated to 97.

## Verification Results

The firmware was successfully compiled from scratch using `ESP-IDF v5.5.3`.

*   **Build Outcome:** SUCCESS
*   **Firmware Size:** `0xef350` bytes
*   **Smallest App Partition:** `0x300000` bytes
*   **Free Space Remaining:** `0x210cb0` bytes (69% free)

## Next Steps

1.  Please review the generated [Phase 4B.3a Physical Tests.md](./Phase%204B.3a%20Physical%20Tests.md) checklist.
2.  Flash the controller and execute the test sequence to physically verify the new dashboard UI and failure recovery transitions.
3.  Report back with the serial logs of the boot sequence indicating that the 97 tests passed successfully.
