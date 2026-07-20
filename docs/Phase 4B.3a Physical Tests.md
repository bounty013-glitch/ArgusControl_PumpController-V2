# Phase 4B.3a - Physical Tests: Wi-Fi Observability and Recovery

All ten tests in this checklist remain pending execution. This is not physical-readiness or acceptance evidence until independent source review authorizes flashing and the recorded results are reviewed.

## Prerequisites

1. Independent source review completed with no blocking findings.
2. Controller flashed with the reviewed Phase 4B.3a candidate.
3. Serial diagnostic console available.
4. Operator device connected to the controller Service AP at `http://192.168.4.1`.

## Test 1: Correct-credential baseline

Enter valid commissioned Wi-Fi credentials and save them.

Expected: STA progresses through `CONNECTING`, `ASSOCIATED_WAITING_IP`, and `CONNECTED`; the controller remains `AP_DISCOVERABLE`; an IP is displayed; the broker runs; `SUPERVISORY/MQTT` is granted only after IP and broker startup.

Actual: `[PENDING]`

## Test 2: Connected apply ordering

While connected, save a different valid credential set and observe serial/dashboard state.

Expected: recovery reports `WAITING_DISCONNECT`; authority is `NONE/NONE`; broker is stopped; disconnect completes before configuration apply; apply completes before connect; AP and HTTP stay available.

Actual: `[PENDING]`

## Test 3: Incorrect password and bounded authentication retry

Save the valid SSID with an incorrect password and observe three authentication failures.

Expected: raw reasons and `AUTHENTICATION` category remain visible; retries enter `RETRY_WAIT`; after the configured authentication streak the STA enters `ACTION_REQUIRED`; broker remains stopped; AP and HTTP remain available.

Actual: `[PENDING]`

## Test 4: Manual reconnect with unchanged bad credentials

From `ACTION_REQUIRED`, press Reconnect once, then attempt a duplicate request while recovery is pending.

Expected: the first request enters `CONNECTING`; prior reason/category/count/guidance remain visible while pending; duplicate recovery is rejected; failed recovery remains truthful without clearing history merely because the button was pressed.

Actual: `[PENDING]`

## Test 5: Correct credentials and successful recovery

From `ACTION_REQUIRED` or `RETRY_WAIT`, save correct credentials.

Expected: asynchronous apply completes only after IP acquisition; STA becomes `CONNECTED`; active failure telemetry clears; broker restarts; `SUPERVISORY/MQTT` is restored; mode remains `AP_DISCOVERABLE`.

Actual: `[PENDING]`

## Test 6: AP unavailable and automatic recovery

Disable the commissioned upstream AP, then restore it.

Expected: raw reason remains observable and category is `AP_UNAVAILABLE`; retries continue without an authentication `ACTION_REQUIRED` transition; countdown does not underflow; restoring the AP returns to `CONNECTED` and clears active failure telemetry.

Actual: `[PENDING]`

## Test 7: Service AP and HTTP preservation

During retry, action-required, connected apply, and manual recovery states, remain attached to the Service AP and refresh the dashboard.

Expected: dashboard remains reachable at `192.168.4.1`; state and retained failure evidence remain truthful throughout recovery.

Actual: `[PENDING]`

## Test 8: Service-entry cancellation

Begin while retry or recovery is active, then enter browser Local Service.

Correction history: review after `d8c898f` found that the checklist required this path while production rejected `AP_DISCOVERABLE + NONE/NONE` and mutated recovery state before that rejection. The corrected candidate must be used; this test has not yet been executed against it.

Expected preflight: **Enter Local Service** is visible only when the server-derived `service_entry_permitted` result is true. During commissioned disconnected recovery this requires Service AP active, STA disconnected, no STA IP, broker observably stopped, exact `NONE/NONE` authority, and `RETRY_WAIT`, `ACTION_REQUIRED`, `IDLE`, or an active recovery transaction. Introduce each contradictory condition where practical and confirm rejection leaves transaction generation, timers, failure evidence, counters, network mode, authority generation, broker, and STA state unchanged.

Expected transition: retry and IP-timeout timers and active Wi-Fi transactions are cancelled only after eligibility revalidation; delayed events do not restart recovery; STA is disabled/idle in `SERVICE_AP_ONLY`; reconnect is unavailable; staged password material is scrubbed and not exposed. If state changes between HTTP preflight and queued execution, execution rejects without cancelling recovery and retains truthful operator evidence.

Actual: `[PENDING]`

## Test 9: Service exit and commissioned reboot

Exit Local Service and reconnect to the Service AP after reboot.

Expected: commissioned operation returns to `AP_DISCOVERABLE` under the always-on AP policy; STA starts from a clean lifecycle; stale transaction/timer events are rejected.

Actual: `[PENDING]`

## Test 10: Manual pure-suite execution and isolation proof

Open the serial diagnostic console and select option `t`.

Expected: the suite reports runtime-derived distinct, repeat, total, passed, and failed counts plus `Production isolation: VERIFIED`. No final count is assumed from source or compilation. Preserve complete console output.

Actual: `[PENDING]`

## Acceptance note

All ten physical tests, including corrected Test 8, remain pending. No physical acceptance has occurred. Any source-derived registration count is provisional until diagnostic option `t` executes on hardware.
