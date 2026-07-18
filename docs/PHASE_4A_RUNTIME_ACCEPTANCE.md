# Phase 4A Runtime Acceptance Evidence

## Source Identification

- **Branch:** `phase4a-hardening-audit`
- **HEAD at flash:** `c68ff24`
- **App version:** `v2-phase4a-dev`
- **ESP-IDF:** v5.5.3
- **Target:** ESP32-S3

## Verification Source

All runtime evidence was supplied by the operator via physical serial
monitor observation. No runtime results were inferred, simulated, or
claimed by tooling.

---

## 1. Pure Non-Motion Unit Tests

**Status: PHYSICALLY EXECUTED -- PASSED**

### Phase 3B Pure Tests

| Item                  | Result  |
|-----------------------|---------|
| Strict Command Parser | PASSED  |
| State Core Permissions| PASSED  |
| Error Propagation     | PASSED  |
| Singleton Isolation   | PASSED  |

### Phase 4A Pure Tests

| Item                  | Value   |
|-----------------------|---------|
| Distinct test cases   | 18      |
| Repeat passes         | 3       |
| Total executions      | 54      |
| Passed executions     | 54      |
| Failed executions     | 0       |

### Production Isolation (Read-Only Proof)

| Observable              | Result              |
|-------------------------|---------------------|
| Authority Generation    | UNCHANGED (Gen 2)   |
| Network State           | UNCHANGED (COMMISSIONED_STA) |
| MQTT Broker State       | UNCHANGED (RUNNING) |
| Machine State           | UNCHANGED (UNLOCKED)|
| Task Count              | UNCHANGED (14)      |

No panic, watchdog event, unintended reboot, or production-state
mutation was observed during any test execution.

---

## 2. AP Discoverability (Scenario Step 2)

**Status: PHYSICALLY VERIFIED -- PASSED**

Starting conditions: COMMISSIONED_STA, SUPERVISORY/MQTT, UNLOCKED.

Menu path: `N` -> `4` (enable service AP) -> `2` (snapshot).

| Observable              | Expected            | Observed            |
|-------------------------|---------------------|---------------------|
| Network Mode            | AP_DISCOVERABLE     | AP_DISCOVERABLE (3) |
| Wi-Fi Driver Mode       | APSTA               | APSTA               |
| STA Status              | CONNECTED w/ IP     | CONNECTED (IP Acquired) |
| Service AP Status       | ENABLED             | ENABLED             |
| MQTT Broker Status      | READY or RUNNING    | READY               |
| Authority Mode          | SUPERVISORY         | SUPERVISORY (1)     |
| Authority Owner         | MQTT                | MQTT (1)            |

DHCP server started on AP interface (192.168.4.1). No authority transfer
occurred from phone connection to Service AP.

---

## 3. Stopped-State Service Entry (Scenario Step 3)

**Status: PHYSICALLY VERIFIED -- PASSED**

Menu path: `5` (request LOCAL_SERVICE as diagnostic CLI).

### Authority Transition Sequence

| Step | Transition                                        |
|------|---------------------------------------------------|
| 1    | SUPERVISORY/MQTT -> SERVICE_TRANSITION/NONE (gen 3) |
| 2    | Controlled normal stop requested                  |
| 3    | Stopped state verified                            |
| 4    | MQTT broker shut down cleanly                     |
| 5    | MQTT server task exited cleanly                   |
| 6    | STA disconnected (run -> init)                    |
| 7    | STA IP released                                   |
| 8    | Wi-Fi changed from APSTA to softAP only           |
| 9    | AP-only state verified                            |
| 10   | Machine safety revalidated                        |
| 11   | SERVICE_TRANSITION/NONE -> LOCAL_SERVICE/DIAGNOSTIC_CLI (gen 4) |

### Absence of Failure Conditions

- No deadlock
- No timeout
- No panic
- No watchdog event
- No unintended reboot
- No broker lifecycle errors
- No duplicate broker tasks
- No authority-generation rejection
- Service AP remained active throughout

---

## 4. Local Service State Verification (Scenario Step 4)

**Status: PHYSICALLY VERIFIED -- PASSED**

| Observable              | Expected            | Observed              |
|-------------------------|---------------------|-----------------------|
| Network Mode            | SERVICE_AP_ONLY     | SERVICE_AP_ONLY (5)   |
| Wi-Fi Driver Mode       | AP                  | AP                    |
| STA Status              | DISCONNECTED        | DISABLED              |
| Service AP Status       | ENABLED             | ENABLED               |
| MQTT Broker Status      | STOPPED             | STOPPED               |
| Authority Mode          | LOCAL_SERVICE       | LOCAL_SERVICE (3)     |
| Authority Owner         | DIAGNOSTIC_CLI      | DIAGNOSTIC_CLI (3)    |
| Authority Generation    | 4                   | 4                     |

