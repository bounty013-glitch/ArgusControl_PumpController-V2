# Phase 4B.4 - Physical Test and Evidence Record

**Status:** PENDING - TEMPLATE ESTABLISHED DURING STEP 0
**Firmware identity:** `v2-phase4b.4-dev`
**Candidate commit:** `[PENDING]`
**Acceptance date:** `[PENDING]`

## Evidence boundary

This document is an unexecuted template. The Step 0 identity-only build is recorded in the Phase 4B.4 implementation plan, but no Phase 4B.4 functional implementation, runtime test result, physical result, or acceptance is claimed. Populate candidate identity and results only after source review authorizes hardware execution.

For every test, record the initial condition, operator action, relevant serial/dashboard transition, actual outcome, PASS/FAIL determination, and screenshot filename when one materially adds evidence.

## Preflight pure suite

**Result:** `[PENDING]`

Record distinct tests, repeat passes, total executions, passed, failed, and before/after production-isolation state. Step 1 registers 151 distinct tests for 453 expected executions across three passes. These are source-derived counts, not runtime results, and may increase during later Phase 4B.4 steps.

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
