# Phase 4C MQTT Supervisory Contract

**Status:** ACCEPTED on July 22, 2026. **AMENDED — see Amendment A1 below.
The amendment is in force on the `atlantis-authority-integration` branch and
merges to `main` only after acceptance testing.**

**Firmware identity:** `v2-phase4c-dev`

## Amendment A1 — Authority acquisition becomes explicit (2026-07-26)

Authorized by Shawn on 2026-07-26. Governing decision documents:
"Deterministic Initial Authority Selection" and "Pump Operation — Authority
Changes". Implementation plan:
`ArgusControl_PumpHMI-Rotary-V1/docs/plan/PHASE_3_PROVISIONING_AND_COMMAND_PLAN.md`.

**What changed and why.** As accepted, §7 made the heartbeat *itself* the
act of acquiring command authority: the first valid current-session
heartbeat bound the lease. That is implicit acquisition, and it is
incompatible with the governing rule that **connection proves presence but
does not grant control**. It also made authority depend on connection order,
which the decision prohibits outright — a client must not win control
because its boot time is shorter or its Wi-Fi associated first.

Under A1:

1. **Acquisition is an explicit, validated, epoch-changing request.** The
   controller evaluates identity, scope, capability, session, the
   commissioned `authority_profile`, and current machine state, then grants
   or refuses. Refusal has no side effects.
2. **The heartbeat is demoted to lease renewal.** It keeps an
   already-granted lease alive. It can no longer create one.
3. **The lease belongs to the authenticated principal**, not to the socket.
4. **Initial ownership comes from the commissioned `authority_profile`**
   (`STANDALONE_HMI` or `ARGUSCORE_PREFERRED`), never from arrival order.
5. **Transfer is asymmetric and controller-adjudicated.** ArgusCore may
   request transfer from the HMI; the HMI may not take authority from a
   healthy ArgusCore lease; the controller decides both.

**What did NOT change, and must not.** §1's fail-operational rule stands
unaltered and is reinforced by A1: loss of MQTT, loss of the heartbeat, loss
of the lease, and transfer of authority all remain incapable of stopping
motion, clearing a target, disabling the driver, or synthesizing a command.
Authority determines who may issue the *next* accepted command. It does not
own the RUN intent or accepted setpoint the controller already holds.
Regression tests `test_4c_fail_operational_*` pin this.

Sections amended: §5 (final paragraph) and §7 (replaced). All other sections
are unchanged and remain as accepted.

## 1. Authority and Safety Boundary

MQTT transports intent and controller information. It is not authoritative. The command router, authority manager, state manager, trajectory engine, and step generator retain their existing ownership. The only normal MQTT motion path is:

`broker -> Phase 4C transport/session decoder -> argus_command_envelope_t -> argus_cmd_router_dispatch() -> authority manager -> state manager -> trajectory -> step generator`

MQTT software E-stop uses the accepted software preemption path. It is not a safety-rated physical E-stop. Loss of MQTT or heartbeat is fail-operational: it changes link observability but does not stop motion, clear a target, disable the driver, or synthesize a command.

## 2. Dynamic Topic Root

The root is constructed once per broker lifecycle from one coherent commissioned-identity snapshot:

`argus/<client_id>/<unit_id>`

The accepted controller uses `argus/paladin/pump_001`. Components must be nonempty and must satisfy the accepted identity contract. Slash, `+`, `#`, control characters, and overflow are rejected. Topic construction never truncates and never falls back to `argus/peristaltic/...`.

## 3. Canonical Topic Tree

External command topics:

```text
command/pump1/set_target_rpm_milli
command/pump1/start
command/pump1/stop
command/pump1/unlock
command/pump1/e_stop
command/pump1/reset_e_stop
command/pump1/recover
```

External heartbeat topic:

```text
status/supervisor/heartbeat
```

Controller-owned retained metadata:

```text
metadata/core/device_name
metadata/core/model
metadata/core/firmware_version
metadata/core/hardware_uid
```

Controller-owned retained state:

```text
state/core/online
state/supervisor/link
state/pump1/mode
state/pump1/driver
state/pump1/direction
state/pump1/estop
state/pump1/fault
```

Controller-owned retained status:

```text
status/core/wifi
status/core/mqtt
status/core/network_mode
status/core/authority_mode
status/core/authority_owner
status/core/uptime_s
status/core/command_session
status/core/last_accepted_sequence
```

Controller-owned retained open-loop telemetry:

```text
telemetry/pump1/configured_target_rpm_milli
telemetry/pump1/trajectory_target_rpm_milli
telemetry/pump1/applied_rpm_milli
telemetry/pump1/generated_rpm_milli
telemetry/pump1/generated_step_count
telemetry/pump1/feedback_available
```

Non-retained application result:

```text
event/pump1/command_result
```

Every path above is appended to the dynamic root.

## 4. Topic Ownership and Retain Policy

External clients may publish only to the seven exact command topics and the exact heartbeat topic. Broker policy runs before retained storage, subscriber delivery, application parsing, heartbeat mutation, or command dispatch. External publication to metadata, state, status, telemetry, event, alarm, configuration, wildcard, near-match, and legacy paths is rejected.

Commands, heartbeats, and command results are never retained. Metadata, authoritative state, status, and open-loop telemetry are retained. The broker has 32 retained slots for the 25-topic baseline and refuses capacity exhaustion instead of evicting authoritative state.

Subscriptions are read-only observation and do not grant publication authority.

## 5. Broker Connection Identity

Each accepted socket receives a monotonically allocated 64-bit connection identity. Application callbacks receive a bounded copy of client ID, connection identity, exact topic, payload length, QoS, RETAIN, DUP, and broker-policy result. No application callback retains a broker packet pointer.

Simultaneously active duplicate MQTT client IDs are rejected deterministically.

**Amended by A1.** As accepted, this paragraph stated that lease ownership uses the connection identity rather than the client ID, so that a recycled slot or repeated name could not impersonate an earlier socket. That protection is retained but re-based: **lease ownership uses the authenticated machine principal**, and the connection identity proves that principal is still on the far end of a live socket. Impersonation is prevented by authentication rather than by socket identity, which is strictly stronger — a repeated client ID never reaches lease arbitration unauthenticated. Keying solely to the connection also had an operational defect: a supervisor that dropped and reconnected was refused its own lease until the heartbeat timeout expired, opening a window in which another client could take control. See §7.

## 6. Broker Command Session

Every controller boot and every prepared broker lifecycle generates a nonzero random 64-bit value, formatted as 16 lowercase hexadecimal characters. It is retained at `status/core/command_session`, is not persisted in NVS, and invalidates all prior command envelopes. A broker lifecycle restart generates a new value even without an MCU restart.

## 7. Heartbeat and Lease

Heartbeat schema:

```json
{"session":"0123456789abcdef","counter":1}
```

The object is strict, flat, bounded, non-retained, and contains exactly one `session` and one nonzero uint32 `counter`. Unknown, duplicate, missing, nested, malformed, oversized, embedded-NUL, and trailing content is rejected.

**Amended by A1. The heartbeat renews a lease; it does not acquire one.**

As accepted, the first valid current-session heartbeat bound an unowned
lease to the connection that sent it. That behaviour is withdrawn: a
heartbeat from a principal that does not already hold the lease is not an
acquisition and is refused. Authority is acquired only through the explicit
request described in A1, and initial ownership follows the commissioned
`authority_profile`, never arrival order.

The heartbeat therefore now means exactly one thing: *the holder is still
alive and still wants the lease.*

Rules in force:

- A heartbeat from the **current holder** renews the lease and refreshes the
  expiry deadline.
- A heartbeat from **any other principal** is refused and has no effect on
  the lease, the epoch, or machine state.
- The holder is the authenticated machine principal. A reconnect **inside
  the lease term** by that same principal renews the lease on the new
  connection and **preserves the authority epoch** — a dropped packet is a
  comms event, and comms events must not move authority. The expiry deadline
  continues to follow the most recent valid heartbeat, so a supervisor that
  goes silent after reconnecting still expires on schedule rather than
  surviving indefinitely inside a reconnect loop.
