# Authority Correction Pass — Evidence Record

**Branch:** `atlantis-authority-integration` (both repositories)
**Status: NOT ACCEPTED. NOT MERGED.** Recorded for checkpoint review.
**Date:** 2026-07-26

Evidence for the correction pass ordered after the
`atlantis-authority-integration` checkpoint. Sections refer to that order.

## -2. Adversarial audit pass (2026-07-27)

Four independent adversarial audits were run over the whole codebase
(memory/resource/concurrency, authority-vs-contract, test validity, security
boundary). They found real defects, several of them introduced by the
correction passes recorded below. Fixed here and verified on hardware:

| # | Defect | Severity | Origin |
|---|---|---|---|
| 1 | `commit_locked()` in `argus_security_directory.c` assigned a whole directory slot via a compound literal, materialising a **~4.4 KB stack temporary** in a function reached from `app_main` where the main task stack is 3584 B. Would smash the stack on **first boot of a factory-fresh or post-factory-reset unit** — the only case where both directory slots are absent and that path runs. The commissioned bench unit never reaches it. | **CRITICAL** | Pre-existing |
| 2 | A2.6 cache key omitted the request KIND, so an acquisition and a byte-identical release sharing a `request_id` collided: the **release was silently discarded and answered `ACCEPTED`/`already_held`** from the acquisition's cached entry, leaving authority held. | HIGH | This pass |
| 3 | `session_mismatch` rejections were cached under the SENDER's session — permanently unfindable, pure eviction pressure. Any authenticated principal could **flush the entire replay table on demand**, destroying QoS-1 idempotency. Also violated A2.7's zero-mutation requirement. | HIGH | This pass |
| 4 | The `ALREADY_HELD` rebind path reset `heartbeat_counter` unconditionally, including when the connection had NOT changed — **disarming the heartbeat replay guard** for that connection's own history. | HIGH | This pass |
| 5 | `publish_authority_state()` took the mutex twice and published the two observations as one, so a lease expiring between them published **`control_owner=LOCAL_HMI` alongside `core_lease_status=EXPIRED`** — contradicting A2.10's internal-consistency requirement. | MEDIUM | Pre-existing |
| 6 | `TRANSPORT_LOST` overwrote `CORE_LEASE_EXPIRED` when an already-expired supervisor's socket later dropped, **attributing an expiry to a transport event**. | MEDIUM | This pass |
| 7 | `httpd_resp_set_hdr()` stores the pointer, not the value; the `Retry-After` buffer was block-scoped and its storage was reclaimed before serialisation — **HTTP-task stack disclosure** in a 429, reachable by any AP peer after five failed logins. | MEDIUM | Pre-existing |
| 8 | `argus_mqtt_runtime_reset_duplicate_cache()` ran **outside the mutex** that serialises every other access to that table, from a different task than the one that reads it. | MEDIUM | This pass |

**The most important finding is not in that table.** The audit established
that **A2.9 epoch enforcement on operational commands was provably untested**:
`ARGUS_MQTT_COMMAND_ADMIT_STALE_EPOCH` and `..._SESSION_MISMATCH` were
asserted by no test in the suite, because every caller either passed the
core's own epoch as the command's epoch (so it could never mismatch) or
tripped an earlier gate first. **Deleting the A2.9 check left 993/993
passing.** That is the precise defect this entire correction effort exists to
eliminate, sitting in the gate correction-order §4 was written to enforce and
in the seam this pass extracted. Four new tests (`test_4c_audit_*`) now drive
that gate and defects 2-4 directly.

Also corrected: `test_4c_fail_operational_epoch_survives_reconnect_blip`
never granted authority, never called `argus_mqtt_session_disconnect()`, and
never referenced `authority_epoch` — the identical flaw the record below says
got its predecessor replaced. The replacement landed under a new name and
this one was left in place with the flaw intact. It now acquires authority,
drops the transport through the real disconnect path, and asserts the epoch
across the blip and its advance on expiry.

A `-Wframe-larger-than=2048 -Werror` guard existed on
`argus_machine_directory.c` — the module that was already correct — and not
on `argus_security_directory.c`, which held the 4.4 KB frame. The guard now
covers both plus `argus_security_store.c` and `argus_mqtt_runtime.c`, so this
class cannot regress silently.

