# Phase 5 - Final Acceptance and Live Testing

**Planning status:** READY FOR REVIEW

**Execution status:** NOT STARTED

**Accepted input baseline:** `31ea4254992f296001d367cece70998659a82783`

**Accepted baseline tag:** `v2-phase4d.4-machine-client-auth-accepted`

## Purpose

This folder is the authoritative execution package for Phase 5. It converts the
accepted Phase 4D.4 controller into a controlled release candidate and closes,
or truthfully carries forward, every physical, process, reliability, security,
documentation, and release-management gate.

Phase 5 is not permission to begin testing immediately. The release profile,
numeric acceptance limits, equipment, safety controls, reviewers, and evidence
locations must be completed and approved first.

## Required Reading Order

1. [00_REPOSITORY_READ_IN_AND_GAP_ANALYSIS.md](00_REPOSITORY_READ_IN_AND_GAP_ANALYSIS.md)
2. [01_PHASE_5_MASTER_PLAN.md](01_PHASE_5_MASTER_PLAN.md)
3. [02_BASELINE_AND_RELEASE_PROFILE.md](02_BASELINE_AND_RELEASE_PROFILE.md)
4. [03_SAFETY_SETUP_AND_HARD_STOPS.md](03_SAFETY_SETUP_AND_HARD_STOPS.md)
5. [04_CALIBRATION_AND_PERFORMANCE_PLAN.md](04_CALIBRATION_AND_PERFORMANCE_PLAN.md)
6. [05_LIVE_ACCEPTANCE_PROCEDURES.md](05_LIVE_ACCEPTANCE_PROCEDURES.md)
7. [06_RELIABILITY_RECOVERY_AND_FAULT_PLAN.md](06_RELIABILITY_RECOVERY_AND_FAULT_PLAN.md)
8. [07_SECURITY_AND_DEFERRED_RISK_GATE.md](07_SECURITY_AND_DEFERRED_RISK_GATE.md)
9. [08_EVIDENCE_AND_TRACEABILITY.md](08_EVIDENCE_AND_TRACEABILITY.md)
10. [09_RELEASE_DOCUMENTATION_AND_HANDOFF_PLAN.md](09_RELEASE_DOCUMENTATION_AND_HANDOFF_PLAN.md)
11. [10_FINAL_RELEASE_CHECKLIST.md](10_FINAL_RELEASE_CHECKLIST.md)
12. [11_FINAL_ACCEPTANCE_RECORD.md](11_FINAL_ACCEPTANCE_RECORD.md)

## Authority of Documents

- Existing doctrine remains controlling.
- The accepted Phase 4D.4 source is the starting baseline, not Phase 5 evidence.
- This folder defines future work and contains no claim that Phase 5 has passed.
- Numeric limits marked `[REQUIRED]` must be approved before the related test.
- A blank, assumed, inferred, or post-hoc criterion is not a passing criterion.
- Where this package conflicts with a manufacturer limit, approved engineering
  specification, or physical safety requirement, the safer limit controls and
  the conflict must be recorded.

## Release Boundary

The controller is open-loop:

- generated pulses are not proof of shaft motion;
- shaft motion is not proof of pumped volume;
- pumped volume is not proof of pressure or process delivery;
- HTTP/MQTT acceptance is not proof of physical action; and
- the software E-stop is not a safety-rated physical E-stop.

Phase 5 may close only the release profile that is explicitly approved in
`02_BASELINE_AND_RELEASE_PROFILE.md`. Anything outside that profile remains
unaccepted.
