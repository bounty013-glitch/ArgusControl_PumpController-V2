# Phase 4B.4 - Physical Test and Evidence Record

**Status:** STEP 2 AUTOMATED CONTROLLER RUNTIME PASSED - PHYSICAL TESTS PENDING
**Firmware identity:** `v2-phase4b.4-dev`
**Candidate commit:** `1b701e5` (Step 2 corrected software candidate; not physically accepted)
**Acceptance date:** `[PENDING]`

## Evidence boundary

This document remains the physical-test template. Phase 4B.4 Step 2 controller-only automated runtime is recorded below, but the motor remained disconnected and no command-API physical result, motor result, pump result, or physical acceptance is claimed.

For every test, record the initial condition, operator action, relevant serial/dashboard transition, actual outcome, PASS/FAIL determination, and screenshot filename when one materially adds evidence.

## Preflight pure suite

**Result:** `PASS - CONTROLLER-ONLY AUTOMATED RUNTIME`

Three final genuine interactive diagnostic-option `t` invocations of the corrected candidate each reported 163 distinct tests, three repeat passes, 489 executions, 489 passed, and 0 failed, for 1,467/1,467 passing outcomes. The first invocation preserved authority generation 3, network `AP_DISCOVERABLE`, broker `STOPPED`, machine `UNLOCKED`, and 15 tasks. The second and third preserved generation 3, `AP_DISCOVERABLE`, broker `RUNNING`, `UNLOCKED`, and 16 tasks. Each invocation preserved the production state present when it began and returned normally to the diagnostic prompt. No panic, post-input reset, watchdog, brownout, assertion, stack-canary, heap-corruption, or task-leak evidence appeared.

## Command API physical tests

| Test | Required behavior | Result | Evidence |
|---|---|---|---|
| 1 | Reject command outside `SERVICE_AP_ONLY` | `[PENDING]` | `[PENDING]` |
| 2 | Reject command without `LOCAL_SERVICE/BROWSER` authority | `[PENDING]` | `[PENDING]` |
| 3 | Apply a valid setpoint through the router | `[PENDING]` | `[PENDING]` |
| 4 | Start through the router | `[PENDING]` | `[PENDING]` |
| 5 | Perform a normal stop through the router | `[PENDING]` | `[PENDING]` |
| 6 | Unlock through the router | `[PENDING]` | `[PENDING]` |
| 7 | Latch E-stop through the router | `[PENDING]` | `[PENDING]` |
| 8 | Reset E-stop only from an eligible state | `[PENDING]` | `[PENDING]` |
| 9 | Execute recovery through the router | `[PENDING]` | `[PENDING]` |
| 10 | Reject malformed, ambiguous, unsupported, and out-of-range JSON without motion | `[PENDING]` | `[PENDING]` |
| 11 | Reject a stale authority generation without motion | `[PENDING]` | `[PENDING]` |
| 12 | Preserve HTTP, network, broker, and authority truth throughout the sequence | `[PENDING]` | `[PENDING]` |

## Final pure suite and isolation proof

**Result:** `[PENDING]`

Repeat the complete pure suite after physical command testing and prove that authority generation, network state, broker state, machine state, and task count are unchanged across that suite execution.

## Final disposition

**Phase 4B.4 physical acceptance:** `[PENDING]`