### -2.1 Verification — HMI connected throughout

```
Heap before tests: free=32932 largest_block=21504 bytes
Heap after  tests: free=33568 largest_block=23552 bytes

Distinct Tests : 335   Repeat Passes : 3
Total Executed : 1005  Passed : 1005  Failed : 0

Production Isolation (Read-Only Proof):
  Authority Generation : UNCHANGED (Gen 3)
  Network State        : UNCHANGED (AP_DISCOVERABLE)
  MQTT Broker State    : UNCHANGED (RUNNING)
  Machine State        : UNCHANGED (UNLOCKED)
  Task Count           : UNCHANGED (24 tasks)
  AP Stations          : STEADY (1 -> 1)
  Broker Clients       : STEADY (1 -> 1)

PHASE 4D.4 PURE UNIT TEST SUITE: PASSED
Diagnostic task stack high-water: before=14116 after=4244 bytes
```

`Broker Clients: STEADY (1 -> 1)` — run under the hard heap condition, not
around it. Zero-warning build including the newly-extended frame guards.

### -2.2 Reported, NOT fixed — needs Shawn's decision

These are real findings from the same audits, deliberately left alone
because each is either a policy/contract decision or carries risk I cannot
verify without hardware I do not have:

- **Unauthenticated MQTT CONNECT flood denies all authentication.** Every
  CONNECT reaches the PBKDF2 worker (correct anti-enumeration design), the
  worker is serialised with a depth-1 queue, and the 8 throttle buckets are
  keyed on source IP with LRU eviction that clears a *blocked* bucket first.
  An attacker on the plant network keeping 2 derivations in flight denies
  browser login (503) and all machine auth indefinitely. Motion is
  unaffected. **HIGH** — needs a per-identity bucket that is not evictable
  and/or a pre-KDF admission bound.
- **Pre-CONNECT client-slot exhaustion.** Slot + 8 KB task are committed at
  `accept()`, 10 slots, no per-source limit, 30 s grace. Ten idle sockets
  deny MQTT to the real HMI. **MEDIUM.**
- **AP-vs-STA decided by IP identity, and ambiguity resolves to the MORE
  privileged answer.** Both `socket_is_softap()` and
  `argus_mqtt_receiving_interface()` compare against the AP's
  192.168.4.1/24; a rogue DHCP server that places the STA interface in that
  subnet makes AP-only browser routes and SOFTAP-only machine records
  reachable from the plant network. Structural defect verified; exploit
  precondition inferred. **MEDIUM.**
- **The broker advertises 10 client slots (81,920 B of task stack) the heap
  cannot fund.** Degrades gracefully but surfaces as unrelated allocation
  failures — which is exactly the class of symptom that produced the 4D.4
  failures. `s_broker` is 54,536 B of `.bss`, 32,000 B of it subscription
  slots (20/client × 160 B) against a contract defining 43 fixed topics.
  **HIGH as a budget decision.**
- **Test objects are in the shipping image.** ~9 KB of `.bss` and the
  diagnostic task's 16 KB stack exist to serve test fixtures; the ten
  deepest frames in the image are all test functions. `CONFIG_ARGUS_DIAGNOSTIC_MODE`
  is not applied at the build-system level, so the test translation units
  compile unconditionally.
- **A2.6 contract text says the cache holds `2 × MAX_MACHINES` (32).** The
  code now holds 16 for heap reasons, documented in-source. Contract and
  code disagree — one must change.
- **A2.2's 512-byte request budget does not exist.** The broker rejects at
  `ARGUS_MQTT_BROKER_PAYLOAD_CAP` = 385 before any callback, so the
  effective ceiling is 384 and `payload_too_large` never occurs on the wire.
- **`retain_forbidden` is unreachable and `qos_1_required` publishes
  nothing.** Broker policy rejects retained messages before the handler, so
  that branch is dead code; a QoS-0 authority request is logged but produces
  no `authority_result`, leaving a client unable to diagnose it.
- **`denied_by_profile` misdirects during the ArgusCore acquisition window.**
  The profile does not forbid the HMI there — it is the fallback once the
  window closes — so the operator is told to recommission when the answer is
  "wait up to 45 s". A2.5 has no reason string for this state.
