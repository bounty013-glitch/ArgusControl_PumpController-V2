# Phase 5 - Final Acceptance and Live Testing

**Planning status:** READY FOR REVIEW

**Execution status:** NOT STARTED

**Phase 5 planning-inclusive branch point:** `6d907a11e294fde26a33fbb60b898839883e1490`

**Accepted firmware/source baseline:** `31ea4254992f296001d367cece70998659a82783`

**Accepted baseline tag:** `v2-phase4d.4-machine-client-auth-accepted`

## Purpose

This folder is the authoritative execution package for Phase 5. Future Phase 5
branches start from the planning-inclusive commit above. The accepted firmware
and source entering Phase 5 remain the separately identified Phase 4D.4 commit.

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
- Every physical criterion comes from one approved revision of the frozen test
  profile in `02_BASELINE_AND_RELEASE_PROFILE.md`.
- An unknown owner-supplied value is marked exactly
  `REQUIRES SHAWN CONFIRMATION`; the related test must not execute until it is
  confirmed.
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

The control claim is fixed as `RPM-CONTROLLED`. Phase 5 characterizes physical
delivery under named conditions; it does not create closed-loop or commanded
flow control.

## Two-Gate Acceptance Model

- **Gate A - Phase 5 engineering acceptance:** loaded controller, driver, motor,
  gearbox, pump, and tube performance; trajectory; independently measured RPM;
  displacement/flow characterization; pressure within the approved fixture;
  stop and software E-stop; authority/communications loss; restart, persistence,
  recovery; thermal/endurance/tube aging; and validated limits.
- **Gate B - production readiness:** security/DHR closure, manufacturing and
  provisioning, installation, operator and maintenance documents, training,
  support, customer-use restrictions, HAZLOC restrictions, and chemical-use
  restrictions.

Gate A may pass while Gate B remains open. Gate A acceptance never means
unrestricted production readiness. The initial release classification is
`CONTROLLED EVALUATION`; the final classification may be `PRODUCTION`,
`CONTROLLED EVALUATION`, or `BLOCKED`.