- Counters advance under uint32 serial-number arithmetic. Equal, older, or
  ambiguous half-range counters are rejected. Counter history is scoped to
  the connection that produced it, so a reconnecting holder may legitimately
  restart its counter.

Supervisors should publish every two seconds. After six seconds without a
valid heartbeat, link state becomes `STALE` and the lease is released; the
authority epoch ends with it. A confirmed disconnect becomes `OFFLINE`.
Counter history is retained while an expired socket remains connected to
reject replay, then cleared when that socket disconnects.

Once a lease has actually **expired**, the epoch is gone. A returning
principal — including the previous holder — must re-authenticate, read
current authoritative state, synchronize its output to the live target, and
submit a new explicit request. Reconnection never restores a former lease
and never automatically reclaims control.

**Neither expiry nor disconnect nor transfer mutates machine state, motion
output, RUN intent, or the accepted setpoint.** This is the §1
fail-operational rule and it is binding: no comms or lease watchdog may be
connected to a stop, a deceleration, or clearing the setpoint.

## 8. Command Envelope

```json
{"session":"0123456789abcdef","sequence":1,"command_id":"batch-42.start","value":true}
```

Required fields appear exactly once. Unknown, duplicate, missing, nested, array, malformed, oversized, embedded-NUL, ambiguous, or trailing input is rejected. `command_id` is 1 through 36 alphanumeric, hyphen, underscore, period, or colon characters. `sequence` is a nonzero uint32. Commands require QoS 1 and RETAIN false and must come from the bound, fresh supervisory connection.

Topic-specific `value` contracts:

| Action | Value |
|---|---|
| `set_target_rpm_milli` | Integer 0 through configured maximum |
| `start` | `true` |
| `stop` | `true` |
| `unlock` | `true` |
| `e_stop` | `true` |
| `reset_e_stop` | `true` |
| `recover` | `true` |

Phase 4C has no direction field. `set_target_rpm_milli` uses the existing forward direction. Zero changes the requested setpoint to zero and is not reinterpreted as Start.

## 9. Freshness, Duplicate, and Replay Rules

Sequence ordering uses RFC-1982-style uint32 serial arithmetic: a nonzero delta below `0x80000000` is newer. The first admitted command may be evaluated; a strictly newer command may be evaluated; an older command is stale.

An exact same-session, same-sequence, same-command-ID, same-topic-action, and byte-identical-payload duplicate is not redispatched. The cached application result is republished. Reusing the latest sequence with changed correlation, action, or payload is `sequence_conflict`. Parser, topic, session, connection, QoS, RETAIN, heartbeat, and authority rejection do not consume a future sequence. A command evaluated by the state manager, whether accepted or state-rejected, commits the sequence and result.

MQTT E-stop is exceptional only in the established authority router. It still requires the exact topic, strict decoder, current session, bound fresh connection, QoS 1, and non-retained message.

## 10. Command Result

Every fully decoded request with safe correlation receives a bounded, non-retained result:

```json
{"session":"0123456789abcdef","sequence":1,"command_id":"batch-42.start","action":"start","outcome":"ACCEPTED","reason":"accepted","authority_generation":3,"command_generation":12,"machine_state":"RUNNING"}
```

Stable outcomes are `ACCEPTED` and `REJECTED`; an exact duplicate republishes its cached original result. Stable reasons include `accepted`, `state_rejected`, `authority_rejected`, `authority_unavailable`, `session_mismatch`, `supervisor_not_bound`, `qos_1_required`, `retained_forbidden`, `topic_forbidden`, `stale_sequence`, and `sequence_conflict`.

PUBACK proves only broker receipt. The application result proves the controller decision. Result-publication failure never reverses or repeats dispatch.

## 11. Authoritative Publication

A complete retained baseline is published after every broker start and refreshed after every client connection. Operational state is republished after state-manager command evaluation and at a bounded 1 Hz health cadence. Current values come from the existing authoritative snapshots.

