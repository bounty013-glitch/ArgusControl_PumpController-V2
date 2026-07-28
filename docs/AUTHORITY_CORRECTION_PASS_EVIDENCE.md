# Authority Correction Pass — Evidence Record

**Branch:** `atlantis-authority-integration` (both repositories)
**Status: ACCEPTED FOR MERGE BY SHAWN.**
**Acceptance date:** 2026-07-27

Shawn accepted this checkpoint and authorized its merge into `main`. The
three-simultaneous-authenticated-client/fourth-refusal hardware test and the
true multi-source pending-socket exhaustion test are explicitly deferred,
documented evidence gaps and are **non-blocking for this acceptance**. Their
production decision seams and resource invariants are tested; their exact
multi-client/multi-host hardware demonstrations remain future verification.
Physical motor testing remains deferred and is not evidence produced by this
checkpoint.

Evidence for the correction pass ordered after the
`atlantis-authority-integration` checkpoint. Sections refer to that order.

Sections are newest-first. The header above describes the accepted
`atlantis-authority-integration` checkpoint; work from §-5 onward continues on
`atlantis-hmi-authority-controls` and is **not** covered by that acceptance.

## -5. Stage 1 bench acceptance — first powered run with the panel in control (2026-07-28)

The first time the rotary panel has held command authority against the real
controller with the operator driving it. Motor **not** connected; no motion
was produced. Controller on COM5, panel on COM18, both flashed from the
branch heads recorded at the end of this section.

Runbook: `docs/plan/STAGE_1_ACCEPTANCE_RUNBOOK.md` in the HMI repository.

### Outcome — §1 through §3 PASS, §4 non-blocking by Shawn's call

Shawn executed §1 (grant control capabilities to the enrolled panel identity),
§2 (confirm the in-place upgrade) and §3 including §3.1 and §3.2. His report:
the capability grant, the upgrade confirmation, the real command, and the
refusal path all behaved as the runbook predicted; commands reported
`accepted` and the machine acted on them; and the ramp behaviour read as the
real controller rather than the simulator.

That last observation is the one worth keeping. The panel is now driving the
controller's own ramp logic rather than a local approximation, and it is
distinguishable by feel — which is the substantive difference Stage 1 was
written to produce.

**§4 — three simultaneous authenticated clients plus a fourth refusal — is
recorded NON-BLOCKING by Shawn's explicit determination**, not by my
judgement and not by silence. His stated rationale: the only two long-term
connections are the rotary HMI and ArgusCore; a third is a diagnostic
connection made by him alone during service; and he has no way to stage a
genuine four-client attempt at the bench right now. That population matches
the measured `MAX_CLIENTS = 3` with one slot spare. The seam and the resource
invariant are covered by host tests; the multi-host hardware demonstration
remains an open evidence gap, unchanged from §-4 and still honestly labelled.

### Serial capture found three defects the runbook could not

The runbook is written for an operator watching the glass. Serial capture
sees what the glass does not, and it found three things.

**CLOSED — the panel discarded the controller's operator-visible admission
conditions.** `status/core/network_fault`, `status/core/network_fault_action`
and `status/core/auth_throttle` were arriving and being logged as
`message on unexpected topic ... ignored`. Those three topics were added to
the controller in §-4 for exactly one purpose: to let an operator see a
network-configuration fault or an authentication throttle instead of guessing
at a panel that has quietly stopped working. The panel was receiving them and
throwing them away, which defeated the whole of that work. They are now
mirrored into `hmi_state`.

Presentation is deliberately **not** included. Where a network fault or an
auth throttle appears on the glass, and what it displaces, is an operator-
facing decision. Mirroring the data is mechanical; deciding what the operator
sees is not, and inventing it silently is how a panel ends up lying. This is
a named, deliberate gap, not an oversight.

**CLOSED — two further false "unexpected topic" reports.**
`event/core/authority_result` was reported as unexpected on every acquisition
answer — the panel's own authority replies. And the panel's lease heartbeat
was reported as unexpected because the panel subscribes to `status/#` and so
receives its own publication back. Neither is unknown and neither is ignored.
A wrong log is worse than no log: the next person debugging a failed
acquisition would have chased twenty phantom warnings.

Measured on hardware across the pass: unexpected-topic reports **26 → 20 → 0**.

**OPEN — acquisition sometimes loses a granted lease. NOT FIXED.**

Observed in the first capture: the panel requested authority, the controller
granted it, ownership never appeared in retained state, the panel's confirm
window expired and it asked again. Acquisition took roughly thirty seconds
and two requests where it should take about two seconds and one.

```
ACQUIRE-EVIDENCE: state=4 requests[sent=2 granted=2 refused=0 unanswered=1]
```

The cause identified was that the panel transmitted nothing at all while in
`HMI_ACQUIRE_CONFIRMING`, and the controller expires an unrenewed lease after
six seconds — inside the five-second confirm window. Waiting for ownership
destroyed the grant being waited for. That is a real defect and it is fixed:
`CONFIRMING` now renews, immediately on entry and then on cadence, with host
tests pinning both that it renews and that a panel holding nothing still
never heartbeats.

**It is an improvement, not a closure, and it is not recorded as fixed.** One
subsequent hardware run acquired cleanly:

```
ACQUIRE-EVIDENCE: state=4 requests[sent=1 granted=1 refused=0 unanswered=0] heartbeats=15
```

The next run reproduced the original failure — `sent=2 granted=2 unanswered=1`
— with the fix present and flashed. So the silence during `CONFIRMING` was
necessary to fix and was not sufficient to explain the behaviour. The actual
cause is not established, and any statement about it now would be a guess.