---

## 5. Non-Owner Rejection (Scenario Step 5)

**Status: PHYSICALLY VERIFIED -- PASSED**

Authority during test: LOCAL_SERVICE/DIAGNOSTIC_CLI (gen 4).

| Probe                | Result                        | State Invariance       |
|----------------------|-------------------------------|------------------------|
| MQTT-source (option 6)   | REJECTED (ESP_ERR_INVALID_STATE) | PASSED (15/15 fields) |
| Browser-source (option 7)| REJECTED (ESP_ERR_INVALID_STATE) | PASSED (15/15 fields) |

Neither probe started motion, changed the setpoint, changed authority,
changed network state, or mutated any production state.

---

## 6. Service Exit and Reboot (Scenario Step 6)

**Status: PHYSICALLY VERIFIED -- PASSED**

Menu path: `X` -> `y` (confirm exit).

### Exit Transition Sequence

| Step | Transition                                            |
|------|-------------------------------------------------------|
| 1    | Ownership validated: LOCAL_SERVICE/DIAGNOSTIC_CLI     |
| 2    | SERVICE_AP_ONLY -> SERVICE_TRANSITION                 |
| 3    | LOCAL_SERVICE/DIAGNOSTIC_CLI -> SERVICE_TRANSITION/NONE (gen 5) |
| 4    | Controlled stop requested and verified                |
| 5    | SERVICE_TRANSITION/NONE -> NONE/NONE (gen 6)          |
| 6    | Intentional reboot (RTC_SW_CPU_RST)                   |

---

## 7. Post-Reboot Recovery (Scenario Step 7)

**Status: PHYSICALLY VERIFIED -- PASSED**

| Observable              | Expected            | Observed              |
|-------------------------|---------------------|-----------------------|
| Reset reason            | RTC_SW_CPU_RST      | RTC_SW_CPU_RST        |
| NVS configuration       | VALID, COMMISSIONED | Slot A: gen=2, valid=YES, Commissioned: YES |
| Network Mode            | COMMISSIONED_STA    | COMMISSIONED_STA      |
| STA Status              | CONNECTED w/ IP     | Connected (192.168.50.236) |
| MQTT Broker Status      | RUNNING             | Listening on port 1883|
| Authority Mode          | SUPERVISORY         | SUPERVISORY           |
| Authority Owner         | MQTT                | MQTT                  |
| Authority Generation    | 2                   | 2 (fresh boot)        |
| Machine State           | UNLOCKED            | UNLOCKED              |

### Post-Reboot Health

- Exactly one MQTT broker server task active
- No repeated broker lifecycle errors
- No watchdog, panic, or unintended reset
- Periodic status logging: OFF
- Diagnostic menu rendered correctly

---

## Phase 4A Completion Status

Phase 4A: COMPLETE WITHIN REVISED SCOPE

All stopped-state acceptance scenarios have been physically verified
by the operator as documented in the sections above.

### Deferred by Design

| Scenario | Placement | Reason |
|----------|-----------|--------|
| Active-motion MQTT/HMI-to-local authority handoff | DEFERRED BY DESIGN TO PHASE 4D END-TO-END ACCEPTANCE | Requires production-intent HMI and Phase 4C MQTT contract. Testing with a temporary MQTT utility would not prove the intended production architecture. |
| Concurrent service-transition E-stop preemption | DEFERRED -- PLACEMENT PENDING OPERATOR DECISION (Phase 4B or Phase 4D) | Requires ISR-level mutex splitting, atomic latches, and hardware timing measurement. Orthogonal to browser portal scope. |

### Active-Motion Handoff Deferred Test Requirements

When exercised during Phase 4D end-to-end acceptance, the test must verify:

1. The production HMI holds SUPERVISORY/MQTT authority.
2. The HMI commands active motor motion.
3. Local service entry is requested.
4. MQTT supervisory authority is revoked.
5. The Controller performs a controlled deceleration.
6. The Controller confirms a safe stopped state.
7. MQTT and STA supervisory paths are shut down.
8. Local browser service authority is granted last.
9. No stale, queued, replayed, or reconnecting HMI command restarts motion.
10. The Controller remains independently operable without the HMI.

- Owner: HMI/Controller integration acceptance
- Trigger: Production-intent HMI and Phase 4C MQTT contract available
- Required evidence: Controller log, HMI command/status evidence,
  authority transitions, motion-state transitions, post-transition
  restart-inhibition evidence
- Final gate: Must pass before v2-phase4-complete
