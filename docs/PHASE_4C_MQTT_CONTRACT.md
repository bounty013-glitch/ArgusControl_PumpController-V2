# Phase 4C MQTT Supervisory Contract

**Status:** ACCEPTED on July 22, 2026

**Firmware identity:** `v2-phase4c-dev`

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

Simultaneously active duplicate MQTT client IDs are rejected deterministically. Lease ownership uses the connection identity, not the client ID, so a recycled slot or repeated name cannot impersonate an earlier socket.

## 6. Broker Command Session

Every controller boot and every prepared broker lifecycle generates a nonzero random 64-bit value, formatted as 16 lowercase hexadecimal characters. It is retained at `status/core/command_session`, is not persisted in NVS, and invalidates all prior command envelopes. A broker lifecycle restart generates a new value even without an MCU restart.

## 7. Heartbeat and Lease

Heartbeat schema:

```json
{"session":"0123456789abcdef","counter":1}
```

The object is strict, flat, bounded, non-retained, and contains exactly one `session` and one nonzero uint32 `counter`. Unknown, duplicate, missing, nested, malformed, oversized, embedded-NUL, and trailing content is rejected.

The first valid current-session heartbeat binds an unowned lease to the actual connection. A different connection cannot steal an `ONLINE` lease. Counters advance under uint32 serial-number arithmetic. Equal, older, or ambiguous half-range counters are rejected.

Supervisors should publish every two seconds. After six seconds without a valid heartbeat, link state becomes `STALE` and the lease is released. A confirmed disconnect becomes `OFFLINE`. Counter history is retained while an expired socket remains connected to reject replay, then cleared when that socket disconnects. Neither transition mutates machine state or motion output.

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