`configured_target_rpm_milli`, `trajectory_target_rpm_milli`, `applied_rpm_milli`, `generated_rpm_milli`, and `generated_step_count` are controller intent/output telemetry. They are not shaft feedback. The accepted hardware publishes `feedback_available=false` and never publishes `actual_rpm`.

## 12. Legacy Disposition

The former `argus/peristaltic/cmd/...` production command path is removed. No dual subscription, dual dispatch, fallback, or writable compatibility layer remains. Legacy strings exist only in rejection tests. HMI and Node-RED consumers must discover `status/core/command_session`, establish a heartbeat lease, use monotonically newer sequences, wait for `command_result`, and treat retained controller state as authoritative.

## 13. Security Boundary

Sessions, sequences, connection IDs, client IDs, and topic policy provide lifecycle freshness and deterministic local ownership. They are not cryptographic identity or authentication. Phase 4C assumes a trusted local network. MQTT authentication, TLS, cryptographic publisher identity, rate limiting, abuse handling, and broader security review remain Phase 4D work.

## 14. Integration Example

1. Subscribe read-only to `argus/paladin/pump_001/#`.
2. Read the retained command session.
3. Publish QoS 1, non-retained heartbeats with increasing counters.
4. Wait for retained supervisor link `ONLINE`.
5. Publish a QoS 1, non-retained command with a unique bounded ID and newer sequence.
6. Correlate `event/pump1/command_result`.
7. Observe retained state and telemetry; do not infer physical motion from HTTP/MQTT acceptance or generated pulses.

On reconnect, read the session again before sending anything. Never replay buffered commands from an earlier session.

---

# Amendment A2 — Authority Wire Contract (2026-07-26)

**Status: NORMATIVE. Amends the accepted Phase 4C MQTT Supervisory Contract.**
Authorized by Shawn in the authority-protocol correction order, 2026-07-26.
The accepted tag is not rewritten; this amends forward on the integration
branch.

A1 established that authority is acquired explicitly and that a heartbeat
renews rather than grants. A1 did **not** define the wire protocol, and the
first implementation shipped with the payload ignored and the topics
unreachable at the broker. A2 defines the protocol completely so that no
part of it is left to implementation choice.

**Publishing to the correct topic is not a request.** A request exists only
when it decodes, validates, and passes admission as defined below.

## A2.1 Topics

All topics are relative to the dynamic root `argus/<client_id>/<unit_id>`.

| Topic | Direction | QoS | Retain |
|---|---|---|---|
| `command/core/request_authority` | client → controller | 1 | **must be false** |
| `command/core/release_authority` | client → controller | 1 | **must be false** |
| `event/core/authority_result` | controller → clients | 1 | false |
| `status/core/control_owner` | controller → clients | 1 | **true (snapshot)** |
| `status/core/authority_epoch` | controller → clients | 1 | **true (snapshot)** |
| `status/core/authority_profile` | controller → clients | 1 | **true (snapshot)** |
| `status/core/local_control_status` | controller → clients | 1 | **true (snapshot)** |
| `status/core/core_lease_status` | controller → clients | 1 | **true (snapshot)** |
| `status/core/authority_reason` | controller → clients | 1 | **true (snapshot)** |

A retained request is rejected with `retain_forbidden`. A retained command
topic would let a broker replay an acquisition after a session change, which
is precisely the class of thing this contract exists to prevent.

**Subscription budget.** The result topic is deliberately placed under
`event/` so an existing subscriber can widen its fifth filter from
`event/pump1/command_result` to `event/#` and cover both. The filter count
stays at 5 and the per-client subscription limit is not raised.

## A2.2 Request schema — `command/core/request_authority`

Maximum payload: **512 bytes**. Larger is rejected `payload_too_large`
without parsing.

```json
{
  "schema": 1,
  "request_id": "a1b2c3d4",
  "session": "0123456789abcdef",
  "authority_epoch": 7,
  "intent": "OPERATOR_INTENT"
}
```