- **A2.4 vs A2.6 conflict on replayed results.** A2.4 says the result lets a
  requester determine the CURRENT owner/epoch; A2.6 mandates replaying the
  CACHED result. The implementation follows A2.6. The contract should say
  which is authoritative on a replay.
- **A misattributed quotation** appears in source comments and in this
  record: "published ownership must remain truthful even while the link is
  offline" is a faithful paraphrase of A2.11's substance but is not text
  that exists in A2.11.

## -1. Final focused correction pass (2026-07-27)

A second independent review of the state at `1d36829` ordered six further
bounded corrections. All six are implemented; the on-target suite run is
recorded in §-1.1 below once it exists, per this document's practice.

**Item 1 — acquisition-side epoch semantics.** The stale-epoch admission
check existed for releases only; an acquisition carrying a nonzero,
since-superseded epoch was arbitrated as if current. Now: nonzero mismatch
is rejected `stale_epoch` before arbitration
(`ARGUS_MQTT_AUTHORITY_ADMIT_REQUEST_STALE_EPOCH` in
`argus_mqtt_authority_admit()`), zero mutation, result through the
production `event/core/authority_result` path. A2.2 revised to match.

**Item 2 — disconnect/rebind coherence.** Two real defects fixed in the pure
core's same-principal reacquisition branch: it left `link` OFFLINE (so a
legitimately rebinding owner's next operational command was refused
`not_authority_holder` — precisely the outcome transport/lease separation
exists to prevent) and left the heartbeat binding on the dead connection.
Both now restored to the new connection. The operational-command admission
rule was extracted from `handle_command()` into
`argus_mqtt_command_admission_check()` (pure, exported) so the
disconnect → rebind → command sequence is proven against the production
gate. Authority reasons: `TRANSPORT_LOST` (defined in A2.10 since A2 but
never set by any code path) now published on owning-transport disconnect;
`CORE_REACQUISITION` on same-principal rebind reached while the link was
not already online, from both the explicit-request and heartbeat entry
points; reason decisions extracted into exported pure functions
(`argus_mqtt_authority_reason_for_*`) tested directly.

**Item 3 — policy ordering and release clarification.** Commissioning
profile now precedes the running-transfer rule: on `STANDALONE_HMI`, an
ArgusCore request is `denied_by_profile` in every machine state — previously
a running pump would have answered `transfer_unsupported_running`,
misdirecting the operator toward waiting for a stop that changes nothing.
A2.3 revised: release admission is exactly owner + session + epoch, no
machine state blocks a release, `denied_machine_state` is explicitly
reserved-and-unused (this resolves the prior pass's §0 item 4, which
reported it as needing Shawn's decision — the review's decision is that no
trapping state exists).

**Item 4 — bounded replay semantics.** The 8-entry cache's "operator-paced"
sizing rationale is withdrawn as insufficient. The cache is now
`2 × ARGUS_SECURITY_MAX_MACHINES` (32) entries — a bound derived from broker
admission rules (one live connection per enrolled machine, fixed enrollment
ceiling), not traffic estimates — with deterministic FIFO rollover and a
monotonic admission ordinal for diagnostics. More importantly, safety no
longer depends on cache residency at all: a TRANSFER (displacing a
different current owner) must carry the current nonzero epoch
(`ARGUS_MQTT_AUTHORITY_TRANSFER_EPOCH_REQUIRED`, wire reason `stale_epoch`);
epoch-0 transfers are refused. An evicted old acquisition redelivered after
ownership changed is therefore structurally unable to preempt, renew,
rebind, advance the epoch, or move the lease deadline — proven by
`test_4c_ffp_evicted_replay_cannot_mutate` through real eviction. A
duplicate-conflict no longer stores a second record for the same identity
(the original stays canonical). A2.6 revised with the full argument,
including the honest limitation: an opaque request_id cannot support a
literal outside-window recognition, and safety is carried by admission
instead.