The blocker is measurement resolution, not analysis. The panel prints
`ACQUIRE-EVIDENCE` every fifteen seconds; the sequence under investigation
unfolds in about five. **Next step: per-transition logging of the acquisition
state machine so the real ordering is visible before anything else is
changed.** No further fix should be attempted until that exists.

### The trace was built, and it moved the defect to the controller

Panel commit `8bc3d01`. Every acquisition transition now logs the rule that
caused it and the controller's published authority at that instant, plus an
`AUTHORITY-DELTA` line whenever the authority tuple changes rather than on a
timer. Host suite 3498 → 3690 checks, all passing. First hardware run
answered the question, and **not in favour of the hypothesis above**:

```
#1  t=29916ms IDLE->REQUESTED    cause=REQUEST_SENT  owner=NONE local=AVAILABLE lease=EXPIRED epoch=6 heartbeats=0
#2  t=30013ms REQUESTED->CONFIRMING cause=GRANTED    owner=NONE local=AVAILABLE lease=EXPIRED epoch=6 heartbeats=0
#3  t=35052ms CONFIRMING->BACKOFF cause=CONFIRM_TIMEOUT owner=NONE local=AVAILABLE lease=EXPIRED epoch=6 heartbeats=3
#4  t=45126ms BACKOFF->IDLE      cause=BACKOFF_ELAPSED  ... epoch=6 heartbeats=3
#5  t=45126ms IDLE->REQUESTED    cause=REQUEST_SENT     ... epoch=6 heartbeats=3
#6  t=45297ms REQUESTED->CONFIRMING cause=GRANTED       ... epoch=6 heartbeats=3
#7  t=50330ms CONFIRMING->BACKOFF cause=CONFIRM_TIMEOUT ... epoch=6 heartbeats=6
   ... third identical cycle ...
#14 t=75633ms REQUESTED->CONFIRMING cause=GRANTED    owner=LOCAL_HMI local=ACTIVE lease=ACTIVE epoch=7 heartbeats=9
#15 t=75633ms CONFIRMING->HOLDING cause=OWNERSHIP_OBSERVED owner=LOCAL_HMI local=ACTIVE lease=ACTIVE epoch=7
```

**The controller answered ACCEPTED three times without its published
authority changing at all.** `owner=NONE local=AVAILABLE lease=EXPIRED
epoch=6` held constant for forty-six seconds across three grants.
`AUTHORITY-DELTA` fired exactly twice in the whole window, so this is an
absence of publication, not a message the panel missed or mishandled.

**Panel silence during `CONFIRMING` is definitively excluded as the cause.**
The heartbeat counters advance 0 → 3 → 6 → 9, exactly three renewals per
five-second confirm window at the 2 s cadence. The panel was renewing
throughout and ownership still never appeared. The `CONFIRMING` fix was
correct on its own terms — a granted lease must be renewed — but it was never
the explanation, which is precisely why it was necessary and not sufficient.

The fourth attempt succeeded with the grant and `epoch=7` arriving in the
same millisecond, and `HOLDING` followed in that same tick.

**The defect is controller-side. The panel behaved correctly on every
attempt**, including the three that failed. Two candidates remain that
panel-side data cannot separate:

1. The controller returns `ACCEPTED` on `event/core/authority_result` without
   committing or publishing the corresponding authority state.
2. It commits the grant and revokes it before publishing anything — for
   instance by rejecting the panel's renewals, which the panel cannot
   observe, since its counter records transmissions and not acceptances.

Distinguishing these requires the controller's own log, which needs a capture
on COM5. **Opening that port resets the controller** — that is how the
distortion recorded further down happened — so it is a deliberate action to
take at a chosen moment, not a casual probe.

Health of the run itself: 0 unexpected topics, 0 panics, 0 watchdogs, 0
`NO_MEM`, ending `HOLDING` at `epoch=7` with `lease=ACTIVE` and heartbeats
climbing.

**Still OPEN, and now correctly located.** Nothing here is a fix.

### Both-ends capture: root cause found — and it is panel-side, not controller-side

Shawn authorized the COM5 capture. Both ports were captured back-to-back
(sequential execution, provable from content below), and the result closes
the diagnosis completely. **The paragraph above locating the defect
controller-side was wrong**, in the same way the CONFIRMING hypothesis before
it was wrong: each pass correctly eliminated its predecessor and then
guessed. This one is not a guess — every link is verified in the capture and
in source.

**Two operational facts first, both corrections to earlier claims.**
A plain serial open did NOT reset the controller: its timestamps run
continuously through COM5 opening (~2.8 h uptime). The earlier mid-capture
reset must have come from the probe method, not the open. And the PANEL reset
between the two captures — proven below — almost certainly from the COM18
close/reopen toggling its auto-reset circuit. That accident was the perfect
experiment: the controller then watched a fresh panel boot run the entire
failing acquisition, from the other side.

**What the controller saw** (`both_controller.log`):

```
10064169  AP: panel STA fails 6 SA Query attempts → disassoc → rejoin ~700ms   (panel rebooting)
10067825  broker: client disconnected (m-b4ad…)
10068825  runtime: supervisor heartbeat stale; motion state intentionally unchanged
10080318  broker: panel re-authenticated, 5 subscriptions restored
10080678  request_authority #1: broker publish accepted — NO runtime decision logged — 3 heartbeats — silence
10095868  #2: identical.  10111086 #3.  10126292 #4.  10141577 #5.  (15.2 s apart)
10156860  request_authority #6 → 4 ms later:
          W argus_mqtt_runtime: authority request (type=1): granted (profile=0 window=0 running=0)
          → continuous 2 s heartbeats to end of capture
```

