# Authority Correction Pass — Evidence Record

**Branch:** `atlantis-authority-integration` (both repositories)
**Status: NOT ACCEPTED. NOT MERGED.** Recorded for checkpoint review.
**Date:** 2026-07-26

Evidence for the correction pass ordered after the
`atlantis-authority-integration` checkpoint. Sections refer to that order.

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
| 9 | Seam-level integration tests | `<pending — this commit>` |
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

**Implemented and unit-tested:** all of §1–§9 and §11, at the pure-core level,
plus — as of §9 — the composed seam from authenticated principal through topic
classification, scope authorization, permission lookup, broker admission,
strict decoding, session/epoch validation, and arbitration.

**What §9 closes, precisely:** 24 tests compose the real production functions
— `argus_mqtt_topics_classify`, `argus_mqtt_security_publish_allowed`,
`argus_mqtt_security_required_permission`,
`argus_mqtt_decode_authority_request`, `argus_mqtt_session_request_authority`,
`argus_mqtt_session_release_authority` — against a synthetic
`argus_mqtt_broker_message_t` and `argus_machine_principal_t`, in the gate
order transcribed from `argus_mqtt_runtime.c`
(`broker_publish_authorize_cb` → `broker_policy_cb` → `handle_message` →
`handle_authority_request`). Every function called is the real one; nothing
is stubbed. §3's rejection matrix — previously validated by inspection and
compilation only — now has ~60 negative-payload cases, each asserted against
its specific decode result, run through the full seam against a controller
holding a live lease and an accepted command, with the session core asserted
byte-identical (`memcmp`) after every one.

**What §9 does NOT close, precisely — the gate order is MIRRORED, not
EXECUTED:** `handle_authority_request` is `static` and requires the broker, a
FreeRTOS queue, and a mutex, so it is not the function under test; the seam
tests reproduce its gate sequence rather than calling it. **If the gate order
in `argus_mqtt_runtime.c` is ever changed, these tests will not detect it.**
Closing that residual risk needs `argus_mqtt_runtime` refactored to expose the
handler as a directly callable, dependency-injected function — a deliberate
change, not a hurried one. Also not modelled: A2.6 duplicate/replay
suppression, which lives in runtime file-scope statics; reimplementing it in
the test would only prove the test agrees with itself, so it is left
uncovered rather than faked. Real socket disconnect, rebind, and
different-principal inheritance across a live TCP drop remain covered only at
the §5 core level, not through this seam.

**One decoder finding surfaced and pinned, not fixed:** the "strict" decoder
accepts a trailing comma before an authority request's closing brace, and
trailing whitespace after it (`{"schema":1,...,"authority_epoch":1,}` decodes
`ARGUS_MQTT_DECODE_OK`). Every field is still bounded, typed, and validated —
nothing unintended gets through — but it is not strict JSON. Left as current
behaviour and pinned in `test_4c_seam_decode_documented_laxness` rather than
changed, since tightening it is a §3-shaped decision, not a §9 one. Shawn's
call whether the contract should be made exact.

**NOT proven through the real authenticated broker:** no authority request has
ever traversed the actual network path — broker socket, TLS/plaintext framing,
and the live FreeRTOS queue are all still synthetic in every test that exists.

**Deferred to HMI #67:** the real authenticated authority request, its
rejection counterpart, and everything that depends on a command-capable panel.

**Still requiring physical motion acceptance:** all of it. Nothing in this pass
has moved a motor and nothing should until Phase 5.

## 5. Watch item

Diagnostic task stack high-water across the three runs of this pass:
3904 → 2652 → **1988** bytes free of a 12 KB stack. Still falling, now under
2 KB free, though it held steady within the §9 run itself (1988 before and
after). §9's own tests were built to minimize their footprint on this stack —
the largest structure (`argus_mqtt_topics_t`, ~6.8 KB) is a single shared
file-scope table, not a per-test stack local — so the drop from 2652 to 1988
is attributable to the volume of new test code executing on the diagnostic
task, not to the §9 fixtures being heavy. No further correction-order work is
scheduled to add tests to this suite, so the trend has no known next input,
but 1988 bytes is closer to a real problem than "not yet a problem" and
`ARGUS_DIAGNOSTIC_TASK_STACK` (currently 12288) should be raised before
anything else adds to this suite.
