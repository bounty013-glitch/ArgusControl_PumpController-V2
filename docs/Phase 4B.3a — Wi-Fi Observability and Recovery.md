# Phase 4B.3a - Wi-Fi Observability and Recovery

## Acceptance status

Phase 4B.3a is implemented on `phase4b3a-wifi-observability` and is awaiting an independent source review followed by physical runtime verification. A clean compile is not runtime-test evidence. This document does not claim readiness to flash, physical acceptance, or a final passing test count.

## Runtime design

The network manager exposes STA lifecycle state, raw ESP-IDF disconnect reason, translated failure category, consecutive-failure count, retry countdown, operator guidance, and whether manual reconnect is currently permitted.

Wi-Fi configuration apply is a nonblocking generation-tagged transaction with these states:

`IDLE`, `PREPARING`, `WAITING_DISCONNECT`, `APPLYING_CONFIG`, `CONNECTING`, `COMPLETE`, `FAILED`, and `CANCELLED`.

The transaction stops retry/IP timers, invalidates prior timer generations, revokes authority to `NONE/NONE`, stops and verifies the MQTT broker, loads and validates commissioned configuration, and preserves validated credentials across an intentional disconnect. A matching disconnect event resumes configuration application and connection. Successful IP acquisition completes the transaction and clears active failure evidence. All terminal failures and cancellation paths erase staged password material.

Raw disconnect classification remains independent of intent. In particular, raw `WIFI_REASON_ASSOC_LEAVE` remains `AP_UNAVAILABLE`. It bypasses failure accounting only when consumed by the active generation-matched intentional-disconnect transaction.

## Manual reconnect policy

Manual reconnect requires a commissioned-capable network mode and valid commissioned credentials. It is permitted in `ACTION_REQUIRED`, `RETRY_WAIT`, and `IDLE`; `IDLE` additionally depends on valid credentials. It is rejected in `DISABLED`, `CONNECTING`, `ASSOCIATED_WAITING_IP`, `CONNECTED`, `SERVICE_TRANSITION`, and `SERVICE_AP_ONLY`.

A permitted reconnect uses the shared recovery transaction, preserves the prior disconnect reason/category/counters while recovery is pending, and clears active failure evidence only after successful IP acquisition. The AP and HTTP portal remain available because recovery changes the STA lifecycle without leaving `AP_DISCOVERABLE`.

## Authority policy

Only these mode/owner pairs are valid:

- `NONE/NONE`
- `SUPERVISORY/MQTT`
- `SERVICE_TRANSITION/NONE`
- `LOCAL_SERVICE/BROWSER`
- `LOCAL_SERVICE/DIAGNOSTIC_CLI`

The pure validator is enforced before production authority state changes. Invalid requests preserve mode, owner, and generation.

## Test and evidence correction history

Earlier Phase 4B.3a passes contained or reported missing Wi-Fi observability, undefined dashboard fields, invalid embedded JavaScript, a synchronous disconnect/connect race, no durable pending transaction, invalid authority pairing, placeholder tests, a pure test that mutated the production authority singleton, contradictory 97/98/119/120 test counts, ESP-IDF v5.5.1 build-log contamination, and premature physical-readiness language. Those failures remain part of the record; this closure does not rewrite them as first-pass success.

The pure suite is launched manually with diagnostic option `t`; it does not execute automatically during boot. The source-derived registration count is provisional and must not be promoted to a final count or passing result until the corrected suite executes on hardware.

### Post-review service-entry correction

After `d8c898f`, triple independent review found that Local Service entry was unavailable in the exact disconnected recovery states required by Physical Test 8. The endpoint admitted only `AP_DISCOVERABLE + SUPERVISORY/MQTT` or `UNCOMMISSIONED_AP + NONE/NONE`, while recovery truthfully held `AP_DISCOVERABLE + NONE/NONE`. The production request path also cancelled transaction and timer state before rejecting that authority pair.

The correction uses one production policy for dashboard permission, `/api/service/enter` preflight, and network-manager execution. Commissioned recovery entry requires all of the following: active Service AP; disconnected STA; no STA IP; broker observably stopped; exact `NONE/NONE` authority; commissioned configuration; no transition in progress; and `RETRY_WAIT`, `ACTION_REQUIRED`, `IDLE`, or an active generation-tagged recovery transaction. Any contradictory fact rejects. Existing normal and uncommissioned entry paths remain accepted when the Service AP is active.