| Field | Type | Required | Rules |
|---|---|---|---|
| `schema` | uint | yes | Must be `1`. Anything else → `schema_unsupported`. |
| `request_id` | string | yes | 1–36 chars, `[A-Za-z0-9._-]` only. |
| `session` | string | yes | Exactly the controller's current 16-char lowercase-hex command session. |
| `authority_epoch` | uint32 | yes | The epoch the request is predicated on. `0` means "no assumption" and is permitted **for requests only**. |
| `intent` | string | no | One of `OPERATOR_INTENT`, `SUPERVISORY_START`, `SERVICE`, `FALLBACK`. Advisory; recorded in audit, never affects admission. |

Unknown fields are **rejected**, not ignored (`unknown_field`), matching the
existing strict command decoder. Wrong types are rejected `invalid_value`.

## A2.3 Release schema — `command/core/release_authority`

Identical shape and limits, with one difference that matters:

`authority_epoch` **must equal the controller's current epoch exactly.**
`0` is not permitted. A release predicated on a stale epoch is rejected
`stale_epoch` and changes nothing.

This is what stops a delayed release from a previous owner releasing a later
owner's authority.

A release is valid only when **all** hold:
- the sender is the authenticated current owner (`not_owner` otherwise);
- the session matches (`session_mismatch`);
- the epoch matches exactly (`stale_epoch`);
- machine-state and profile policy admit it (`denied_machine_state`).

**Releasing authority does not stop the pump**, clear RUN intent, clear the
accepted setpoint, or alter output or trajectory. It changes only who may
issue the next accepted command.

## A2.4 Result schema — `event/core/authority_result`

Published for every request that survives broker admission, accepted or
rejected. A request rejected *at the broker* (topic scope or permission)
produces no result, because the controller never sees it — that is a
deliberate isolation property, not an omission.

```json
{
  "schema": 1,
  "request_id": "a1b2c3d4",
  "session": "0123456789abcdef",
  "outcome": "ACCEPTED",
  "reason": "granted",
  "control_owner": "LOCAL_HMI",
  "authority_epoch": 8,
  "core_lease_status": "ACTIVE",
  "local_control_status": "ACTIVE"
}
```

This satisfies the minimum observability requirement: the requester can
determine `request_id`, accepted/rejected, reason, current owner, current
epoch, and controller session from a single message.

## A2.5 Stable rejection-reason vocabulary

These strings are contract surface. They may be added to; existing values
must not be renamed or repurposed.

**Decode and envelope:** `schema_unsupported`, `payload_too_large`,
`malformed_json`, `missing_field`, `unknown_field`, `invalid_value`,
`retain_forbidden`, `qos_1_required`, `invalid_request_id`

**Binding:** `session_mismatch`, `stale_epoch`, `duplicate_conflict`

**Authorization** (broker boundary; no result published):
`topic_scope_denied`, `not_permitted`

**Admission:** `denied_by_profile`, `denied_held_by_other`,
`denied_machine_state`, `not_owner`, `transfer_unsupported_running`

**Success:** `granted`, `already_held`, `released`

## A2.6 Duplicate handling

Keyed on `(session, request_id)`.

- **Identical repeat** — same key, byte-identical payload: the cached result
  is republished. No arbitration, no epoch change, no lease renewal. This
  makes QoS 1 redelivery idempotent.
- **Conflicting repeat** — same key, different payload: rejected
  `duplicate_conflict` with zero mutation. A request id is not reusable for
  a different request.

The duplicate record is scoped to the controller session and is discarded on
session change.

## A2.7 Session and epoch change behaviour

- **Controller session change** invalidates every previous session's requests,
  results, duplicate records, and operational commands. A request carrying a
  prior session is rejected `session_mismatch` with zero mutation.
- **Authority epoch change** invalidates every operational command predicated
  on the previous epoch (see A2.9), and every release predicated on it.

## A2.8 Supported acquisition and transfer subset — CURRENT PHASE

Stated explicitly so the contract does not imply capability that does not
exist yet.

