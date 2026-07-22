# Phase 4C Tests and Acceptance Record

**Current disposition:** FULLY ACCEPTED on July 22, 2026

**Firmware identity:** `v2-phase4c-dev`

## 1. Acceptance Boundary

Phase 4C accepts the production MQTT supervisory contract, dynamic Argus topics, broker-lifecycle sessions, heartbeat lease, freshness and replay protection, application command results, truthful open-loop telemetry, fail-operational supervisory loss, low-speed unloaded motor motion, MQTT software E-stop, reconnection, and final safe stop.

This acceptance does not cover pump head, hose, tubing, fluid, chemical, hydrogen peroxide, pressure, flow, calibration, loaded torque, process behavior, or endurance. MQTT E-stop is software-level and not a safety-rated physical E-stop. Generated output and step count are not physical feedback.

## 2. Starting State

- Starting branch: `main`
- Starting main and `origin/main`: `1bab5b464bc0fa55320232f7a5f76fa75598a6eb`
- Accepted baseline tag: `v2-phase4b.6-hardware-verified`
- Feature branch: `phase4c-mqtt-contract-and-fail-operational`
- Baseline firmware: `v2-phase4b.6-dev`
- Phase 4C firmware: `v2-phase4c-dev`
- Controller: ESP32-S3 QFN56 revision 0.2, 8 MB PSRAM, USB Serial/JTAG
- Hardware MAC: `3c:dc:75:6e:c2:d0`
- Commissioned identity: `paladin` / `pump_001`
- Device name: `Argus Peristaltic Pump V2 - TEST`
- Network: `Paladin6661`; observed STA address `192.168.1.22`

The baseline was clean, synchronized, stationary, zero-output, driver-disabled, `AP_DISCOVERABLE`, and `SUPERVISORY/MQTT` before implementation.

## 3. Audit and Identity-First Commit

The audit found a preliminary hardcoded `argus/peristaltic/...` interface with no lifecycle session, connection-bound lease, replay defense, strict command envelope, application result, or complete authoritative baseline. Existing router, authority, state-manager, network, trajectory, and step-generator ownership was preserved.

Identity was established before functional work:

- `077f8ec` - `chore: establish Phase 4C identity`
- `930abcc` - `chore: correct Phase 4C suite banner`

## 4. Implemented Source Contract

- `313c626` - `feat: implement Phase 4C MQTT supervisory contract`
- `6d55480` - `fix: keep Phase 4C topic table off network task stack`
- `b70f6e9` - `test: cover Phase 4C broker session renewal`
- `0107ad1` - `fix: mark expired supervisor offline on disconnect`
- `ccedf51` - `fix: republish MQTT baseline on reconnect`

`argus_mqtt_contract` owns bounded topic construction, strict JSON decoding, session state, heartbeat lease, serial arithmetic, duplicate/replay decisions, and cached results. `argus_mqtt_runtime` owns the worker queue, production router integration, application results, retained baseline, and 1 Hz publication. `argus_mqtt_broker` supplies bounded metadata, non-reusable connection identity, policy-before-storage/delivery, deterministic duplicate-client rejection, lifecycle callbacks, and 32 retained slots.

Production contains exactly one MQTT router-dispatch call. The MQTT modules contain no direct state mutation, trajectory control, step-generator control, or GPIO control. Legacy command topics are absent from production registration and dispatch.

## 5. Pure and Source Validation

Twenty Phase 4C tests were added inside the complete registered suite without production-singleton mutation. Coverage includes topic bounds and ownership, all command decoders, strict structure and value rules, heartbeat decoding and lease ownership, expiry and disconnect, uint32 wrap arithmetic, first/newer/stale/duplicate/conflict sequence decisions, session invalidation, session formatting, retained capacity, and the topic table stack-size boundary.

| Check | Result |
|---|---|
| Phase 4B embedded JavaScript host checks | PASS; no Phase 4C JavaScript change |
| Strict source/compiler flags | `-Wall -Wextra -Werror` and project strict warnings active |
| ASan/UBSan host execution | Not supported by the available Windows/ESP target toolchain; no runnable host C compiler was available |
| Production MQTT router dispatch sites | Exactly 1 |
| Direct MQTT state/trajectory/step/GPIO mutation | 0 |
| Legacy production command registration | 0 |
| Retained baseline capacity | 25 required topics in 32 slots |
| `git diff --check` | PASS |
| Credential/sensitive-data diff audit | PASS |

