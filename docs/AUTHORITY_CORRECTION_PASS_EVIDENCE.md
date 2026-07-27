# Authority Correction Pass — Evidence Record

**Branch:** `atlantis-authority-integration` (both repositories)
**Status: NOT ACCEPTED. NOT MERGED.** Recorded for checkpoint review.
**Date:** 2026-07-26

Evidence for the correction pass ordered after the
`atlantis-authority-integration` checkpoint. Sections refer to that order.

## 0. Independent-review correction order (2026-07-27)

**Commit:** `215eeae` (code and tests). This document's own update is a
separate, later commit, per this document's established practice of citing
already-landed work rather than a commit describing itself.

An independent review of this pass, after §1–§11 above were believed
complete, found the §9 seam tests real but insufficient proof of certain
claims, and found several production defects the §9 seam had not yet been
built wide enough to catch. This section records what that review found and
what was corrected in response — the corrections are code-level facts,
verified by a clean build; the hardware suite run that proves them on target
is Shawn's, and is recorded separately once it lands (§2.3), not fabricated
here.

**1. `event/core/authority_result` did not exist.** A2.1/A2.4 define this as
the authority result channel. The implementation instead published every
authority outcome onto `event/pump1/command_result` — the PUMP's result
channel. Added the topic (`argus_mqtt_contract.h/.c`), added
`event`/`state`/`status`/`telemetry`-style category matching for it in
`argus_mqtt_security.c` so an HMI can subscribe (previously only an exact
literal match to `command_result` worked — `event/#` and a direct
subscription to the new topic both failed closed), and moved all
publication onto it.