Five requests answered `ACCEPTED` on the wire with no runtime arbitration;
the sixth arbitrated and granted. The three-heartbeat bursts prove the panel
received `ACCEPTED` each time — it only enters CONFIRMING on a grant.

**Proof the panel had rebooted, and that the windows are disjoint:** the
heartbeat `payload_len` steps 42 → 43 at exactly the **10th** heartbeat after
reconnect — the counter crossing 9 → 10. A fresh provider counts from 1; the
panel's own capture ends at counter ≈ 280. The same numbers prove the
captures ran sequentially (panel first): one device cannot be at counter 280
and counter < 10 in the same window.

**The counting law that gives it away.** The panel's final pre-reboot
evidence read `sent=5 granted=5`. After reboot: exactly **5** phantom grants,
success on request **6**. Every observed instance of this defect obeys the
same law — N requests sent by the previous boot → N phantom grants after
reboot → success on N+1:

| Run | Previous boot had sent | Phantom grants | Succeeded on |
| --- | --- | --- | --- |
| First bench acceptance | 1 | 1 | #2 |
| Transition-trace run | 3 | 3 | #4 |
| Both-ends capture | 5 | 5 | #6 |

**The mechanism, verified in source end-to-end:**

1. The panel's request IDs are `hmi-auth-%010lu` of a counter that restarts
   at zero **every boot** — `hmi_command_codec.c:176`, driven by
   `p->request_counter` which `hmi_mqtt_provider_init()` zeroes.
2. The controller's A2.6 duplicate cache is keyed on
   `(session, request_id, kind)` — `argus_mqtt_runtime.c` `auth_dup_find()`.
   The session is the CONTROLLER's, and the controller did not restart, so
   the key collides across panel reboots.
3. The rebooted panel's payload is byte-identical to the previous boot's
   (same session string, same request ID, `epoch: 0`, same type), so the
   SHA-256 payload-identity check classifies it `DUPLICATE_REPLAY`, not
   `DUPLICATE_CONFLICT`.
4. The replay path — `argus_mqtt_runtime.c:1015` — republishes the CACHED
   result and returns: **no arbitration, no epoch change, no authority-state
   publication, no log line.** Exactly what the capture shows, and exactly
   what A2.6 specifies for a redelivered request.

**The controller is behaving correctly.** A2.6's replay semantics exist so a
QoS-1 redelivery cannot re-execute an authority change; the cache did
precisely its job. The defect is the panel presenting a GENUINELY NEW request
under the identity of an old one. `request_id` exists to name one request; a
boot-relative counter names a slot, and every panel reboot re-issues names
the previous boot already spent.

**Second instance of the same class, found while verifying: command
sequences.** The panel's `next_sequence` also restarts at 1 every boot
(`hmi_mqtt_provider_init()`, and on session change), while the controller's
`last_sequence` for the session persists. `argus_mqtt_session_check_sequence()`
(`argus_mqtt_contract.c:829`) classifies a not-newer sequence `STALE` and a
matching-sequence/different-payload `CONFLICT`. With `last_accepted_seq=3` on
the controller (visible in the capture) and `next_seq=1` on the freshly
booted panel (visible in its evidence line), the next THREE operator commands
after any panel reboot will be refused before the fourth is accepted. Latent
in this capture only because no command was attempted (`sent=0`). This is
bench-visible and Stage-1-relevant: the operator presses START and is
refused, three times, with no indication that time will fix it.

**Not explained, stated so:** between the trace run and this capture, in an
unobserved window, the panel lost and re-acquired authority once
(`sent` 4→5, epoch 7→9, `unanswered` 3→4). Request #5 was a fresh ID, so this
was NOT the replay defect. Consistent with a transient link stall expiring
the lease, and with fail-operational the pump kept its state — but the window
was unobserved and this is inference, not evidence.

**Fix directions, for decision — both panel-side, neither implemented:**

1. **Request IDs must be unique across boots within a controller session.**
   Salt the ID with per-boot entropy (e.g. `hmi-auth-<nonce>-<counter>`),
   supplied by the platform layer so the codec stays host-testable. No
   contract change: A2.2 constrains charset and length, not structure, and
   the controller treats the ID as opaque.
2. **Sequence adoption on session bind.** When binding to a session the
   controller already reports `last_accepted_sequence` for, start from
   `last_accepted_sequence + 1` rather than 1. This is compliance with the
   existing ordering contract, not a change to it.

These are one doctrine — *boot-relative identity may never be presented
against session-scoped state* — and should be decided together, which is why
neither was implemented under a capture authorization.