## 6. Failure and Correction History

1. The first flashed implementation produced a real `argus_net_mgr` task stack overflow immediately after MQTT topic preparation. Offline analysis identified an approximately 5 KB topic table allocated on the network task stack. The table was moved directly into firmware-lifetime storage, an accessor that copied the complete table was later removed, and the corrected image booted without recurrence.
2. A headless ConPTY harness could not answer linenoise cursor-position requests without fabricating ANSI responses. It was abandoned. Final suites used genuine Windows Terminal plus ESP-IDF monitor logging.
3. Review found that a stale lease connection could disconnect without changing `STALE` to `OFFLINE`. Disconnect now recognizes either the active lease or retained heartbeat connection and clears both safely.
4. Review found that retained state was available on reconnect but not explicitly refreshed by the connection event. `CONNECTED` now queues a bounded baseline republication on the protocol worker.
5. Two parallel full-clean builds ended when unrelated ESP-IDF compiler subprocesses exited silently with no diagnostic. A new full-clean, no-ccache build serialized with `ninja -j 1` completed all 1,108 commands. This was classified as a host parallel-process failure, not a source failure.
6. The first powered acceptance harness evaluated mode before all retained topics arrived. It stopped before heartbeat or command publication. The harness was corrected to require a complete safety snapshot, then the powered run passed.
7. Several Windows Terminal capture attempts produced diagnostic-menu invalid-input messages only. They issued no accepted CLI or motion command and changed no production state. Four complete controller suites ultimately passed; the final three are the acceptance set.

## 7. Final Build and Size

| Item | Result |
|---|---|
| ESP-IDF | v5.5.3 |
| Clean command | `idf.py fullclean` |
| Configure | `idf.py --no-ccache reconfigure` (`CCACHE_ENABLE=0`) |
| Build | `ninja -C build -j 1 all` |
| Build commands | 1,108 plus bootloader |
| Compiler warnings | 0 |
| Compiler errors | 0 |
| Failed build commands | 0 |
| Application binary | `0x10e650` bytes (1,107,536 bytes) |
| Binary SHA-256 | `d45f82b5b3812c6b9bdfe2b6e4be38d5b6179eed2ff4cda8f3cdf17548f84eb4` |
| OTA slot | `0x300000` bytes |
| OTA headroom | `0x1f19b0` bytes (2,038,192 bytes; 65%) |
| DIRAM | 180,583 bytes used; 161,177 bytes free |
| `.bss` / `.data` / DIRAM `.text` | 75,992 / 20,092 / 84,499 bytes |

Against accepted Phase 4B.6, the application binary grew 16,128 bytes. DIRAM grew 26,844 bytes, primarily 26,600 bytes of `.bss` for bounded topic, retained, connection, queue, and session storage. `.data` grew 32 bytes and DIRAM `.text` grew 212 bytes. The increase is deliberate static capacity, not unbounded allocation.

## 8. Flash and Boot

Command: `idf.py -p COM5 flash`

Result: PASS. Bootloader, application, partition table, and OTA data all reported `Hash of data verified`. The final image booted as `v2-phase4c-dev`, reacquired `192.168.1.22`, constructed `argus/paladin/pump_001`, published a new command session, and remained stationary.

## 9. Controller Pure Suites

Final genuine Windows Terminal and `idf.py monitor` acceptance set:

| Invocation | Distinct tests | Internal passes | Executions | Passed | Failed |
|---|---:|---:|---:|---:|---:|
| 1 | 197 | 3 | 591 | 591 | 0 |
| 2 | 197 | 3 | 591 | 591 | 0 |
| 3 | 197 | 3 | 591 | 591 | 0 |
| Aggregate | 197 | 9 | 1,773 | 1,773 | 0 |

Each invocation preserved authority generation 3, `AP_DISCOVERABLE`, MQTT `RUNNING`, machine `UNLOCKED`, and 17 tasks. No panic, reset, watchdog, brownout, assertion, stack-canary failure, heap corruption, task leak, retained-table corruption, or production-state contamination occurred. One additional complete 591/591 invocation also passed during evidence-capture recovery.

