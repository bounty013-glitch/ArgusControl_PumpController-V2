# Phase 5 Master Plan

**Status:** PLANNED - NOT STARTED

## 1. Objective

Phase 5 converts the accepted Phase 4D.4 software baseline into a fully
traceable release decision for one defined controller, motor, pump assembly,
tubing/fluid system, deployment boundary, and operating envelope.

The campaign must answer:

1. What exact product and deployment profile is being released?
2. Does the immutable candidate still satisfy every accepted software contract?
3. Does the real electrical and mechanical assembly behave truthfully across its
   approved envelope?
4. What repeatable open-loop delivery performance does the pump achieve?
5. Does it remain safe and recoverable through credible failures?
6. Are residual security and product risks acceptable for the declared release?
7. Can another engineer reproduce the evidence and identify the exact artifact?

## 2. Non-Negotiable Doctrine

- Controllers control; interfaces and MQTT communicate intent.
- Safety must survive network, display, and supervisory failure.
- Evidence outranks expectation.
- Criteria are frozen before results are observed.
- Build success is not runtime evidence.
- Generated telemetry is not physical feedback.
- A failed safety-critical test stops the campaign.
- Earlier accepted history is never rewritten to make a later candidate pass.
- Phase identity establishment is Step 0 before any Phase 5 code change.

## 3. Baseline

Phase 5 begins from:

- repository: `ArgusControl_PumpController-V2`;
- commit: `31ea4254992f296001d367cece70998659a82783`;
- tag: `v2-phase4d.4-machine-client-auth-accepted`;
- firmware identity: `v2-phase4d.4-dev`; and
- accepted implementation candidate:
  `cea28c2c8476f8f991f8337699e24cb16ea217e8`.

The baseline is software, automated-runtime, browser, security, stationary MQTT,
and earlier bounded unloaded-motion accepted. It is not pump, tubing, fluid,
pressure, flow-accuracy, loaded-torque, process, or endurance accepted.

## 4. Release Claim Decision

Before implementation or physical testing, the owner must select exactly one:

### Profile A - RPM-Controlled Pump

The controller accepts output-shaft RPM commands. Pump output is characterized
for a named pump head, tubing, fluid, temperature, suction condition, discharge
pressure, and tubing life. No direct flow command, measured flow telemetry, or
closed-loop flow claim is made.

### Profile B - Calibrated Flow-Delivery Product

The product accepts or displays calibrated flow as a production feature. This
requires a reviewed production path for calibrated displacement storage,
validation, command conversion, UI/MQTT/API representation, versioning, and
truthful uncertainty. The current baseline does not provide that complete path.
Any implementation starts a Phase 5 implementation microphase and invalidates
downstream evidence until review and regression are repeated.

### Profile C - Evaluation Build Only

The system is released only for controlled field evaluation under trusted-local
network and named operating restrictions. It is not represented as a production
or hostile-network-ready product.

**Selected profile:** `[REQUIRED]`

**Approved release statement:** `[REQUIRED]`

## 5. Phase 5 Gates

| Gate | Purpose | Exit requirement |
|---|---|---|
| 5.0 | Identity and baseline | Clean branch from accepted tag; Phase 5 identity established before functional edits |
| 5.1 | Requirements/profile freeze | Hardware BOM, operating envelope, release claim, numeric criteria, and reviewers approved |
| 5.2 | Source and safety review | Three bounded independent reviews complete; blockers corrected and re-reviewed |
| 5.3 | Release-candidate build | ESP-IDF v5.5.3 full-clean no-ccache build, zero warnings/errors, size and hashes recorded |
| 5.4 | Stationary and unloaded regression | Boot, pure suites, interfaces, authority, router, security, and unloaded motion pass |
| 5.5 | Pump assembly and calibration | Safe-fluid delivery, direction, repeatability, accuracy/characterization, pressure envelope, and stops pass |
| 5.6 | Fault and reliability | Network loss, restart/power loss, fault response, thermal, cycle, and endurance campaign pass |
| 5.7 | Deployment/security decision | Deferred register reviewed; audit and deployment restrictions resolved or release blocked |
| 5.8 | Final release | Evidence reconciled, independent final review complete, release commit merged and annotated tag pushed |

No gate may be skipped because a later gate appears to cover similar behavior.

## 6. Review Model

Before the first flash of any changed Phase 5 firmware:

1. **Architecture/doctrine review:** layering, authority, state, truthfulness,
   persistence, compatibility, and test isolation.
2. **Motion/process safety review:** electrical limits, physical protections,
   test envelope, stop behavior, fluid compatibility, pressure, and operator
   procedure.
3. **Security/release review:** route and MQTT boundaries, secrets, deployment
   assumptions, deferred register, artifact provenance, and release claims.

Where practical, reviewers must be independent of the author. Every finding gets
one of: corrected, rejected with evidence, deferred with an explicit release
boundary, or release-blocking.

## 7. Change Control

If source, tests, build configuration, partitioning, embedded assets, or release
identity changes:

- create a normal follow-up commit; never amend accepted history;
- perform source review before flashing;
- rebuild full-clean;
- repeat affected automated and physical gates based on documented impact;
- assign a new release-candidate commit; and
- never combine evidence from different candidates as though it were one run.

A documentation-only correction may preserve runtime evidence only when it does
not alter test instructions, acceptance criteria, or the identity of the tested
artifact.

## 8. Stop Conditions

Stop immediately for:

- unexpected motion, wrong direction, or failure to stop;
- unguarded moving parts or unreachable physical disconnect;
- leak, tube rupture, uncontrolled discharge, pressure excursion, or cavitation;
- abnormal sound, vibration, stall, step loss, driver fault, heat, odor, or smoke;
- panic, reset, watchdog, brownout, assertion, stack-canary failure, heap
  corruption, or task leak;
- dishonest or contradictory UI/MQTT/serial/physical state;
- unknown firmware, hardware, tubing, fluid, or instrument identity;
- missing or expired instrument calibration;
- evidence contamination, missing raw data, or post-hoc criteria; or
- any safety-critical test failure.

Resume only after root cause, correction, review, and the required regression
scope are documented.

## 9. Final Outcomes

Exactly one final outcome is permitted:

- `ACCEPTED` - every required gate passed for the exact declared profile;
- `CONDITIONALLY ACCEPTED` - only when every condition is explicit, bounded,
  approved, and does not hide a safety or doctrine violation;
- `REJECTED` - a tested requirement failed; or
- `BLOCKED` - required evidence, equipment, decision, or safe test condition is
  unavailable.

No merge or acceptance tag is created for `REJECTED` or `BLOCKED`.