**The finding remains OPEN pending that decision.** The panel is currently
HOLDING (the controller granted request #6 during the capture); pump idle,
motor not connected, no motion commanded.

### Evidence classification

| Claim | Basis |
| --- | --- |
| §1–§3 behaved as written, commands accepted and acted on | Operator-observed at the bench by Shawn |
| Ramp is controller logic, not simulator | Operator-observed, qualitative |
| Zero unexpected-topic reports | Hardware serial capture, COM18 |
| No panic, watchdog, `NO_MEM` or bad payload | Hardware serial capture, COM18 |
| Lease held ACTIVE with heartbeats flowing | Hardware serial capture, COM18 |
| `CONFIRMING` renews; panel holding nothing never heartbeats | Host tests, seam-verified |
| Acquisition completes in one request | **Observed once, contradicted twice. NOT a standing claim.** |
| Controller grants without publishing authority state | Hardware transition trace, COM18 — three consecutive cycles |
| Panel renews throughout `CONFIRMING` | Hardware transition trace — heartbeats 0→3→6→9 |
| Three-client capacity, fourth refused | **Not demonstrated. Non-blocking per Shawn.** |

### Two things that distorted evidence in this pass

**I reset the controller mid-sequence.** Probing COM5 for availability
restarted it while a capture was running, which is why one later capture
shows `sent=2` against a controller that had just rebooted. The clean
single-request acquisition is the separate `s1_panel2.log` capture. Probing a
live device is not a free operation and I should not have done it during a
run.

**The flashed panel image predates three commits Shawn pushed during the
pass.** Two of them touch `main/hmi_mqtt_client.c/.h`, but the diff is
comments plus two log-message strings with no behavioural change, so the
capture remains valid. Worth noting because his header correction retires the
old claim that this client never publishes a heartbeat and only publishes
while holding authority — a claim the `CONFIRMING` renewal fix had made
untrue. His documentation now matches the built behaviour.

### State at the end of this section

Host suite **3690 checks, all passing** (3471 before this pass; 219 added).
Controller unchanged. Panel at `8bc3d01`, pushed. No merge to `main`, no tag,
no acceptance mark — Stage 1 is **not** accepted while the acquisition
finding is open, and it is now open against the controller rather than the
panel.

## -4. MQTT admission isolation and capacity proof (2026-07-27)

Shawn's final closure order, acting on independent review of `81fb3ed`. That
commit is preserved as the restoration point. Six items; all implemented,
both configurations built with zero warnings, **both flashed to COM5**, and
verified on hardware under load.

Read §-3 first: this section corrects claims made there.

### What §-3 got wrong, and how

**The pools were never separated.** §-3 said "unauthenticated sockets are now
bounded SEPARATELY from authenticated clients" and set
`MAX_PRECONNECT = 4` against `MAX_CLIENTS = 4`. Both counters indexed the
same `s_broker.clients[4]` array. Four silent sockets therefore filled every
physical record, and the rotary HMI could not reconnect at all — the exact
denial the change was written to prevent. The test that was supposed to cover
it asserted `MAX_PRECONNECT <= MAX_CLIENTS` and passed happily throughout,
because it encoded the fig leaf rather than the property. This is the same
failure mode as `epoch_survives_reconnect_blip`: a green test proving nothing.

**The client budget was arithmetic, not measurement.** §-3 recorded
"~13.0 KB per client" and declared `MAX_CLIENTS = 4` from it, and said so
plainly — but the dominant term, the 8192-byte client task stack, had never
been measured. It could as easily have been 3 KB (making 4 cheap) or 7.5 KB
(making 4 unsafe). It is now instrumented and measured.

**The production image had never been flashed.** §-3 proved its exclusions
from the link map and said explicitly that it was not flashed. Flashing it in
this pass immediately boot-looped — see the finding below.

### 1. Physically separate pending and authenticated pools

`s_broker.clients[]` is now sized `MAX_CLIENTS + MAX_PRECONNECT` and the
subscription table moved out of the connection record into a separate
`sessions[MAX_CLIENTS]` array. **Owning a session record IS the authenticated
capacity**: there are exactly `MAX_CLIENTS` of them, a record cannot become
`connected` without claiming one, and subscriptions can only live in one.

The invariant that makes this physical rather than clerical:

```
pending        <  MAX_PRECONNECT      (checked before a record is taken)
authenticated  <= MAX_CLIENTS         (session-slot ownership)
=> in_use      <= MAX_CONNECTIONS - 1
```

so an admitted arrival always has a free record, and a pending socket that
completes CONNECT always finds authenticated capacity when fewer than
`MAX_CLIENTS` sessions exist — whatever the other pool is doing.
`test_4d4_pending_pool_cannot_starve_authenticated` drives
`argus_mqtt_broker_preconnect_decide()` — the function production calls, not
a transcription — across every reachable `(pending, authenticated)` pair and
asserts the invariant at each.

**No reserved reconnect slot is claimed any more.** §-3's comment promised
"one reconnect overlapping its own stale slot"; nothing reserved anything.
A stale slot is recovered by keep-alive reaping, and the contract now says so.

Final limits: `MAX_CLIENTS = 3`, `MAX_PRECONNECT = 3`, unproven share 2,
per-source 1, `MAX_CONNECTIONS = 6`, grace 3 s.

### 2. Measured per-connection cost — the numbers the budget rests on

Measured on target, diagnostic build, HMI connected and authenticated:

| Quantity | Measured |
|---|---|
| Client task stack | 8192 B configured; **worst free margin 2504 B** → 5688 B used |
| Heap per connection | 56,544 → 44,736 free with one extra pending socket = **11,808 B** |
| Task count per connection | 24 → 25 |
| Connection record | 344 B × 6 |
| Session record | 1288 B × 3 |
| Broker static (`s_broker`) | 29,424 B (link map `.bss.s_broker` 0x72f0) |
| Recovery after release | 56,924 B free, 24 tasks — **no slot or task leak** |

**The stack must not be cut.** I expected 8192 to be inherited and generous
and intended to halve it; the measurement says 5688 bytes are actually used,
so 8192 leaves ~30% margin and is correct. This is the direct reason
`MAX_CLIENTS` is 3 and not 4: at 11.8 KB per connection, six connections cost
~71 KB, which the production heap supports with headroom and a seventh would
not.

### 3. Adversarial pending-socket exhaustion — what was and was not shown

**Demonstrated on hardware, single source (192.168.50.172):** six sockets
opened simultaneously → **1 admitted, 5 refused** by the per-source cap
(`per_source=5` in the refusal counters). Peak pending 1. The authenticated
HMI was unaffected throughout (`Authenticated 1/3` steady). After release and
grace expiry, pending returned to 0, tasks to 24, heap to 56,924.

**Demonstrated on hardware, sustained mixed flood** (silent sockets +
bogus CONNECTs, 1441 sockets / 867 CONNECTs over 200 s), with the chip reset
mid-flood so the HMI was forced to drop and reconnect **while the flood was
running**:

- HMI re-authenticated at **16.3 s** (diagnostic image) / **19.9 s and 22.7 s
  and 32.1 s** across production runs.
- 128 and 793 pre-connect refusals logged.
- The **reserved share fired on hardware**: `reserved=41` refusals, i.e.
  unproven sockets turned away to keep a slot available to a proven source.
- Zero panics, zero watchdogs, zero `NO_MEM`, zero client-task creation
  failures, zero pool-invariant violations, zero `active_client_count`
  underflows.
- Wi-Fi, SoftAP, MQTT, HTTP and the network manager all stayed up; HTTP
  answered (`HTTP/1.1 403 Forbidden` — the AP-only policy correctly refusing
  a station-side request to the portal, i.e. the server is alive and
  enforcing).

**NOT demonstrated on hardware, and this is a real gap:** the multi-source
case. The bench host has exactly one routable address (192.168.50.172 on
Ethernet 2; WSL and Hyper-V switches are NAT'd behind it, Wi-Fi is
disconnected). With `MAX_PRECONNECT_PER_SOURCE = 1`, **one host cannot hold
more than one pending socket by design**, so it cannot fill the pool. Adding
an IP alias to the host is a system network-settings change I did not make.
The multi-source behaviour is covered exhaustively through the production
decision function instead, and is labelled seam-verified below, not
hardware-demonstrated. Shawn's bounded action to close it is listed at the
end.

I kept `PER_SOURCE = 1` on merit rather than loosening it for testability: it
is the stronger bound, and a legitimate client's existing socket is
`connected`, so it does not consume this pool.

### 4. Sustained authentication flood — the honest answer

**Inspection result.** Within the existing protocol, nothing can identify a
legitimate client before credential verification: client id and username in
CONNECT are attacker-chosen text, so reserving capacity "for the HMI's
identifier" reserves it for whoever types that identifier. The only
pre-verification signals are the source address, the receiving interface, and
history. Source address is not authenticated either, but it is not free — the
peer must complete a TCP handshake from it.

**What was implemented.** A proven-source reservation: an address that
completed a successful machine authentication within 10 minutes keeps a
reserved share of both the pre-connect pool (1 of 3) and the KDF budget
(3 of 8). Entries are created **only** by a successful authentication, and the
table is deliberately separate from the LRU failure buckets so that an
attacker cycling addresses cannot evict one. No wire change; no new credential
exchange; no architecture change.

**Exact supported guarantee.** A flood from addresses that have never
authenticated cannot consume the whole pre-connect pool or the whole KDF
budget; a recently authenticated client retains a share and reconnects with
bounded delay. Measured: 16–32 s to re-authenticate under continuous flood.

**Residual limitation, asserted in the suite so it cannot quietly become a
claim** (`test_4d4_proven_source_reservation_bounds_the_flood`):

- Nothing against a flood originating from or spoofing a proven address — the
  test drives exactly that case and asserts the proven source is refused.
- Nothing before the first successful authentication after a reboot; the
  table is empty and the HMI competes as unproven. Every hardware reconnect
  measured above was in precisely this state.
- No identification of a legitimate client, at all.

**Future protocol work, named and not attempted:** a pre-authentication
challenge (TLS-PSK, or an HMAC cookie in CONNECT) is what would actually
distinguish a legitimate client. Wire-protocol change; out of scope.

**Operator-visible condition.** `status/core/auth_throttle` is published
`ACTIVE`/`CLEAR` on transition, and each transition is logged so it is visible
on a production image with no diagnostic menu.

Two corrections were made to this condition after watching it on hardware:

1. It first keyed only on the global KDF budget. Against a single-address
   CONNECT flood the **per-source failure lockout fires long before the
   budget does** — 1803 of 1840 attempts were refused by the lockout and the
   global bucket never engaged, so the published condition stayed `CLEAR`
   through a sustained flood. It now covers every reason authentication is
   being refused.
2. It then asserted when the budget was merely *spent*, producing the log line
   `THROTTLED: 0 refusals` — which is not a throttle. Five legitimate
   reconnects reach that state on an ordinary power-up storm. It now keys on
   refusals **within the current window**.

Final hardware evidence, production image:

```
W (13876) machine authentication THROTTLED: 6 refusals (6 by the KDF budget,
          0 sources in lockout); clears in 2 s
I (15878) machine authentication throttle cleared
W (17881) machine authentication THROTTLED: 32 refusals (32 by the KDF budget,
          1 sources in lockout); clears in 28 s
I (46016) machine authentication throttle cleared
```

**Also corrected:** the comment in `argus_machine_service_authenticate()`
claimed the global bound was consulted "before any per-source decision". It is
not — the per-source failure lockout runs first. The comment now states the
real order and why it is acceptable.

### 5. AP/STA ambiguity published as an authoritative network fault

`status/core/network_fault` (`NONE` | `AP_STA_ADDRESS_CONFLICT`) and
`status/core/network_fault_action` are published on transition, retained, and
logged. Clearing is deterministic and requires **positive evidence** — a
classification observing both interfaces valid and non-overlapping — never a
timer and never an interface merely going away. Ambiguous log lines are
rate-limited to one per minute; the fault is a state, not an event, and the
state is published.

Nothing suspect survives a clear: while the fault holds, ambiguous sockets are
refused admission, so there is no session admitted under ambiguity to keep
distrusting afterwards.

**Demonstrated end to end on hardware through the production seam** — the
same `argus_net_mgr_classify_interface()` the broker and HTTP server call,
the same 1 Hz publisher, and the retained value read back from the broker:

```
1. cleared            : latch=CLEAR published=NONE
2. overlap            : classify=AMBIGUOUS (SOFTAP would be a defect) latch=SET
3. published          : AP_STA_ADDRESS_CONFLICT
   action             : AP and station interfaces report the same IP address...
   AP-only admission for an ambiguous socket: REFUSED
4. persists           : latch=SET (interface down is not evidence of health)
5. cleared by evidence: latch=CLEAR published=NONE
```

The published state agreed with the classifier latch at every step. A genuine
AP/STA collision needs a rogue DHCP server on the plant segment, which the
bench does not have, so the addresses are fed to the production classifier by
a diagnostic-only console action rather than staged on the network — stated
plainly rather than presented as a live network event.

### 6. Retained-store overflow — found on hardware, not in review

Adding three retained topics pushed the retained store past its 32-slot
capacity. The broker did the right thing (`retained capacity exhausted;
refusing to evict authoritative state`) but the consequence was silent:
`network_fault_action` and `auth_throttle` never became retained, so a client
subscribing later would not have learned about a live fault.

At 32 the store was **already only one slot clear of the 31 retained topics
the contract publishes** — a latent fragility that predates this pass.

Fixed: capacity 32 → 40, `ARGUS_MQTT_RETAINED_TOPICS_REQUIRED = 34` declared
in the contract header, a static assertion tying them together, live
occupancy exposed via `argus_mqtt_broker_get_capacity()`, and a suite check
that the store keeps headroom. Hardware confirms **34 / 40**.

### 7. Production image — boot-looped on first flash

`sdkconfig.production.defaults` documented the build as
`-DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.production.defaults"`.
`sdkconfig.defaults` carries no credentials — Shawn's Wi-Fi passphrase, AP
secret and machine secret live only in the gitignored generated `sdkconfig`.
So the production image was built with an empty `CONFIG_ARGUS_SERVICE_AP_PASS`
and aborted in `app_main` at the AP-secret bootstrap:

```
ESP_ERROR_CHECK failed: esp_err_t 0x102 (ESP_ERR_INVALID_ARG)
file: "./main/app_main.c" line 1012   expression: ap_bootstrap_err
```

Continuous boot loop, no network, no serial application output. The §-3 pass
built this image and proved its exclusions from the link map, but never
flashed it, so nothing caught it. The `.gitignore` comment asserting that
`sdkconfig.production` "inherits everything in the gitignored sdkconfig
chain" was simply false.

Corrected to chain the **live** config: `-DSDKCONFIG_DEFAULTS="sdkconfig;
sdkconfig.production.defaults"`. Later entries override earlier ones, so the
production overlay still wins for `CONFIG_ARGUS_DIAGNOSTIC_MODE=n`, and every
credential stays in the single gitignored file it already lived in. No secret
was read, copied, printed or logged at any point.

### Build results — both configurations, zero warnings

| | `.bss` | `.data` | Image | `argus_tests` refs | `4a_run_all` | `bss.topics` | diagnostic task |
|---|---|---|---|---|---|---|---|
| Diagnostic | 84,040 | 20,156 | 1,285,473 | **2218** | 6 | 1 | 3 |
| Production | 74,312 | 20,140 | 1,053,641 | **0** | 0 | 0 | 0 |

`.bss.s_broker` is 0x72f0 (29,424 B) in both.

### Hardware — production image, flashed to COM5

Services on boot: MQTT broker listening, HTTP server, Wi-Fi driver, SoftAP,
network manager, MQTT runtime — all present. Diagnostic surface absent:
0 menu banners, 0 test-suite banners, 0 capacity reports.

| Run | Load | HMI re-auth | Refusals | Faults |
|---|---|---|---|---|
| prod2 | mixed flood, 407 sockets / 208 CONNECTs | 32.1 s | 793 pre-connect | none |
| prod3 | CONNECT flood, 1196 attempts | 22.7 s | 5 throttle assertions | none |
| prod4 | CONNECT flood, 1088 attempts | 19.9 s | 5 assertions / 4 clears | none |

Every run: 0 panics, 0 watchdogs, 0 `NO_MEM`, 0 task-creation failures,
0 pool-invariant violations, 0 count underflows; HTTP answered throughout.

### Hardware — diagnostic image, three suite runs under load

HMI connected and authenticated for all three (`Broker Clients STEADY 1 → 1`,
`AP Stations STEADY 1 → 1`, `Task Count UNCHANGED 24`), every isolation field
clean (`Authority Generation UNCHANGED Gen 3`, `Network State UNCHANGED
AP_DISCOVERABLE`, `Machine State UNCHANGED UNLOCKED`).

| Run | Distinct | Executed | Passed | Failed | Heap before | Heap after | Largest block | Diagnostic stack free |
|---|---|---|---|---|---|---|---|---|
| 1 | 341 | 1023 | 1023 | 0 | 56,480 | 56,960 | 31,744 | 3,696 / 16,384 |
| 2 | 341 | 1023 | 1023 | 0 | 58,132 | 57,076 | 31,744 | 3,840 / 16,384 |
| 3 | 341 | 1023 | 1023 | 0 | 58,096 | 55,472 | 29,696 | 3,844 / 16,384 |

An earlier run in this pass, before the retained-store fix, also returned
1023/1023 — the overflow was a warning-level failure invisible to the suite,
which is why the suite now checks retained-store headroom.

### Evidence classification — derived vs seam-verified vs hardware-demonstrated

**Hardware-demonstrated:** per-connection heap and task cost; client task
stack low-water; per-source pre-connect cap; reserved-share refusal;
sustained-flood survival with HMI reconnect on both images; throttle
assertion, breakdown and clearing; AP/STA fault assertion, persistence,
publication and evidence-based clearing; retained-store occupancy; production
service startup and diagnostic-surface absence; no leaks after load.

**Seam-verified only (production functions, not live traffic):** the
multi-source pre-connect case and the reserved share across two or more
source addresses; the authenticated-pool refusal of one client beyond the
limit (CONNACK 0x03); the proven-source residual limitations.

**Derived, not observed:** that `MAX_CLIENTS = 3` and `MAX_PRECONNECT = 3`
can be occupied *simultaneously* within the production heap. The arithmetic
is 6 × 11.8 KB ≈ 71 KB against a production free heap of roughly 80 KB at one
client, but full occupancy was never reached, because reaching it needs three
enrolled machine credentials.

### Two items requiring Shawn — exact bounded actions

Machine enrollment is reachable only through the authenticated browser
security API (`enroll_machines` permission). I hold no portal credential and
did not attempt to obtain one.

**A. Prove 3 authenticated clients and clean refusal of the 4th.**
1. Browser portal → Security → enroll two machines, e.g. `m-test-a`,
   `m-test-b`, transport MQTT, interface STA, permission `view_status` only
   (no motion, no authority). The portal shows each secret once.
2. Run three concurrent MQTT clients (rotary HMI + the two test machines),
   then a fourth. Expect: three connected, the fourth answered
   **CONNACK 0x03**, `session_pool` refusal counter incremented.
3. Console `[b]` before and after for heap, largest block, task count and
   both pool occupancies.
4. Revoke `m-test-a` and `m-test-b` afterwards.

**B. Fill the pending pool from two or more sources.**
Copy `flood_worker.py` to a second machine on the plant LAN and run it
against `192.168.50.236` for 60 s while the first host also runs it, then
reset the controller so the HMI reconnects mid-flood. Expect: pending reaches
2 from unproven sources, the third refused with `reserved` as the reason, and
the HMI still reconnects.

Neither is a defect; both are measurements I could not take without a
credential or a second host.

### Files changed

`main/argus_mqtt_broker.c/.h`, `main/argus_machine_service.c/.h`,
`main/argus_net_mgr.c/.h`, `main/argus_mqtt_runtime.c/.h`,
`main/argus_mqtt_contract.c/.h`, `main/app_main.c`,
`main/argus_tests_4d4.c/.h`, `main/argus_tests_4a.c`,
`sdkconfig.production.defaults`, `docs/PHASE_4C_MQTT_CONTRACT.md`,
this record. HMI repository unchanged.

---

## -3. Network admission and resource-budget closure (2026-07-27)

Shawn's work order, acting on the §-2.2 findings. Six items; all implemented,
built in BOTH configurations with zero warnings, and verified on hardware
under load.

### Design decisions and implemented limits

**1. Authentication flood.** A global token bucket — 8 machine-auth KDF
derivations per 10 s — is now consulted BEFORE any per-source decision. It
is the bound that cannot be escaped by cycling source addresses, because it
never looks at the address; the 8 per-source buckets remain, but they were
never a bound (only 8 of them, LRU-evicted, and a BLOCKED bucket is the
stalest so it was evicted first — an attacker cleared its own cooldown by
moving address). Concurrent machine KDF admission dropped 2 → 1 so the
depth-1 worker queue always has a slot for the browser/recovery path, which
shares the worker. Sizing rationale: a legitimate reconnect storm is small
and rare; 8/10 s covers it with margin while capping an attacker at ~16 s of
worker time per 10 s instead of a 100% duty cycle. **Stated honestly: during
an active flood a legitimate reconnect may be delayed up to one window. It
is bounded delay, not indefinite denial** — and the browser recovery path
stays available throughout.

**2. Pre-CONNECT exhaustion.** Unauthenticated sockets are now bounded
SEPARATELY from authenticated clients (`ARGUS_MQTT_MAX_PRECONNECT` = 4) with
a per-source cap of 2, so unauthenticated load can never consume
authenticated capacity and one address cannot fill the pool alone. The
CONNECT grace deadline dropped 30 s → 3 s.

**3. Client capacity — measured, not chosen.** `ARGUS_MQTT_MAX_CLIENTS`
10 → **4**. Each accepted client measured **~13.0 KB of heap** (8192 B task
stack plus ~4.9 KB lwIP/TCB), from this document's own instrumentation:
free heap 45,984 B at 0 clients vs 32,932 B at 1. Ten clients would have
needed ~130 KB that does not exist; the shortfall surfaced as unrelated
allocations failing, which is how the 4D.4 directory tests began failing.
Subscriptions per client 20 → 8 (the contract defines a 5-filter budget), so
`s_broker` fell **54,536 → 25,592 bytes**.

**4. AP/STA ambiguity.** One shared classifier
(`argus_net_mgr_classify_interface`) replaces two independent
implementations. A local address matching BOTH interfaces — or neither —
returns `ARGUS_NET_IFACE_AMBIGUOUS`, never `SOFTAP`. The old MQTT path
checked AP first and returned immediately, so an overlap resolved to the MORE
privileged answer. Ambiguity now denies AP-only HTTP routes and yields
interface 0 for MQTT, which `argus_machine_service_authenticate()` rejects,
so a SOFTAP-only machine record cannot authenticate from an ambiguous socket.
The conflict is latched and readable
(`argus_net_mgr_interface_conflict_detected`) so the refusal is explainable
rather than an unexplained lockout.

**5. Contract reconciliations.** All six closed in
`docs/PHASE_4C_MQTT_CONTRACT.md`: replay bound `2 × MAX_MACHINES` →
`MAX_MACHINES` (16, matching code); A2.2 payload ceiling 512 → **384**, the
value actually enforced by `ARGUS_MQTT_BROKER_PAYLOAD_CAP` before any
callback runs; `retain_forbidden` documented as **reserved and unreachable**
(broker policy drops retained command publishes before the application sees
them — the refusal was always real, the reporting never existed); QoS 0 now
**publishes** a `qos_1_required` result instead of silently logging; a new
`denied_window_open` reason distinguishes "the profile permits you, wait for
ArgusCore's window" from "the profile forbids you, recommission the unit";
and A2.4 now states explicitly that a replayed result carries the values AS
OF THE ORIGINAL DECISION, with A2.6 governing.

**6. Production build gate.** `CONFIG_ARGUS_DIAGNOSTIC_MODE=n` now excludes
every `argus_tests_*.c` translation unit from the link (main/CMakeLists.txt)
and the test headers from `app_main.c`, so fixtures are absent from the
image rather than merely unreachable. Built via
`sdkconfig.production.defaults`; the generated `sdkconfig.production` and
`build_production/` are gitignored because they inherit the credential-bearing
sdkconfig chain.

### Build proof — production excludes diagnostics

| | `.bss` | `.data` | image |
|---|---|---|---|
| Diagnostic | 79,104 | 20,156 | 1,275,280 |
| Production | **69,864** | 19,852 | **1,049,936** |

`argus_tests` symbol references in the link map: **diagnostic 2,180,
production 0**. `argus_tests_4a_run_all`: 6 vs 0. The duplicate 6,952-byte
`bss.topics` fixture: present vs **absent**. Both configurations compile with
zero warnings.

### Hardware verification — three runs, HMI connected throughout

| Run | Executed | Passed | Failed | Heap before | Largest block | Stack free |
|---|---|---|---|---|---|---|
| 1 | 1014 | 1014 | 0 | 63,312 | 31,744 | 4,248 |
| 2 | 1014 | 1014 | 0 | 63,248 | 31,744 | 4,096 |
| 3 | 1014 | 1014 | 0 | 61,700 | 31,744 | 4,240 |

338 distinct tests × 3 repeat passes, per run. Every run: `Broker Clients
STEADY (1 -> 1)`, `Task Count UNCHANGED (24)`, all isolation fields
UNCHANGED/STEADY. Free heap under HMI load rose **32,932 → ~63,000** and
largest contiguous block **21,504 → 31,744** versus the previous pass — the
budget work returned ~30 KB, which is what makes 4 clients supportable
rather than aspirational.

**A first run failed 6/1014 and is recorded rather than hidden:**
`test_4c_authority_startup_window_blocks_early_hmi` and
`test_4c_seam_denied_paths_preserve_operation` asserted `DENIED_PROFILE` for
the acquisition-window case, which item 5 deliberately changed to
`DENIED_WINDOW_OPEN`. The tests caught an intended semantic change, which is
what asserting the reason code is for. Expectations corrected; behaviour
under test (a faster boot never converts into control) is unchanged.

### Network admission probes — against the live broker

Non-motion probes only: sockets and MQTT CONNECT packets. **No PUBLISH of any
kind was sent, so no command could reach the controller.**

| Probe | Observed | Meaning |
|---|---|---|
| Silent socket | closed by broker after **4.6 s** | 3 s deadline + 2 s liveness poll. Was 30 s. |
| 12 sockets from one source | **10 closed by broker**, 2 held | per-source pre-connect cap of 2, exactly |
| 25 bogus-credential CONNECTs | **20 fast refusals, 5 KDF-paced**, 0 errors | global bucket capping work; only the budgeted few reach PBKDF2 |

After the probes the controller was healthy — no panic, no `NO_MEM`, Wi-Fi /
AP / network manager / broker / HTTP all up — and **the HMI re-authenticated**
(`authenticated machine connected`), which is the "flooding must not
indefinitely deny legitimate HMI reconnection" requirement demonstrated
rather than asserted.

### Not verified, stated plainly

- **Maximum declared client load (4 authenticated clients) was NOT exercised.**
  Doing so requires four enrolled machine credentials; Shawn holds the machine
  secret and only the rotary HMI is enrolled on this bench. The 4-client figure
  is derived from measured per-client cost (~13.0 KB) against measured free
  heap, not from observing four simultaneous authenticated clients. **This is
  the one verification item in the work order I could not complete.**
- The AP/STA ambiguity guard is verified through the production classifier
  seam (`test_4d4_iface_ambiguity_fails_closed`, all branches including the
  latch), not by staging a rogue DHCP server on the plant network.
- The production image was built and its exclusions proven from the link map;
  it was **not flashed**. COM5 continues to run the diagnostic build, which is
  what acceptance testing requires.

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