## 10. Stationary Live MQTT Acceptance

The final stationary client completed 72 checks:

- Complete 25-topic retained baseline and `v2-phase4c-dev` identity
- `feedback_available=false`; zero configured, trajectory, applied, and generated output
- Controller-owned retained spoof rejected before delivery and storage
- Heartbeat lease `ONLINE`, expiry `STALE`, disconnect `OFFLINE`, and newer-counter rebind
- QoS 0, RETAIN, wrong session, wrong connection, stale sequence, sequence conflict, malformed input, and legacy paths rejected without dispatch
- Exact duplicate returned cached result without redispatch
- Rejected QoS/session/connection attempts did not consume the future valid sequence
- Duplicate simultaneous MQTT client ID rejected while the original connection remained usable
- Final state remained stationary and driver-disabled

The controller was then restarted while stationary. Session `c914e2eb62f4e2da` changed to `c0c571501e5b2448`. An old-session `start` envelope returned `REJECTED/session_mismatch`; a fresh-session zero-target command was accepted. No nonzero target was used in this restart proof.

## 11. Powered Gate Confirmation

Before the first powered command, the operator explicitly approved the powered gate and confirmed the motor was in a safe place and powered tests could proceed. The authorized maximum was 500 mRPM (0.5 output RPM), with no pump head, hose, fluid, chemical, pressure, or process load.

## 12. Powered MQTT Acceptance

The powered harness completed 50 assertions and 12 correlated command results on session `60b6c37040069488`.

### Low-Speed Start

`set_target_rpm_milli=500` and `start=true` were accepted through the production path. State reached `RUNNING`; configured, trajectory, applied, and generated values reached 500 mRPM; generated step count advanced.

### MQTT Software E-Stop

One fresh `e_stop=true` command was accepted. State became `EMERGENCY_STOPPED`, generated and applied output reached zero, and the E-stop remained latched. `reset_e_stop=true` returned through the state manager to stationary `HOLDING`; `unlock=true` returned to `UNLOCKED` with the driver disabled. This is software-level behavior, not a safety-rated physical E-stop.

### Fail-Operational Supervisory Loss

A second 500 mRPM run reached stable `RUNNING`. The supervisory client disconnected without Stop. Link became `OFFLINE`, and after more than the six-second lease timeout the controller remained `RUNNING` with configured, trajectory, applied, and generated values all 500 mRPM. Generated step count continued advancing. No automatic Stop, Unlock, target clear, driver disable, or synthetic command occurred.

A new connection established a fresh heartbeat lease. Retained state truthfully republished the existing `RUNNING` condition without a new Start. A fresh normal Stop produced controlled zero output and `HOLDING`; Unlock produced `UNLOCKED` with the driver disabled.

### Post-Stop Replay

The earlier Start envelope was rejected as `stale_sequence`. Reuse of the latest accepted sequence with changed action/correlation was rejected as `sequence_conflict`. Both produced zero motion. A fresh zero-target command remained usable afterward.

## 13. Final Controller State

Final retained snapshot after the acceptance client disconnected:

- Firmware: `v2-phase4c-dev`
- Command session: `60b6c37040069488`
- Machine: `UNLOCKED`
- Configured / trajectory / applied / generated: `0 / 0 / 0 / 0` mRPM
- E-stop: `CLEAR`
- Fault: `0`
- Driver: `DISABLED`
- Network: `AP_DISCOVERABLE`
- Authority: `SUPERVISORY/MQTT`
- Broker: `RUNNING`
- Supervisor link: `OFFLINE`
- Windows Wi-Fi: connected to `Paladin6661`
- MQTT clients and serial monitor: closed
- COM5: released

## 14. Final Disposition

Phase 4C is software, automated-runtime, protocol, stationary-live, and bounded unloaded-motor accepted. All normal MQTT commands use the existing authority/router/state path; stale and adversarial traffic dispatches zero motion; fail-operational supervisory loss is proven at 500 mRPM; and the controller finishes stationary and stable.

Phase 4D security work remains open. No pump/process acceptance is implied.