| Case | Supported now |
|---|---|
| Grant when unowned, profile permits | **Yes** |
| Same principal re-requests while holding | **Yes** — `already_held`, epoch unchanged |
| Owner releases | **Yes** |
| `ARGUSCORE` takes from `LOCAL_HMI`/`SERVICE_TOOL`, pump **not** running | **Yes** — epoch advances |
| `ARGUSCORE` takes from `LOCAL_HMI`/`SERVICE_TOOL`, pump **running** | **No** — rejected `transfer_unsupported_running` |
| `LOCAL_HMI` takes from a healthy `ARGUSCORE` lease | **No** — `denied_held_by_other` |
| Any client takes from a same- or higher-standing holder | **No** — `denied_held_by_other` |

Live running transfer awaits proven bumpless PID tracking. Until then it is
**deferred, not simulated**: the request is rejected with a defined reason,
the current owner keeps authority, and the pump keeps running. The controller
must never stop the pump to make a transfer easier to implement.

## A2.9 Operational command envelope — `authority_epoch` becomes REQUIRED

Every operational command envelope gains a required field:

```json
{
  "session": "0123456789abcdef",
  "sequence": 42,
  "command_id": "…",
  "authority_epoch": 8,
  "target_rpm_milli": 72000
}
```

A command whose `authority_epoch` does not equal the controller's current
epoch is rejected `stale_epoch` **before** the command sequence is consumed,
before any state mutation, before any setpoint change, before any motion
dispatch, and before any last-accepted-command record is updated.

Epoch validation **supplements** the existing identity, connection, session,
freshness, sequence, machine-state, and safety checks. It replaces none of
them.

Required sequence, which must hold even when the same principal regains
authority:

1. Principal A owns epoch 1.
2. A command from epoch 1 is delayed in flight.
3. Authority moves away from A. Epoch becomes 2.
4. A later reacquires authority. Epoch becomes 3.
5. The delayed epoch-1 command arrives.
6. It is **rejected with zero mutation**, even though A is the owner again.

An epochless command is invalid and is rejected `missing_field`. Contract
snapshots, fixtures, tests, and client plans must be updated so no client can
construct one.

## A2.10 Authority state vocabulary

`control_owner`: `NONE` | `LOCAL_HMI` | `ARGUSCORE` | `SERVICE_TOOL` | `OTHER`

`authority_profile`: `STANDALONE_HMI` | `ARGUSCORE_PREFERRED`

`core_lease_status`: `NONE` | `ACQUIRING` | `ACTIVE` | `EXPIRING` | `EXPIRED`

`local_control_status`: `UNAVAILABLE` | `WAITING` | `AVAILABLE` | `ACTIVE`

`authority_reason`: `STARTUP` | `STANDALONE_ACQUISITION` | `CORE_ACQUISITION`
| `OPERATOR_TRANSFER` | `CORE_LEASE_EXPIRED` | `CORE_REACQUISITION` |
`SESSION_INVALIDATED` | `RELEASED` | `TRANSPORT_LOST`

All six are retained snapshots and must be internally consistent after every
transition listed in the correction order §6. If the controller cannot
determine a value, it publishes the **most restrictive** value it can
justify — never a fictional `AVAILABLE`. Presentation fails closed.

## A2.11 Transport versus lease — normative

These are distinct concepts and must not be collapsed:

transport/link condition · authenticated principal identity · authority
ownership · renewable lease validity · heartbeat deadline · connection binding

- A transport disconnect marks the link offline and invalidates the departed
  connection binding. It **does not** clear an otherwise unexpired lease,
  **does not** advance the epoch by itself, and **does not** touch RUN intent,
  setpoint, output, or trajectory. The heartbeat deadline keeps running.
- The **same** authenticated principal may rebind to a new connection while
  the lease is unexpired, preserving owner and epoch. The old connection is
  rejected from that moment. Rebinding is not acquisition and replays nothing.
- A **different** principal may not inherit the lease by connecting or by
  heartbeating.
- On deadline expiry with no valid renewal: the lease expires, the epoch
  advances, delayed commands from the old epoch are invalidated, the
  commissioned fallback policy applies, RUN intent / setpoint / output are
  preserved, and the full A2.10 state is republished with an accurate reason.
