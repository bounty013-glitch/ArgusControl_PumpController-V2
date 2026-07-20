# Phase 4B.3a - Implementation Plan: Wi-Fi Failure Observability and Recovery

## Objective

Provide truthful Wi-Fi failure telemetry and recovery while preserving the always-on Service AP, exclusive authority, nonblocking event handling, pure-test isolation, and operator evidence until recovery is proven.

## Runtime transaction

Configuration apply and manual reconnect share a caller-owned, generation-tagged transaction core. Production owns one transaction instance; pure tests own stack-local instances with injected operations.

The apply sequence is:

1. Reject a duplicate active transaction.
2. Stop retry and IP-timeout timers and invalidate old generations.
3. Revoke authority to `NONE/NONE`.
4. Stop the MQTT broker and verify it stopped.
5. Load and validate commissioned configuration, including identity invariants, SSID bounds, WPA2 password bounds, and mask-sentinel rejection.
6. If connected, stage the validated configuration, mark the disconnect intentional, request disconnect, and return to the event loop.
7. Resume only from a generation-matched disconnect event.
8. Apply STA configuration, erase staged password material, and request connection.
9. Complete only after a generation-matched IP-acquisition event.
10. On every failure or cancellation, report the originating error and erase staged secrets.

## Manual reconnect policy

Manual reconnect is allowed in `ACTION_REQUIRED`, `RETRY_WAIT`, and `IDLE` when valid commissioned credentials exist and the network mode is `COMMISSIONED_STA`, `AP_DISCOVERABLE`, or `NETWORK_FAULT`. It is rejected in `DISABLED`, `CONNECTING`, `ASSOCIATED_WAITING_IP`, `CONNECTED`, `SERVICE_TRANSITION`, and `SERVICE_AP_ONLY`.

For permitted states the STA is already disconnected, so the transaction connects without issuing a redundant disconnect. It does not reapply credentials. Previous failure reason, category, counts, and guidance remain visible while recovery is pending and clear only after IP acquisition.

## Timer policy

Each timer captures the active generation when scheduled. Callback events carry that captured value, and consumers reject stale values. Apply, reconnect, service entry, successful connection, and cancellation invalidate prior generations. Timer command failures are observable and a retry state is not claimed if scheduling fails.

## Pure verification

All Phase 4B.3a tests use caller-owned authority, transaction, evidence, and callback-trace state. Tests must cover valid/invalid authority pairs, asynchronous apply ordering, stale events, all required callback failures, configuration validation, secret clearing, successful completion, duplicate requests, reconnect policy, evidence retention, mixed failure streaks, countdown boundaries, queue truthfulness, service cancellation, and AP/HTTP preservation.

Every test is registered through `RUN_TEST()`. The source registration count remains provisional until diagnostic option `t` executes on hardware. Compilation does not establish a runtime pass.

## Post-`d8c898f` service-entry correction

Triple independent review after `d8c898f` identified a policy contradiction: the physical checklist required browser Local Service entry during Wi-Fi recovery, but both HTTP preflight and production execution rejected the truthful recovery authority pair `NONE/NONE`. Production also cancelled the active transaction and timers before completing that rejection. This history is retained; the defect was not present only in documentation.

The corrected shared policy preserves normal `AP_DISCOVERABLE + SUPERVISORY/MQTT` entry and uncommissioned `UNCOMMISSIONED_AP + NONE/NONE` entry. It additionally permits commissioned `AP_DISCOVERABLE + NONE/NONE` only when the Service AP is active, STA is disconnected, no STA IP is held, the broker is observably stopped, no service transition is active, and recovery is explicitly cancellable through `RETRY_WAIT`, `ACTION_REQUIRED`, `IDLE`, or an active generation-tagged recovery transaction. Connected STA, retained IP, running or unobservable broker, absent Service AP, uncommissioned recovery, mixed authority, ineligible lifecycle state, and an active service transition all reject.

HTTP preflight and queued execution use the same pure evaluator. An accepted browser request carries a fingerprint of the coherent eligibility snapshot. The network-manager task revalidates policy and that fingerprint under the network lock, then revalidates again after taking dispatch ownership. Rejection occurs before any transaction, generation, timer, evidence, counter, mode, authority, broker, or STA mutation.

Only after eligibility is proven does production invalidate recovery generations, cancel and scrub the transaction, and stop both timers before continuing through the existing service-entry orchestrator. A timer-command failure preserves the exact callback error and identifies whether retry-timer or IP-timeout stopping failed; generations remain invalidated so a delayed event cannot restart recovery.

The earlier correction record singled out Physical Test 8, but no Phase 4B.3a physical test had executed. All ten tests remain pending. At that checkpoint the runner contained 139 source registrations, including 45 Phase 4B.3a registrations; those historical source counts were provisional pending diagnostic option `t` execution on hardware.

## Final runtime-state and delayed-event closure

Commit `0e5aa1b` validly corrected recovery service-entry eligibility, shared dashboard/API policy, preflight fingerprinting, and mutation ordering. Independent review afterward found four separate runtime-state defects that the policy correction did not address:

1. Successful AP-only convergence did not explicitly commit the authoritative STA lifecycle to `DISABLED`, so a disconnected recovery state could remain reported as `RETRY_WAIT`, `ACTION_REQUIRED`, or `CONNECTING` in Local Service.
2. The production STA-IP handler mutated lifecycle, evidence, counters, errors, broker, and authority state before excluding delayed events in `SERVICE_TRANSITION` or `SERVICE_AP_ONLY`.
3. Production retry countdown still subtracted unsigned tick values before the tested millisecond-rounding helper, allowing an expired timer to appear as a huge future countdown.
4. A service timer-cancellation failure invalidated generations but could leave `RETRY_WAIT`, scheduled-retry guidance, and a visible countdown even though the event could no longer perform recovery.

The final closure adds a required `set_sta_disabled` orchestration operation after AP-only and physical STA-absence verification and before machine-safety revalidation and authority grant. Any later failure enters `NETWORK_FAULT` while retaining truthful `DISABLED` STA state.

STA association, IP acquisition, disconnect, and stop events now pass through an explicit mode/generation decision before lifecycle or recovery mutations. Association and IP events are ignored in both service modes; AP-only disconnect and stop events reconfirm `DISABLED`; stale transaction generations are ignored; operational modes, including explicit `NETWORK_FAULT` recovery, continue accepting legitimate events. Ignored service/stale events clear optimistic physical flags without clearing failure evidence, counters, network errors, broker state, or authority.

Retry countdown now derives a modular tick delta only when the timer is active, its generation is current, and state is `RETRY_WAIT`. Zero/past expiry and modular distances beyond the valid half-range return zero; future expiry across a 32-bit tick wrap remains valid. Millisecond multiplication is bounded before partial seconds round upward.

Timer-cancellation failure now records exact timer identity and `esp_err_t`, preserves prior disconnect evidence and counters, transitions to `ACTION_REQUIRED`, exposes zero retry countdown, and provides Reconnect Wi-Fi and Enter Local Service guidance. Invalidated delayed callbacks remain harmless and observable as ignored.

Broker `stopped` observation now requires `STOPPED`, no server task, no listener, and zero active clients. The final source contains 140 registrations, including 46 Phase 4B.3a registrations. These source-derived counts remain provisional until diagnostic option `t` runs on hardware. All ten Phase 4B.3a physical tests remain pending.

The final closure was full-clean built with ESP-IDF v5.5.3. The application image is `0xf9820` bytes in a `0x300000`-byte smallest application partition, leaving `0x2067e0` bytes (68%) OTA headroom. The clean log contains zero compiler warnings, zero compiler errors, and zero failed commands. All four embedded JavaScript blocks passed syntax validation. These are static/build facts only and do not establish runtime or physical acceptance.

## Post-`5daab467` composition-level correction

Independent review after `5daab467` found a narrower defect in the otherwise-correct generation gate. `argus_net_apply_sta_event_action()` did not know the event type and cleared `sta_started`, `sta_connected`, and `sta_ip_acquired` for every ignored event. That was valid for enforcing service-mode physical absence but invalid for operational stale events, because the Wi-Fi callback had already written its immediate physical observation. A queued old-generation disconnect processed after a new transaction began could falsely report the STA driver stopped and make the active generation's association/IP path ineligible.

The event application seam is now event-aware. Operational `IGNORE` preserves callback observations; processed disconnect/IP-timeout observations clear association and IP without implying driver stop; only a processed STA stop clears started in operational modes. `SERVICE_TRANSITION` and `SERVICE_AP_ONLY` continue forcing delayed events physically absent, and AP-only disconnect/stop confirmation leaves authoritative state `DISABLED`.

Broker shutdown decisions now distinguish "not `RUNNING`" from observably stopped. Production service-entry and Wi-Fi-apply wrappers observe the full broker composition before stop, attempt the actual stop unless the full stopped predicate is already true, propagate observation and stop errors exactly, and verify with bounded polling until `STOPPED` has no task, listener, or clients. `argus_mqtt_broker_is_running()` now requires coherent `RUNNING` state, task, and listener; active clients are not required for running. The broker stop implementation publishes `STOPPED` only after all resources are absent, and diagnostic output no longer labels every non-running state as stopped.

The corrective pure coverage includes the exact prior-generation disconnect/current-generation association-IP race and production-consumed broker stop/verification decisions for converged `STOPPED`, residual resources, `STARTING`, coherent/incomplete `RUNNING`, `STOPPING`, observation failure, successful post-stop convergence, and exact timeout/failure propagation. Service-entry and Wi-Fi-apply orchestration remain fail-closed at broker verification. The source contains 142 actual registrations, including 48 Phase 4B.3a registrations. Every source-derived or eventual runtime count remains provisional until diagnostic option `t` executes on hardware. All physical checks remain pending; no readiness or acceptance is claimed.

Final static verification used ESP-IDF v5.5.3 and the proven serial, ccache-disabled clean-build method: `CMAKE_BUILD_PARALLEL_LEVEL=1`, `--no-ccache`, `CMAKE_JOB_POOLS=compile_pool=1`, and `CMAKE_JOB_POOL_COMPILE=compile_pool`. `fullclean`, the 1,096-command build, and `size` succeeded. The log contains zero compiler warnings, zero compiler errors, and zero failed commands. The image is `0xfab10` bytes with `0x2054f0` bytes (67%) free in the `0x300000`-byte smallest application partition. Four of four extracted embedded JavaScript blocks passed Node.js syntax validation. These results do not establish diagnostic runtime or physical acceptance.