**Item 5 — HMI bounded receive, behaviorally tested.** The receive rules
moved verbatim from `hmi_portal.c`'s `provision_post()` into
`common/hmi_body_reader.c` (injectable recv/clock seams; production passes
thin wrappers over `httpd_req_recv`/`esp_timer_get_time`). 12 host-test
scenarios cover: one-read completion, fragmented completion, timeout-retry
then success, total-deadline expiry with partial body (wiped), truncation,
early peer close, negative socket error, declared-oversized refusal with
zero reads, exact-boundary lengths, a recv over-claiming bytes (refused),
and bad-arguments never touching the transport. Host suite: 1194/1194.
Evidence boundary stated plainly: this is host/pure evidence of the
production rules plus a zero-warning HMI target build; no real HTTP client
and no authenticated authority path was exercised (deferred to #67).

**Item 6 — diagnostic stack and documentation.**
`ARGUS_DIAGNOSTIC_TASK_STACK` raised 12288 → 16384 after the measured
1028–1036 bytes free; verified on target, not assumed (see §-1.1).
`MQTT_STANDARDS.md` in BOTH repos corrected: authority is no longer
described as a connection-created/heartbeat-created lease; heartbeat is
lease maintenance only. A2.2/A2.3/A2.6/A2.8 revised as above; HMI frozen
snapshot re-refreshed with provenance; this record updated.

### -1.1 Hardware evidence — three runs, one real regression found and fixed

#### First run (commit `c43784d`, run by Shawn): 17 failures — a real regression

```
Distinct Tests : 331   Repeat Passes : 3
Total Executed : 993   Passed : 976   Failed : 17
PHASE 4D.4 PURE UNIT TEST SUITE: FAILED
Diagnostic task stack high-water: before=14120 after=4456 bytes
```

Six `test_4d4_machine_directory_*` tests failed (one of them on only 2 of 3
passes — the signature of a resource margin, not a logic error). Every
failing assertion was a `CHECK(... != NULL)` on a multi-kilobyte `calloc`;
the one machine-directory test that allocates nothing passed. Root cause:
this pass's 32-entry duplicate cache stored full 513-byte payloads —
`.bss.s_auth_dup_cache = 0x7900` (30,976 B), boot heap 176 → 153 KiB — and
that consumed exactly the heap margin those tests' allocations lived in.
**A regression introduced by this correction pass, found by the suite on
hardware.** The isolation-proof MUTATED/CHANGED entries in this run
(task 23→24, broker clients 0→1) were the HMI reconnecting mid-run, as in
§2.3.1 — outside activity, not test mutation.

#### Fix: payload identity by digest, not by copy

The cache now stores `(payload_len, SHA-256)` instead of the payload bytes.
The A2.6 semantic is unchanged — identical repeat replays, conflicting reuse
refused — and length-plus-SHA-256 is byte-identity for every purpose this
comparison serves (it disambiguates replay-vs-conflict among authenticated,
capability-checked principals; same-length collision construction is not in
this threat model). Entry ~968 → ~490 bytes;
`.bss.s_auth_dup_cache = 0x3d00` (15,616 B); boot heap recovered to
168 KiB. Zero-warning build.

#### Console made deterministically scriptable (Shawn's direct instruction)

Investigated why scripted serial input never reached the menu while a human
terminal worked: every `linenoise()` prompt emits `ESC[6n` (twice) and then
blocks reading the terminal's cursor-position reply via the USB-Serial/JTAG
VFS with `portMAX_DELAY` — forever, no timeout. A human terminal
auto-replies; a scripted client hangs the prompt and has its keystrokes
silently consumed as reply bytes. Fixed in
`argus_console_transport_init()`: `linenoiseSetDumbMode(1)` unconditionally
— no escape queries ever emitted, no reply parsing on the input path,
identical behavior for an operator terminal and a scripted acceptance run.
(IDF's own REPL uses probe-then-dumb; unconditional was chosen because the
probe's outcome depends on whether a terminal is attached during a 500 ms
boot window — nondeterministic console behavior on a field device, and this
menu takes one character per line so line editing buys nothing.) The suite
still runs only on an explicit `t`; no boot-time automatic test path was
restored. OPEN OBSERVATION, recorded honestly: during earlier scripted
probing, sending a cursor-position reply at an unexpected time crashed the
firmware once (LoadProhibited, EXCVADDR `0xa5a5a5e9` — the FreeRTOS
stack-fill pattern, i.e. a load through never-written stack memory).
Source inspection of linenoise's reply parser found it bounded and
initialized throughout, so the defect is likely elsewhere in the console
path and remains un-root-caused. Dumb mode makes the triggering path
unreachable; the observation stands as a finding, not a fix.

#### INVALID VERIFICATION — recorded because it is the instructive part

The run below was reported at the time as proof the heap fix worked. **It
was not proof.** Its isolation block reads `Broker Clients: CHANGED (0 -> 1)`
— the HMI was NOT connected when the suite started. Shawn's failing run had
`1 -> 0`: the HMI *was* connected. Each broker client is an 8 KB heap task
stack, so the run below executed under ~13 KB more free heap than the run it
claimed to vindicate. Shawn re-ran the suite three times on the same build
and got the same 17 failures, which is what exposed this.

Two compounding causes, both mine:
1. **The condition was never controlled.** Opening the serial port resets the
   chip, so a scripted run always begins on a fresh boot ~25 s before the HMI
   reconnects. Every scripted run therefore silently selected the EASY heap
   condition. The harness now takes a `hmi` argument that blocks until
   `authenticated machine connected` appears before sending `t`, and exits
   non-zero if it never does — a run that cannot reach the hard condition now
   fails loudly instead of passing quietly.
2. **The heap was never measured.** Nothing in the transcript distinguished
   "logic correct" from "allocation happened to fit", so a passing run and a
   lucky run were indistinguishable. The suite now prints free heap and
   largest free block before and after.

This is the same failure this entire correction pass exists to correct — a
green result that proved nothing because the conditions differed — committed
by the person writing the corrections, one section after documenting it.

#### Second fix: the first fix was insufficient, not just unverified

Hashing the payload took the entry from ~968 to ~490 bytes, but the table was
still 32 entries (15,616 B) against a baseline of 8 entries (7,680 B), and
the diagnostic stack was still +4 KB. Under the HMI-connected condition that
left roughly 8 KB of contiguous heap against a ~13 KB requirement — short,
exactly as observed. The real fix removes the rest:

- The result is stored as the **fields** it is composed from, not the
  composed 385-byte JSON. `compose_authority_result_json()` was split out as
  a pure function of exactly those fields, so a replay recomposes
  byte-identical output. Storing fields is not a summary of the result; it is
  the result's complete input set.
- The table is `ARGUS_SECURITY_MAX_MACHINES` (16), not 2×. The 2× was
  speculative padding for "a request and its follow-up", not a protocol
  property — and a machine's follow-up is only sent after its first is
  answered. One slot per machine that can have a request in flight is the
  honest bound.

Entry ~968 → ~152 B; table `.bss` 30,976 → 15,616 → **2,368 B**, now
*smaller than the original 8-entry table it replaced*. Net against the
baseline that passed: −5,312 B of `.bss` against +4,096 B of diagnostic
stack, i.e. this pass now returns about 1.2 KB more heap than it consumes.

#### Final run — HMI CONNECTED throughout, the condition that failed

```
Heap before tests: free=32912 largest_block=21504 bytes
Heap after  tests: free=33544 largest_block=23552 bytes

Distinct Tests : 331   Repeat Passes : 3
Total Executed : 993   Passed : 993   Failed : 0

Production Isolation (Read-Only Proof):
  Authority Generation : UNCHANGED (Gen 3)
  Network State        : UNCHANGED (AP_DISCOVERABLE)
  MQTT Broker State    : UNCHANGED (RUNNING)
  Machine State        : UNCHANGED (UNLOCKED)
  Task Count           : UNCHANGED (24 tasks)
  AP Stations          : STEADY (1 -> 1)
  Broker Clients       : STEADY (1 -> 1)

PHASE 4D.4 PURE UNIT TEST SUITE: PASSED
Diagnostic task stack high-water: before=14116 after=4100 bytes
```

`Broker Clients: STEADY (1 -> 1)` and `Task Count: UNCHANGED (24)` are the
load-bearing lines: the HMI held its broker connection for the whole run, so
this executed under Shawn's failing condition rather than around it. Every
isolation field is clean — the first run in this pass for which that is true.

The margin is now measured, not inferred: 21,504 B largest contiguous block
at suite start against a ~13 KB peak requirement (the directory tests
allocate two ~6.5 KB slots simultaneously; measured suite-wide peak
consumption is ~12.7 KB). For comparison, the same measurement under the
HMI-absent condition reads 31,744 B — the ~10 KB delta between them is the
margin every previous "verification" in this pass was silently spending.

#### Superseded run (HMI absent — retained for the record)

```
Distinct Tests : 331   Repeat Passes : 3
Total Executed : 993   Passed : 993   Failed : 0

Production Isolation (Read-Only Proof):
  Authority Generation : UNCHANGED (Gen 3)
  Network State         : UNCHANGED (AP_DISCOVERABLE)
  MQTT Broker State     : MUTATED (RUNNING)      <- HMI reconnect mid-run,
  Task Count            : MUTATED (24 tasks)        attributed by the suite's
  Broker Clients        : CHANGED (0 -> 1)          own outside-activity note
  Machine State         : UNCHANGED (UNLOCKED)
  AP Stations           : STEADY (1 -> 1)

PHASE 4D.4 PURE UNIT TEST SUITE: PASSED
Diagnostic task stack high-water: before=14124 after=4268 bytes
```

All six previously-failing machine-directory tests pass; all nine
`test_4c_ffp_*` tests pass on hardware. **The stack raise is vindicated by
measurement, not assumption:** worst-case consumption is ~12.1 KB
(16,384 − 4,268), which sits above the previous 12,288-byte allocation —
the old stack was at or past its edge, and the pre-raise 1028-byte
high-water was the same fact seen from the other side. Boot path with the
final build: heap 168 KiB main region, Wi-Fi sta+softAP, DHCP, HTTP,
broker, STA association all clean; 4× `Hash of data verified` on flash;
COM5 identity `9&26DB8A4B` verified before every flash. No motor connected,
none energized, no motion command issued at any point.

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

### 2.3 Independent-review correction order — on hardware, COM5

#### 2.3.1 First run (commit `215eeae`): 5 failures found

```
Distinct Tests : 322
Repeat Passes  : 3
Total Executed : 966
Total Passed   : 951
Total Failed   : 15

Production Isolation (Read-Only Proof):
  Authority Generation : UNCHANGED (Gen 4)
  Network State         : UNCHANGED (AP_DISCOVERABLE)
  MQTT Broker State     : MUTATED (RUNNING)
  Machine State         : UNCHANGED (UNLOCKED)
  Task Count            : MUTATED (24 tasks)
  AP Stations           : STEADY (1 -> 1)
  Broker Clients        : CHANGED (0 -> 1)

PHASE 4D.4 PURE UNIT TEST SUITE: FAILED
```

Diagnostic task stack high-water: before=9988, after=1028 bytes free of
12 KB — see the watch item in §5, materially worse than previously reported.

5 of 322 distinct tests failed, consistently across all three repeat passes
(Shawn ran the suite twice; both runs failed identically):
`test_4c_seam_decode_rejects_unknown_and_duplicate_fields`,
`test_4c_seam_decode_rejects_missing_fields`,
`test_4c_seam_decode_bounds_and_arguments`,
`test_4c_seam_release_requires_named_epoch`,
`test_4c_seam_authority_result_topic_and_schema`.

**All five were test-fixture bugs introduced earlier in this same correction
pass, not new production defects.** Root causes, fixed in `15b59d5`:

- Three fixtures used placeholder `intent` strings (`"a"`/`"b"`, `"take"`,
  `"handover"`) written before this pass added A2.2 intent-enum validation.
  Each failed at the intent field before ever reaching what the test meant
  to exercise. Fixed with valid enum members where only key repetition or
  field presence mattered to the test, not the value.
- An over-long `request_id` now correctly decodes to the new
  `ARGUS_MQTT_DECODE_INVALID_REQUEST_ID` (added by this same pass, §0 item
  9), not the generic `INVALID_VALUE` one test still asserted — missed when
  the other request_id-reason call sites were updated.
- `test_4c_seam_authority_result_topic_and_schema` reused the same
  `request_id` for two logically separate requests (a panel's grant, then a
  different principal's grant meant to prove `SERVICE_TOOL` is reported
  distinctly). With the real bounded A2.6 duplicate cache now live, the
  second call was an exact-payload replay of the first's cached
  `LOCAL_HMI` result, not a fresh evaluation — and silently passed the
  result-code check only because `ARGUS_MQTT_AUTHORITY_GRANTED` is enum
  value 0, matching the outcome struct's zero-initialization. Only the
  JSON-content assertion (still `LOCAL_HMI`, not `SERVICE_TOOL`) caught it.
  The same request-ID-collision pattern had already been fixed in three
  other tests earlier in this pass; this one was missed.

The `MUTATED`/`CHANGED` isolation-proof deltas (task count 23→24, broker
clients 0→1) are **not a regression**: `argus_mqtt_broker.c` spawns one
FreeRTOS task per accepted client connection
(`argus_mqtt_client_task`) — the HMI reconnecting to the live broker on a
real networked bench device during the suite run, unrelated to the pure
unit tests, which never touch the real broker or its sockets.

#### 2.3.2 Second run (commit `15b59d5`): clean

```
Distinct Tests : 322
Repeat Passes  : 3
Total Executed : 966
Total Passed   : 966
Total Failed   : 0

Production Isolation (Read-Only Proof):
  Authority Generation : UNCHANGED (Gen 4)
  Network State         : UNCHANGED (AP_DISCOVERABLE)
  MQTT Broker State     : UNCHANGED (RUNNING)
  Machine State         : UNCHANGED (UNLOCKED)
  Task Count            : UNCHANGED (24 tasks)
  AP Stations           : STEADY (1 -> 1)
  Broker Clients        : STEADY (1 -> 1)

PHASE 4D.4 PURE UNIT TEST SUITE: PASSED
```

322 distinct tests (up from 313 before this correction order — 9 net new:
24 seam tests added in §0 minus 3 decode-laxness tests replaced by 3
strict-rejection tests, per that section's own accounting). 966/966
executions passed, 0 failed. Isolation proof fully clean — broker clients
now steady at 1→1 rather than 0→1, consistent with the HMI's connection
already being established before this second run started rather than
arriving mid-run.

Diagnostic task stack high-water: before=10028, after=1036 bytes free of
12 KB. Consistent with the first run's 1028 (within run-to-run noise);
**not improving.** See §5.

Both runs flashed to COM5 (device identity `9&26DB8A4B` confirmed each
time), 4× `Hash of data verified`, hard reset. Read-only serial capture
between runs confirmed clean boot and diagnostic menu reachable. Both suite
runs executed by Shawn at the console.

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

## 5. Watch item — now closer to a real problem than a watch item

Diagnostic task stack high-water (the `after` figure — the worst-case
minimum free stack actually reached during the run — is the one that
matters for overflow risk):

3904 → 2652 → 1988 → **1028** → **1036** bytes free of a 12 KB stack.

The last two are both from the independent-review correction order
(§2.3.1's failing run and §2.3.2's clean run), 8 bytes apart — consistent
with each other, not noise, and not improving. The `before` figures reported
alongside them (9988, 10028) are a different measurement (apparently
captured near task start rather than a prior run's watermark) and are not
directly comparable to the historical trend above; the `after` figures are.

At 1028–1036 bytes free of 12288 (roughly 8%), this is no longer speculative
headroom-watching. `ARGUS_DIAGNOSTIC_TASK_STACK` was flagged as due for a
raise before the independent-review correction order added ~22 more test
functions to this suite (313 → 322 net, but several of the new tests carry
more local state — `argus_mqtt_broker_message_t`, `argus_mqtt_session_core_t`,
`argus_mqtt_authority_outcome_t` — than the tests they sit alongside). It
should be raised before anything else is added to this suite, and arguably
before continuing to treat it as merely "recommended."

**RESOLVED by the final focused pass (§-1 item 6):**
`ARGUS_DIAGNOSTIC_TASK_STACK` raised 12288 → 16384 in `app_main.c`, chosen
so the measured worst case (~11.3 KB consumed) keeps roughly 5 KB of
headroom even as this pass adds nine more test functions. The post-raise
high-water and the boot-path memory evidence (Wi-Fi/network/AP/broker/HTTP
startup unimpaired by the extra 4 KB allocation) are recorded in §-1.1 from
the actual hardware run, not assumed.