**2. The result was missing `core_lease_status` and `local_control_status`.**
A2.4's schema requires both. Added; both are computed by the same pure
function the retained snapshot uses (see #6), so the result message and the
retained snapshot can never disagree.

**3. `transfer_unsupported_running` did not exist.** A2.8 requires an
ArgusCore transfer from `LOCAL_HMI`/`SERVICE_TOOL` to be refused while the
pump is actively driving the motor (`STARTING`/`RUNNING`/`DECELERATING`
— `HOLDING` has step generation stopped and is not gated). Nothing consulted
machine state before this pass; a running-transfer request would have been
GRANTED. Added `argus_mqtt_session_request_authority_with_state()`, an
additive wrapper around the existing pure core function (chosen over
modifying the base function's signature, which ~30 existing tests call
directly and have no notion of machine state) that peeks the same
held/preempt facts and refuses before ever calling the mutating base
function.

**4. `denied_machine_state` (A2.3's release-side machine-state gate) is
NOT implemented.** The contract lists it as a possible release outcome
("machine-state and profile policy admit it") but defines no concrete rule
for what machine state should block a release, anywhere in the contract or
the governing decision documents this repository has. A2.8's running-transfer
rule is well-specified and is #3 above; nothing analogous exists for
release. Inventing a rule here was judged a bigger risk than leaving it
unimplemented — release is documented as never stopping the pump or
disturbing anything but ownership, and a wrong invented gate could make a
legitimate release fail for a reason nobody decided. **Reported, not guessed
at. Needs Shawn's decision**, same disposition as the KDF iteration-budget
finding in §11.

**5. The duplicate-request cache was single-slot.** A2.6 requires idempotent
replay per `(session, request_id)`. The single-slot cache could only
remember the ONE most recently seen key; two requests interleaved on the
wire — A, then B, then a redelivery of A — would have B silently evict A's
remembered result, so the redelivery of A would be re-arbitrated instead of
replayed. Replaced with a bounded 8-entry, FIFO-evicted cache
(`ARGUS_MQTT_AUTH_DUP_CACHE_SIZE`). Eviction is explicit and documented in
source: the oldest entry is displaced regardless of whether it is still "in
flight"; a redelivery arriving after 8 other distinct requests is
re-arbitrated rather than replayed, which is safe (full admission runs
again) even though it is not the A2.6-idempotent path. Memory cost is fixed
at ~7.7 KB regardless of traffic (confirmed in the link map:
`.bss.s_auth_dup_cache = 0x1e00`).

**6. Published ownership was untruthful while disconnected.** A2.11:
"published ownership must remain truthful even while the link is offline."
The retained-snapshot path required `link == ONLINE` to consider a lease
"owned" at all, so `status/core/control_owner` and
`status/core/local_control_status` reported `NONE`/`AVAILABLE` for a lease
that was merely disconnected but not expired — while the pump was, in fact,
still under a live lease. Fixed: ownership is `lease_machine_id[0] != '\0'`
alone, matching what the pure core already guarantees (disconnect preserves
that field; only expiry clears it). The lease-status/local-status
computation was factored into one pure function
(`authority_status_from_core()`, exported for direct testing) used by both
the retained snapshot and the per-request result, so they cannot drift
apart again.

**7. Three of the six listed transitions never republished the snapshot.**
§4's list is acquisition, release, disconnect, rebind, lease expiry,
fallback. Acquisition and release already republished. Disconnect, lease
expiry, and the ArgusCore acquisition window closing with nobody having
acquired (fallback) did not — nothing touched the six A2.10 topics on any of
those paths, so a subscriber could see a stale snapshot indefinitely after
any of them. All three fixed: disconnect and expiry each call
`publish_authority_state()` on their own event; the window-close case has no
discrete triggering event (it is a pure function of elapsed time), so it is
polled once per `argus_mqtt_runtime_tick()`, comparing this tick's
window-open state to last tick's. Rebind (reconnect within the lease term)
was also added, gated on the link actually transitioning from non-ONLINE
back to ONLINE, so a healthy heartbeat cadence does not turn into a steady
stream of redundant republishes.

**8. The decoder accepted a trailing comma.**
`{"schema":1,...,"authority_epoch":1,}` decoded `OK`. Not a hole — every
field was still bounded, typed, and validated — but not strict JSON, and the
contract's stated strictness ("trailing content is rejected") gives no
exception for it. Fixed in all three decoders sharing this parsing shape
(heartbeat, command, authority request) for consistency, since the defect
was structural to the shared parsing pattern, not specific to one decoder.
Trailing whitespace AFTER the closing brace remains explicitly permitted —
a different thing from a comma inside the object, and never confusable with
content.

**9. `schema_unsupported` and `invalid_request_id` did not exist as distinct
reasons.** Both fell through to the generic `invalid_value`. A2.5 gives them
their own wire vocabulary. Added two decode-result values used only by the
authority-request decoder; every other field's failure still falls through
to `invalid_value` as before.

**10. `intent` accepted any string.** A2.2 restricts it to
`OPERATOR_INTENT`/`SUPERVISORY_START`/`SERVICE`/`FALLBACK` when present.
Unsupported values are now refused (`invalid_value`) rather than silently
accepted and recorded.

**11. The §9 seam tests mirrored `handle_authority_request`'s gate order
instead of calling it.** Correct in what they modelled, but a rename or
reorder inside the real handler could have drifted from the test silently.
Extracted the handler's logic (from the point broker-level admission and
the `REQUEST_AUTHORITY` capability check complete — both still the caller's
job, unchanged) into `argus_mqtt_authority_admit()`, taking the session core
and message as explicit parameters instead of reading the module's global
runtime state. Both the production runtime task and every §9 seam test now
call this exact function. 24 new tests exercise it directly: the running-
transfer rule, the bounded duplicate cache under real interleaving (A, B,
redelivery of A), FIFO eviction, the `authority_result` topic and full
schema, and the disconnected-truthful-ownership fix. The two gates outside
`argus_mqtt_authority_admit()` by design (broker publish admission, the
capability check) are still transcribed in the test harness, each calling
the real production function for that gate — documented in source as the
one place a future gate reorder at that specific boundary would not be
caught automatically.

**Wire-vocabulary correction found along the way:** the published rejection
reason for a profile denial was `denied_by_commissioned_profile`, which was
never valid A2.5 surface (the contract specifies `denied_by_profile`).
Corrected. Also consolidated two independent reason-string mappings (one for
the audit log, one — bounds-clamped by array index — for the published
result) into one `switch`-based function used by both, so adding
`TRANSFER_UNSUPPORTED_RUNNING` did not silently fall through a stale bounds
check the way the old array pattern would have.

**Not addressed by this pass, stated precisely:**
- #4 above (`denied_machine_state` release gate) — no rule to implement.
- §5.10 of the HMI Phase 3 plan (audit): authority events are logged, not
  yet routed to the persistent security audit log the way MQTT connect/policy
  events already are.
- The two broker-level gates still transcribed in the seam test harness
  (#11 above) rather than called through a single production entry point —
  would need `argus_mqtt_runtime`'s broker callback wiring itself refactored,
  a larger and more deliberate change than this pass's scope.
- No authority request has ever traversed the real authenticated broker path
  end to end — deferred to HMI issue #67, per Shawn, unchanged from §9's own
  disposition below.

## 1. Sections complete

| § | Subject | Commit |
|---|---|---|
| 1 | Amendment A2 — complete authority wire contract | `1978bcb` |
| 2 | Broker authorization for authority actions | `10945a0` |
| 3 | Strict request/release decoding | `7f9e888` |
| 4 | `authority_epoch` enforced on operational commands | `d60862f` |
| 5 | Transport state separated from lease state | `dc5d6ab` |
| 6 | `core_lease_status` + `authority_reason` published | `165577c` |
| 7 | Acquisition window anchored to service readiness | `10945a0` |
| 8 | Bounded provisioning body read (HMI) | `e5a5dde` |
| 9 | Seam-level integration tests | `e9ad8a8` |
| 11 | KDF timing measured; iteration-budget inconsistency flagged | `1af6e8e` |
| 5* | Two lease defects found by the first suite run | `497e22a` |

**All eleven sections of the correction order are now complete.** §10 items
5–7 (a real authenticated authority request) remain explicitly deferred by
Shawn until the HMI can produce one genuinely, rather than manufacturing a
credentialed client for the purpose — that is scoped to HMI issue #67, not to
this pass.

## 2. Test evidence — on hardware, COM5

Second run, after the two defects below were fixed:

```
Distinct Tests : 289
Repeat Passes  : 3
Total Executed : 867
Total Passed   : 867
Total Failed   : 0

Production Isolation (Read-Only Proof):
  Authority Generation : UNCHANGED (Gen 3)
  Network State        : UNCHANGED (AP_DISCOVERABLE)
  MQTT Broker State    : UNCHANGED (RUNNING)
  Machine State        : UNCHANGED (UNLOCKED)
  Task Count           : UNCHANGED (24 tasks)
  AP Stations          : STEADY (1 -> 1)
  Broker Clients       : STEADY (1 -> 1)

PHASE 4D.4 PURE UNIT TEST SUITE: PASSED
```

289 distinct tests, up from 276 at the checkpoint. Both ESP-IDF v5.5.3 builds
(controller and HMI) are clean with **zero warnings**.

### 2.1 The first run found two real defects

The first execution returned 861/867 — two tests failing on all three passes.
Both were genuine defects in the §5 correction, not test errors:

1. **A lease that could never expire.** `argus_mqtt_session_tick()` keyed
   expiry on `lease_machine_id[0] != '\0'`, so a lease established without a
   recorded identity — the legacy path the contract still supports — would
   have held authority indefinitely. Worse than the bug §5 set out to fix.
2. **The fix reintroduced the vulnerability it was fixing.**
   `argus_mqtt_session_request_authority()` still derived `held` from
   `link == ONLINE`. Once a lease survived a disconnect, the link read OFFLINE
   while the lease was valid, so a **different principal was granted a lease
   that was still held** — precisely the inheritance transport/lease
   separation exists to prevent.

Both fixed in `497e22a`.

**Defect 2 was caught by a test written in the same commit as the code it
disproved.** That is the outcome the §5 methodological correction was meant to
produce: exercise the real production entry point, not a convenient one. The
contrast is the whole argument for this pass — the test it replaced
(`epoch_survives_reconnect_blip`) had passed indefinitely against the opposite
behaviour because it never called the disconnect callback at all.

### 2.2 Third run — after §9, on hardware, COM5

24 seam-level tests added (313 − 289). Shawn ran the suite; result reported
verbatim:

```
Test Execution Summary:
  Distinct Tests : 313
  Repeat Passes  : 3
  Total Executed : 939
  Total Passed   : 939
  Total Failed   : 0

Production Isolation (Read-Only Proof):
  Authority Generation : UNCHANGED (Gen 11)
  Network State         : UNCHANGED (AP_DISCOVERABLE)
  MQTT Broker State     : UNCHANGED (RUNNING)
  Machine State          : UNCHANGED (UNLOCKED)
  Task Count             : UNCHANGED (24 tasks)
  AP Stations            : STEADY (2 -> 2)
  Broker Clients         : STEADY (1 -> 1)

PHASE 4D.4 PURE UNIT TEST SUITE: PASSED
```

313 distinct tests, up from 289. 939/939 executions passed, 0 failed.
Isolation proof clean — including AP stations steady at 2 rather than the
prior run's 1, reflecting the HMI's own connection alongside whatever else was
on the bench at the time, not anything this pass touched.

#### 2.2.1 A build defect this run's own process caught before it reached hardware

The first attempt at the §9 build compiled clean but was never flashed: the
seam tests declared `argus_mqtt_topics_t` (~6.8 KB) as a `static` local in
thirteen separate test functions. Each got its own `.bss` copy — roughly
88 KB of duplicated static tables — which starved the Wi-Fi driver's internal
DRAM allocation at init:

```
W (2838) wifi:esf_buf_setup_static: alloc eb fail(10)
E (2841) wifi_init: Failed to deinit Wi-Fi (0x3001)
E (2841) argus_app_main: Network manager initialization failed: ESP_ERR_NO_MEM
```

Boot otherwise completed and the suite would likely still have passed — the
defect was in test code, not the code under test — but it is exactly the kind
of thing "compilation is not acceptance" exists to catch, and it reached
Shawn's bench before it was caught on this side. Fixed by replacing the
thirteen per-function statics with one file-scope table shared by every §9
test, rebuilt on entry to each one so no state carries between tests.
Confirmed in the link map: `.bss.topics` is a single 0x1a88-byte (6,792 byte)
symbol, not thirteen. The run recorded above is against the corrected build.

### 2.3 Independent-review correction order — hardware run PENDING

The eleven corrections in §0 above are code-level facts: the build is clean
with zero warnings, and the link map confirms the `.bss` figures cited
there. **They are not yet proven on hardware.** Per this document's own
established practice (§2.2's third run was recorded only after it actually
happened, not written in advance), the on-target suite run — distinct test
count, execution/pass/fail totals, isolation proof, and updated diagnostic
task stack high-water — will be recorded here as a follow-up entry once
Shawn has run it. Flashed and boot-verified as far as this side can confirm
without the interactive console (see §3); the console itself still requires
Shawn to press `t`.

## 3. §10 hardware items 1–4, 9–10

| Item | Result |
|---|---|
| 1. Flash correction branch | Done, 4× `Hash of data verified` |
| 2. Clean startup, no automatic suite run | Confirmed — no `[BOOT-TEST]`, `startup completed successfully` |
| 3. HMI still connects monitor-only | Confirmed — station `d0:cf:13:1e:13:98` joined, DHCP `192.168.4.2`, broker client present |
| 4. Telemetry subscriptions functional | Confirmed — broker client steady across the run |
| 9. Clean reboot | Confirmed — commissioned config intact, slot 1 gen 5, NVS schema V3 |
| 10. Trees clean, no credentials | Confirmed — both worktrees clean, secret scan of the full branch diff returns 0 |

Boot evidence: `App version v2-phase4d.4-dev`,
`Commissioned: YES (Valid / Commissioned)`,
`HTTP server started on port 80 (max_conn=4)`,
`authority acquisition window opened (45000 ms)` — the §7 anchor firing at
broker start rather than at MCU boot.

No motion command was issued and the motor was not energised at any point.

## 4. Remaining uncertainty — stated precisely

**Implemented and unit-tested:** all of §1–§9 and §11, at the pure-core
level, the composed seam from authenticated principal through topic
classification, scope authorization, permission lookup, broker admission,
strict decoding, session/epoch validation, and arbitration — and, as of the
§0 independent-review correction order, the machine-state-aware transfer
rule, the bounded duplicate cache under real interleaving, the
`event/core/authority_result` schema and topic, and the
disconnected-but-unexpired truthful-ownership fix.

**What the seam closes, precisely:** the production admission path is now a
directly callable function, `argus_mqtt_authority_admit()` — not a
transcription of `handle_authority_request`'s gate order, but the function
`handle_authority_request` itself calls. Both the runtime task and every §9
test call it. §3's rejection matrix — previously validated by inspection and
compilation only — has ~60 negative-payload cases, each asserted against its
specific decode result, run through the full seam against a controller
holding a live lease and an accepted command, with the session core asserted
byte-identical (`memcmp`) after every one.

**What still is NOT closed, precisely:** two gates remain outside
`argus_mqtt_authority_admit()` by design — broker publish admission and the
`REQUEST_AUTHORITY` capability check, both still the caller's documented
responsibility — and the test harness still transcribes those two,
calling the real production function for each rather than reaching them
through one production entry point. **If those two specific gates are ever
reordered relative to each other, or relative to what calls into
`argus_mqtt_authority_admit()`, the test will not detect it.** Closing that
residual risk would need `argus_mqtt_runtime`'s broker callback wiring
itself refactored — a larger, more deliberate change than this pass's scope.
Also not modelled: real socket disconnect, rebind, and different-principal
inheritance across a live TCP drop, which remain covered only at the §5 core
level, not through this seam — reaching further needs the same broker-wiring
refactor.

**The decoder finding from the prior evidence entry is now fixed, not just
pinned:** the trailing-comma laxness reported there
(`{"schema":1,...,"authority_epoch":1,}` decoding `OK`) has been corrected —
see §0 item 8. Trailing whitespace after the closing brace remains
explicitly, deliberately permitted; that was never the defect.

**`denied_machine_state` (A2.3's release-side machine-state gate) remains
unimplemented** — see §0 item 4. Not guessed at; needs Shawn's decision.

**NOT proven through the real authenticated broker:** no authority request has
ever traversed the actual network path — broker socket, TLS/plaintext framing,
and the live FreeRTOS queue are all still synthetic in every test that exists.

**Deferred to HMI #67:** the real authenticated authority request, its
rejection counterpart, and everything that depends on a command-capable panel.

**Still requiring physical motion acceptance:** all of it. Nothing in this pass
has moved a motor and nothing should until Phase 5.

## 5. Watch item

Diagnostic task stack high-water across the three runs recorded so far:
3904 → 2652 → **1988** bytes free of a 12 KB stack, holding steady within
the third run (1988 before and after). The independent-review correction
order adds roughly a dozen more test functions to this same suite; its own
effect on this number is **not yet measured** — see §2.3. The new
duplicate-request cache (~7.7 KB) is `.bss`, not stack, and does not bear on
this figure directly, but the additional test bodies executing on the
diagnostic task might. `ARGUS_DIAGNOSTIC_TASK_STACK` (currently 12288) was
already flagged as due for a raise before this pass; that recommendation
stands and has not gotten less urgent.
