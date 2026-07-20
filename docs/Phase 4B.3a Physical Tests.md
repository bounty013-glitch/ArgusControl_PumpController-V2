# Phase 4B.3a — Physical Tests: Wi-Fi Observability and Recovery

This document outlines the required operator verification sequence for the Phase 4B.3a network manager extensions. It confirms that Wi-Fi failure classification, automatic retries, and the HTTP dashboard API behave correctly in the physical environment.

---

### Prerequisites
1. Argus V2 Controller flashed with the Phase 4B.3a firmware.
2. Operator device connected to the configuration AP (`ARGUS-...`).
3. Dashboard open in a browser (`http://192.168.4.1`).

---

### Test 1: Correct-Credential Baseline Connection
*   **Objective:** Verify that providing valid credentials successfully transitions through the STA states and fully connects.
*   **Preconditions:** Controller is uncommissioned or has invalid credentials.
*   **Operator Actions:**
    1. Enter a valid Wi-Fi SSID and Password in the portal.
    2. Click "Save Configuration".
*   **Expected Dashboard Observations:**
    *   STA State transitions from `CONNECTING` -> `ASSOCIATED_WAITING_IP` -> `CONNECTED`.
    *   IP address is displayed.
    *   Broker transitions to `Running`.
*   **Actual Result:** `[PENDING]`

### Test 2: Deliberately Incorrect Password
*   **Objective:** Verify that an incorrect password is appropriately classified as an authentication failure.
*   **Preconditions:** Controller is connected and commissioned.
*   **Operator Actions:**
    1. Enter the valid SSID but a deliberately incorrect password.
    2. Click "Save Configuration".
*   **Expected Dashboard Observations:**
    *   STA State becomes `RETRY_WAIT`.
    *   Last Failure category reports `AUTHENTICATION`.
    *   Retry countdown is displayed.
    *   Broker remains `Stopped`.
*   **Actual Result:** `[PENDING]`

### Test 3: Three-Attempt Authentication Limit and Operator-Action-Required State
*   **Objective:** Verify that the system stops retrying after three consecutive authentication failures.
*   **Preconditions:** Test 2 is in progress.
*   **Operator Actions:**
    1. Wait for three retry cycles to complete.
*   **Expected Dashboard Observations:**
    *   STA State becomes `ACTION_REQUIRED`.
    *   Failures counter reaches `3`.
    *   Operator Guidance explicitly states that manual intervention is needed.
    *   "Reconnect Wi-Fi" manual button becomes visible.
*   **Actual Result:** `[PENDING]`

### Test 4: Manual Reconnect While Credentials Remain Incorrect
*   **Objective:** Verify that a manual reconnect attempt processes correctly but still fails if credentials are unchanged.
*   **Preconditions:** Controller is in the `ACTION_REQUIRED` state from Test 3.
*   **Operator Actions:**
    1. Click the "Reconnect Wi-Fi" button on the dashboard.
*   **Expected Dashboard Observations:**
    *   Alert confirms the request was accepted.
    *   STA State briefly transitions to `CONNECTING`.
    *   Connection fails, returning to `ACTION_REQUIRED` (or `RETRY_WAIT` depending on failure accumulation).
*   **Actual Result:** `[PENDING]`

### Test 5: Correction of Credentials and Successful Recovery
*   **Objective:** Verify that providing the correct password recovers the connection and clears all error states.
*   **Preconditions:** Controller is in the `ACTION_REQUIRED` or `RETRY_WAIT` state.
*   **Operator Actions:**
    1. Enter the correct Wi-Fi Password.
    2. Click "Save Configuration".
*   **Expected Dashboard Observations:**
    *   STA State returns to `CONNECTED`.
    *   Failures counter is reset.
    *   Last Failure category clears.
    *   Operator Guidance returns to "Normal operation".
*   **Actual Result:** `[PENDING]`

### Test 6: Access Point Unavailable Followed by Automatic Recovery
*   **Objective:** Verify transient failure handling.
*   **Preconditions:** Controller is successfully connected (Test 5 complete).
*   **Operator Actions:**
    1. Power off or disable the configured Wi-Fi Access Point.
    2. Wait for the controller to disconnect.
    3. Power the Access Point back on.
*   **Expected Dashboard Observations:**
    *   Upon disconnect, STA State becomes `RETRY_WAIT`.
    *   Last Failure category reports `AP_UNAVAILABLE`.
    *   Retries continue indefinitely (no transition to `ACTION_REQUIRED`).
    *   Once AP is restored, STA State becomes `CONNECTED` automatically.
*   **Actual Result:** `[PENDING]`

### Test 7: Preservation of AP and HTTP Access Throughout Failure
*   **Objective:** Verify that the controller remains accessible despite STA failure.
*   **Preconditions:** Controller is in a disconnected/retry loop.
*   **Operator Actions:**
    1. Verify connection to the `ARGUS-...` service AP.
    2. Refresh the browser at `http://192.168.4.1`.
*   **Expected Dashboard Observations:**
    *   Dashboard loads successfully.
    *   Network status is accurately reflected.
*   **Actual Result:** `[PENDING]`

### Test 8: Retry Suppression During Browser Local Service
*   **Objective:** Ensure Wi-Fi auto-reconnect logic does not interfere with `SERVICE_AP_ONLY` mode.
*   **Preconditions:** Controller is in `ACTION_REQUIRED` or `RETRY_WAIT`.
*   **Operator Actions:**
    1. Click "Enter Local Service" on the dashboard.
*   **Expected Dashboard Observations:**
    *   STA State becomes `DISABLED` or `IDLE`.
    *   Auto-retry timers do not fire in the background (no serial logs indicating connection attempts).
    *   Manual "Reconnect Wi-Fi" button is hidden or clicking it is rejected with a Conflict error.
*   **Actual Result:** `[PENDING]`

### Test 9: Normal Commissioned Recovery After Service Exit
*   **Objective:** Ensure the system restores its Wi-Fi retry logic after exiting local service.
*   **Preconditions:** Controller is in Local Service.
*   **Operator Actions:**
    1. Click "Exit Local Service" on the dashboard.
    2. Reconnect to the AP and reload the page after reboot.
*   **Expected Dashboard Observations:**
    *   Controller boots into `COMMISSIONED_STA`.
    *   Connection logic correctly resumes from scratch.
*   **Actual Result:** `[PENDING]`

### Test 10: Final Pure-Suite Execution with Production-Isolation Proof
*   **Objective:** Prove no mock leakage or unintended production mutations occurred.
*   **Preconditions:** Final candidate firmware is built and flashed.
*   **Operator Actions:**
    1. Reboot the controller (physically or via portal).
    2. Observe the boot sequence serial output.
*   **Expected Dashboard/Serial Observations:**
    *   `Pure suite: 98 distinct × 3 passes = 294/294`
    *   `Production isolation: VERIFIED`
*   **Actual Result:** `[PENDING]`
