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
| 5* | Two lease defects found by the first suite run | `497e22a` |

Not complete: **§9** seam-level integration tests, **§11** KDF timing
measurement. **§10** items 5–7 (real authenticated authority request) were
explicitly deferred by Shawn until the HMI can produce one genuinely, rather
than manufacturing a credentialed client for the purpose.

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

**Implemented and unit-tested:** all of §1–§8, at the pure-core level.

**NOT integration-tested:** the seam from authenticated principal through
topic classification, scope authorization, permission lookup, broker
admission, strict decoding, session/epoch validation, arbitration and result
publication has **never been exercised end to end**. That is §9 and it is the
largest remaining gap.

**NOT proven through the real authenticated broker:** no authority request has
ever traversed the actual path. §3's rejection matrix — roughly fifteen refusal
paths — is validated by inspection and compilation only, with no negative
tests. A green suite does not vindicate it.

**Deferred to HMI #67:** the real authenticated authority request, its
rejection counterpart, and everything that depends on a command-capable panel.

**Still requiring physical motion acceptance:** all of it. Nothing in this pass
has moved a motor and nothing should until Phase 5.

## 5. Watch item

Diagnostic task stack high-water is falling as tests are added:
3904 → 2652 bytes free of a 12 KB stack across recent runs. Not yet a problem,
but §9 will add more and the trend should be checked before it bites.
