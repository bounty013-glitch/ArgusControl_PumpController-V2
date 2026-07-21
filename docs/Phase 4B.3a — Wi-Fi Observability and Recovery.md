# Phase 4B.3a - Wi-Fi Observability and Recovery

## Acceptance status

Phase 4B.3a is COMPLETE AND ACCEPTED as of July 21, 2026. Acceptance applies to runtime firmware commit `87eff30f36c9d264351ee939ff4061116c0dd128`; the later screenshot commit `62674ad26312cab040cbe6f72661b7d6f1593db5` contains evidence only, and accepted feature-branch record head `4766d96d3845483828dfbfc1aa83eb77a72dd52e` contains the final physical-test record. See [Phase 4B.3 and 4B.3a - Final Acceptance](Phase%204B.3%20and%204B.3a%20-%20Final%20Acceptance.md).

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

### Post-`5daab467` composition correction

The final Phase 4B.3a firmware candidate version is `v2-phase4b.3a-dev`. The ESP-IDF application descriptor is the single displayed firmware-version authority; the identity response copies that value once without adding a second hardcoded phase string. The identity-correction candidate was full-clean built and sized with ESP-IDF v5.5.3 at `0xfab00` bytes, leaving `0x205500` bytes (67%) in the smallest application partition, with zero compiler warnings, zero compiler errors, and zero failed commands. Embedded JavaScript source was unchanged from `085e8ef`.

Review after `5daab467` found that the event decision was correct but its application seam was not. Every ignored event cleared all three physical STA flags. In operational modes, a queued prior-generation disconnect could therefore clear `sta_started` after a new generation had begun connecting, contradicting the Wi-Fi callback's immediate physical observation and preventing current-generation association/IP events from completing the transaction. The application seam now receives the lifecycle event type. Ignored operational events preserve callback-established physical flags; service transition and AP-only modes may still force physical absence; and only a processed STA-stop observation clears `sta_started` in operational modes.

The same review found that several production paths treated "not `RUNNING`" as equivalent to convergently stopped. They now consume one authoritative lifecycle observation: stopped means `STOPPED`, no server task, no listener, and zero clients. `STARTING`, `STOPPING`, contradictory `STOPPED`, incomplete `RUNNING`, and observation failure all fail closed. Stop is idempotently successful only for the full stopped predicate; otherwise the real stop operation runs and its exact error propagates. Running remains `RUNNING` plus server task plus listener; client count is irrelevant to that running predicate. Diagnostic status now distinguishes `STOPPED`, `NOT CONVERGED`, and `UNOBSERVABLE`.

Two pure tests were added. The stale-event race test advances the active transaction generation, rejects a queued old-generation disconnect without clearing `sta_started` or changing evidence/authority/broker/counters/timers, accepts current association and IP, completes the transaction, preserves ignored operational association/IP callback observations, suppresses delayed service events, and proves a true stop may clear started. The broker production-decision test covers converged and contradictory `STOPPED`, `STARTING`, coherent and incomplete `RUNNING`, `STOPPING`, observation failure, exact stop failure propagation, successful post-stop convergence, and verification timeout. Existing service-entry and Wi-Fi-apply orchestration tests continue to prove that a broker-verification failure stops the callback chain.

The source now has 142 actual `RUN_TEST(...)` registrations, including 48 Phase 4B.3a registrations, for an expected 426 executions across three passes. These are source-derived provisional counts, not runtime results. No runtime count is final until diagnostic option `t` executes on hardware. All ten physical tests remain pending; this correction does not claim readiness to flash, physical readiness, or acceptance.

This correction was full-clean built and sized with ESP-IDF v5.5.3 using the host-stable serial method: `CMAKE_BUILD_PARALLEL_LEVEL=1`, `--no-ccache`, `CMAKE_JOB_POOLS=compile_pool=1`, and `CMAKE_JOB_POOL_COMPILE=compile_pool`. The build completed 1,096 commands with zero compiler warnings, zero compiler errors, and zero failed commands. The application image is `0xfab10` bytes in the `0x300000`-byte smallest app partition, leaving `0x2054f0` bytes (67%) OTA headroom. All four extracted embedded JavaScript blocks passed Node.js syntax validation. These are static/build facts only.

## Final acceptance

The remaining gates listed during correction were subsequently completed. Both hardware executions of diagnostic option `t` reported 142 distinct tests repeated three times, for 426 passed and 0 failed executions per run. Boot identity, pure-suite preflight, Physical Tests 1-9, and the final pure-suite/isolation proof (Test 10) all passed. The observed Test 3 wording differed from the planned wording but remained truthful, understandable, actionable, and acceptable; it was not a runtime defect or acceptance exception.

Phase 4B.3a was physically accepted on July 21, 2026, with no known acceptance-blocking defects. Historical pending statements above describe earlier review checkpoints and are retained as correction history; they do not describe the final disposition.