Queued browser entry carries its accepted snapshot fingerprint. The network manager revalidates the fingerprint and policy before and after dispatch serialization, with no recovery, evidence, network, authority, broker, or STA mutation on rejection. After acceptance, cancellation invalidates transaction and timer generations, scrubs staged credentials, attempts both timer stops, and reports the exact failing timer operation if either command fails. The server exposes `service_entry_permitted`, and dashboard JavaScript consumes that result without recreating policy from network mode.

The earlier review singled out Physical Test 8, but no checklist item had executed: all ten physical tests remain pending. No hardware execution, physical readiness, or acceptance is claimed. The corrected registration count remains source-derived and provisional until diagnostic option `t` runs on the controller.

## Build verification

The closure candidate was full-clean built and sized with ESP-IDF v5.5.3:

- Build result: success
- Compiler warnings: 0
- Compiler errors: 0
- Application image: `0xf5a80` bytes
- Smallest application partition: `0x300000` bytes
- OTA headroom: `0x20a580` bytes (68% free)

These are build facts only. They do not establish a pure-suite runtime pass or physical acceptance.

### Correction build verification

The post-`d8c898f` correction was full-clean built and sized with ESP-IDF v5.5.3:

- Build result: success
- Compiler warnings: 0
- Compiler errors: 0
- Application image: `0xf7860` bytes
- Smallest application partition: `0x300000` bytes
- OTA headroom: `0x2087a0` bytes (68% free)
- Embedded JavaScript: 4 of 4 script blocks passed syntax validation
- Source registrations: 139 total, including 45 Phase 4B.3a; provisional until hardware execution

These correction-build facts supersede the earlier candidate size for the new commit only. They do not erase the earlier build record and do not establish runtime or physical acceptance.

### Final independent-review runtime findings

The `0e5aa1b` correction remains the valid recovery service-entry policy fix. A subsequent independent review found additional runtime-state defects outside that policy decision:

- AP-only service entry did not explicitly converge authoritative STA state to `DISABLED`.
- Delayed STA IP-success events could mutate state and erase evidence before service-mode exclusion.
- The live retry countdown still used underflow-capable unsigned `expiry - now` arithmetic.
- Timer-cancellation failure could advertise a retry whose generation had already been invalidated.
- Broker `STOPPED` observation did not itself prove absence of task, listener, and clients.

The final closure adds post-verification STA disablement to the service orchestrator; mode/generation decisions before every STA association, IP, disconnect, and stop mutation; modular half-range tick arithmetic; explicit timer-cancellation failure identity/error and `ACTION_REQUIRED` guidance; and coherent broker-stop observation. AP-only delayed disconnect/stop events retain `DISABLED`, while delayed association/IP events cannot clear evidence, counters, errors, or restart broker/authority behavior.

The earlier policy correction was not ineffective; these were later runtime convergence and event-ordering findings. All ten physical tests remain pending, and no final runtime test count exists until diagnostic option `t` executes on hardware.

### Final runtime-state closure build

The final source was full-clean built and sized with ESP-IDF v5.5.3:

- Build result: success
- Compiler warnings: 0
- Compiler errors: 0
- Failed build commands: 0
- Application image: `0xf9820` bytes
- Smallest application partition: `0x300000` bytes
- OTA headroom: `0x2067e0` bytes (68% free)
- Embedded JavaScript: 4 of 4 script blocks passed syntax validation
- Source registrations: 140 total, including 46 Phase 4B.3a; provisional until diagnostic `t` execution

These facts supersede only the prior candidate size and source-registration checkpoint. They do not claim diagnostic-suite execution, physical readiness, or acceptance; all ten physical tests remain pending.

## Remaining acceptance

1. Complete an independent read-only source review.
2. Flash only after that review authorizes physical testing.
3. Run diagnostic option `t` and record the runtime-derived distinct/execution/pass/fail counts.
4. Execute the physical checklist and preserve serial/dashboard evidence.
5. Claim acceptance only after the physical results are reviewed.
