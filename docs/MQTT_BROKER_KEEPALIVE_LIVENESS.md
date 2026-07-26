# MQTT Broker Keep-Alive Liveness Enforcement

**Status:** implemented and verified on hardware, 2026-07-26.

**Scope:** `main/argus_mqtt_broker.c` only. No change to the Phase 4C
command, authority, session, or fail-operational architecture, and no change
to the duplicate-client-ID *policy*.

## The defect

The broker parsed the CONNECT packet's keep-alive value into
`argus_mqtt_connect_request_t.keep_alive_s` and then **never used it**. No
liveness timeout existed anywhere, and each client task blocked
indefinitely in `recv()` with no socket timeout.

Consequence: a peer that died without sending a clean DISCONNECT — device
reset, power loss, Wi-Fi drop, half-open TCP — left its slot allocated and
its client ID marked connected **forever**. Because Phase 4C §5
deterministically rejects a duplicate client ID (correctly, and by design),
the real device was then permanently refused with CONNACK return code 2 on
every reconnect attempt. Only rebooting the controller cleared it.

This is a field-availability defect, not a theoretical one: an HMI that
loses power once would never reconnect to its own controller until someone
power-cycled the controller.

## How it was found

During Phase 2.5 bring-up of `ArgusControl_PumpHMI-Rotary-V1`. The HMI
connected successfully, was reset to load new firmware, and was then locked
out — CONNACK 2 on every retry for more than five minutes across ~10
attempts, resolving only when the controller was restarted. Authentication
was never the problem: the rejection came from the *bind* stage
(`!bind_allowed`), well after the credential check, which is why the code
was 2 and not 4.

## The fix

MQTT 3.1.1 §3.1.2.10 already specifies the required behavior: *"If the Keep
Alive value is non-zero and the Server does not receive a Control Packet
from the Client within one and a half times the Keep Alive time period, it
MUST disconnect."* The broker now does that.

- Accepted sockets get `SO_RCVTIMEO` (`ARGUS_MQTT_RECV_POLL_S`, 2s) so a
  client task wakes periodically instead of blocking forever.
- `argus_mqtt_client_t` gained `keep_alive_s` and `last_activity_us`.
  The keep-alive is adopted from CONNECT when the bind succeeds; the
  activity stamp is refreshed on every received packet.
- On an idle poll the task compares elapsed idle time against the deadline
  and closes the connection when exceeded, releasing the slot and the
  client ID.
- **Keep-alive 0 disables the timeout**, per spec.
- A freshly accepted socket that never sends CONNECT is reaped after
  `ARGUS_MQTT_CONNECT_GRACE_US` (30s), closing a second slot-exhaustion
  hole.

### What deliberately did NOT change

- **Duplicate client IDs are still rejected deterministically.** MQTT 3.1.1
  would permit the new connection to take over instead, but Phase 4C §5
  chose rejection, and Phase 4D.4 acceptance-tested it. That decision
  stands. The fix only makes "already connected" mean "actually alive".
- Packet limits, subscription limits, authentication, authorization,
  topic policy, and retained-state handling are untouched.

### Regression guarded against

Adding a socket timeout means a `recv()` mid-packet can now return `EAGAIN`
simply because TCP split a packet across segments. Treating that as a
disconnect would drop healthy clients. `argus_mqtt_read_exact()` therefore
retries on `EAGAIN` with a bounded budget
(`ARGUS_MQTT_PARTIAL_READ_POLLS`, 8 polls ≈ 16s) and only fails if the peer
genuinely stalls mid-packet. The single header byte read in
`argus_mqtt_client_task()` is a deliberate bare `recv()` — that read *is*
the idle detector and must surface `EAGAIN`.

## Verification

Clean ESP-IDF build, zero warnings; app binary `0x12b680`, 61% free in the
3MB OTA slot. Flashed to the controller on COM5 (identity re-verified:
`USB\VID_303A&PID_1001&MI_00\9&26DB8A4B&1&0000`).

Live behavior with the rotary HMI:

- HMI authenticated (`CONNACK accepted`) and subscribed successfully;
- HMI was reset while holding a live session, then **reconnected cleanly**
  — the lockout that motivated this fix did not recur;
- sustained telemetry delivery, ~2000 messages, no broker faults.

## Silent-death path — PROVEN 2026-07-26

The gap recorded above is now closed. The rotary HMI's power was physically
disconnected while it held a live, subscribed MQTT session, left off, then
reconnected. That is a genuine silent death: no DISCONNECT packet, a
half-open socket on the broker, and exactly the case the keep-alive timer
exists to handle.

**The HMI reconnected unaided.**

The decisive detail is the broker session identifier. `status/core/command_session`
read `ed8192703745619c` both before the power cut and after the reconnect —
byte-identical. Per Phase 4C §6 every controller boot and every broker
lifecycle generates a fresh random value, so an unchanged session proves the
**controller did not reboot**. Before this fix, a controller restart was the
only thing that could release a dead client; the HMI rejoining the same
broker lifecycle therefore proves the keep-alive sweep genuinely reaped the
stale connection and freed both the slot and the client ID.

Corroborating, from the same window: no session change, no parse failures,
no error-level output, and the reconnected client's subscriptions were
granted in full (5/5, none rejected).

Full HMI-side capture and analysis:
`ArgusControl_PumpHMI-Rotary-V1/docs/evidence/phase2/PHASE2_STATUS.md`.
