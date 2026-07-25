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

Future Phase 5 execution branches begin from:

- repository: `ArgusControl_PumpController-V2`;
- planning-inclusive branch point:
  `6d907a11e294fde26a33fbb60b898839883e1490`;
- accepted firmware/source baseline:
  `31ea4254992f296001d367cece70998659a82783`;
- tag: `v2-phase4d.4-machine-client-auth-accepted`;
- firmware identity: `v2-phase4d.4-dev`; and
- accepted implementation candidate:
  `cea28c2c8476f8f991f8337699e24cb16ea217e8`.

The baseline is software, automated-runtime, browser, security, stationary MQTT,
and earlier bounded unloaded-motion accepted. It is not pump, tubing, fluid,
pressure, flow-accuracy, loaded-torque, process, or endurance accepted.

## 4. Control Claim and Release Classification

The control claim is fixed:

**Control claim:** `RPM-CONTROLLED`

The controller accepts output-shaft RPM commands. Pump output may be
characterized for a named pump head, tubing, fluid, temperature, suction
condition, discharge pressure, and tube life. No direct flow command, measured
flow telemetry, commanded-flow behavior, or closed-loop flow claim is made.

**Initial release classification:** `CONTROLLED EVALUATION`

After both gates are evaluated, the final release classification is exactly one
of `PRODUCTION`, `CONTROLLED EVALUATION`, or `BLOCKED`.

## 5. Phase 5 Gates

| Gate | Purpose | Exit requirement |
|---|---|---|
| 5.0 | Identity and baseline | Clean branch from planning-inclusive commit; accepted firmware/source baseline separately verified; Phase 5 identity established before functional edits |
| 5.1 | Requirements/profile freeze | Hardware BOM, operating envelope, release claim, numeric criteria, and reviewers approved |
| 5.2 | Source and safety review | Three bounded independent reviews complete; blockers corrected and re-reviewed |
| 5.3 | Release-candidate build | ESP-IDF v5.5.3 full-clean no-ccache build, zero warnings/errors, size and hashes recorded |
| 5.4 | Stationary and unloaded regression | Boot, pure suites, interfaces, authority, router, security, and unloaded motion pass |
| 5.5 | Pump assembly and calibration | Safe-fluid delivery, direction, repeatability, accuracy/characterization, pressure envelope, and stops pass |
| 5.6 | Fault and reliability | Network loss, restart/power loss, fault response, thermal, cycle, and endurance campaign pass |
| 5.7 | Deployment/security decision | Deferred register reviewed; audit and deployment restrictions resolved or release blocked |
| 5.8A | Engineering acceptance | Gate A evidence reconciled and loaded-system limits accepted |
| 5.8B | Production readiness | Gate B security, manufacturing, documentation, training, support, customer, HAZLOC, and chemical restrictions resolved |
| 5.9 | Final classification | `PRODUCTION`, `CONTROLLED EVALUATION`, or `BLOCKED` recorded without conflating Gate A and Gate B |

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

## 9. Two Independent Gates

**Gate A - Phase 5 engineering acceptance** covers the loaded
controller/driver/motor/gearbox/pump/tube assembly, trajectory, independently
measured RPM, displacement and flow characterization, pressure within the
approved fixture, Stop/software E-stop, authority and communications loss,
restart/persistence/recovery, thermal/endurance/tube aging, and validated limits.

**Gate B - production readiness** covers security and DHR disposition,
manufacturing/provisioning, installation, operator and maintenance instructions,
training, support, customer-use restrictions, HAZLOC restrictions, and chemical
restrictions.

Gate A may be `ACCEPTED` while Gate B remains `OPEN`. This permits only the
truthful final classification `CONTROLLED EVALUATION`, never unrestricted
production. The final release classification is `PRODUCTION`, `CONTROLLED
EVALUATION`, or `BLOCKED`.
