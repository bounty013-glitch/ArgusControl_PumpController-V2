# Repository Read-In and Gap Analysis

**Audit date:** July 25, 2026

**Audit mode:** Documentation planning only; no firmware or test source changed

## 1. Material Reviewed

The Phase 5 plan was derived from:

- all eight active doctrine documents;
- README, V2 baseline audit, architecture, and implementation plan;
- Phase 3B motion/state acceptance;
- Phase 4A network/authority plans and runtime evidence;
- Phase 4B browser, configuration, recovery, UI, and physical records;
- Phase 4C MQTT contract, plan, and powered acceptance;
- Phase 4D security contract, subphase plans, and acceptance records;
- the full deferred-hardening register;
- active motor configuration, conversion, feedback, state, trajectory,
  step-generator, authority, browser, MQTT, security, and test source;
- the UIM344 V5.11 manual and repository HMI warning/reference; and
- current branch, tags, accepted commit history, build identity, and repository
  status.

The archived Arduino HMI is explicitly obsolete and is not a release client for
this controller.

## 2. Accepted Foundation

The accepted Phase 4D.4 main/tag establishes:

- exact fixed-point active-low STEP generation;
- verified common-anode wiring;
- deterministic trajectory and state management;
- one command router and explicit authority ownership;
- durable commissioning, network recovery, and portal lifecycle;
- authenticated local browser sessions and security administration;
- strict Phase 4C MQTT session/freshness/result contract;
- machine-client enrollment, authentication, per-packet policy, and immediate
  credential invalidation; and
- extensive pure-suite and live stationary evidence.

Earlier accepted powered work established unloaded motor motion and bounded
low-speed browser/MQTT control. Those results are regression baselines, not proof
of the final pump/process profile.

## 3. Existing Phase 5 Objective

The only explicit original Phase 5 plan was:

- tune trajectory acceleration;
- verify displacement scaling through physical validation; and
- finalize retained discovery configuration topics.

That outline is no longer sufficient. Phases 4A through 4D expanded the product
to include commissioning, local browser control, MQTT supervision, recovery,
human/machine security, and release-significant deployment assumptions. Final
acceptance must cover the integrated system and every accumulated exclusion.

## 4. Accumulated Unaccepted Areas

Prior records consistently excluded:

- pump head and coupling;
- tubing/hose and fittings;
- fluid and chemical compatibility;
- pressure and relief behavior;
- measured flow and flow accuracy;
- calibrated displacement;
- loaded torque and step-loss behavior;
- process interaction;
- thermal performance;
- long-duration operation and mechanical/tubing endurance; and
- safety-rated physical E-stop behavior.

Phase 5 is the first place these may be accepted, and only for an explicitly
defined physical and deployment profile.

## 5. Source-to-Architecture Gaps

### 5.1 Flow and Displacement

The architecture diagram names `argus_displacement` and says product
configuration persists commissioning calibration. Current source does not
contain that module or a complete calibration persistence path.

Current source provides:

- `argus_conversions_flow_to_rpm()` fixed-point math;
- provisional Kconfig text `0.04 gal/rev`; and
- production commands in milli-RPM.

The provisional displacement is not an accepted calibration, and no production
flow command, browser/MQTT flow contract, measured-flow telemetry, or closed-loop
flow behavior exists.

**Phase 5 consequence:** freeze an RPM-controlled characterization claim or
implement and independently accept a real flow-delivery contract before claiming
flow control.

### 5.2 Physical Feedback

`argus_feedback` returns unavailable and `ESP_ERR_NOT_SUPPORTED`. There is no
accepted encoder, flow, pressure, temperature, current, or driver-fault input.

**Phase 5 consequence:** use independent calibrated instruments. Do not label
generated output as actual physical performance.

### 5.3 Operating Envelope

The hardware configuration identifies 0.5 through 200 output RPM as its nominal
range with 10 RPM/s ramps. The production state manager accepts a zero target and
nonzero targets below 0.5 RPM, while the pulse generator supports rates down to
1 mRPM so trajectories can move smoothly through that region. Earlier evidence
includes unloaded operation, including historical 200 RPM testing, but the repo
does not define whether externally requested 1-499 mRPM targets are a supported
product feature. It also contains no accepted pump-head/tubing/fluid/pressure/
duty specification proving that 200 RPM is a valid loaded release limit.

**Phase 5 consequence:** explicitly decide the external minimum command policy.
The release profile, manufacturer limits, and measured behavior define the
physical envelope. The firmware maximum is only an upper software bound.

### 5.4 Driver Observability

The UIM344 manual describes internal missing-step compensation/lock and
electrical/thermal protection. The controller has no accepted direct input for
those conditions.

**Phase 5 consequence:** observe driver behavior independently and document any
unobservable fault as a release limitation. Do not fabricate controller alarms.

### 5.5 Diagnostic Hardware Menu

The diagnostic hardware submenu calls state-manager APIs directly and includes a
200 RPM selection. It is an accepted diagnostic lower-layer path, not the normal
browser/MQTT production pipeline.

**Phase 5 consequence:** production end-to-end acceptance uses browser and MQTT.
Diagnostic tests may supplement timing/mechanics but cannot prove production
authorization/routing or justify an unsafe speed.

## 6. Security and Deployment Gaps

Phase 4D.4 closes machine authentication but does not close plaintext transport
or hostile-network deployment.

Release-significant open items include:

- DHR-002 plaintext browser transport;
- DHR-003 physical extraction of software-stored keys;
- DHR-009 comprehensive security audit required before production release;
- DHR-011 always-available Service AP production policy;
- DHR-015 bounded flat HTTP JSON parser debt;
- DHR-016 MQTT transport and adversarial hardening;
- DHR-017 certificates/hostile-network operation; and
- DHR-018 residual credential, DoS, audit-capacity, time, and device-protection
  risks.

The DHR-018 narrative predates Phase 4D.4 and still lists machine authentication
as open. Phase 5 must reconcile that historical text while preserving the
remaining risks.

**Phase 5 consequence:** production acceptance requires DHR-009 closure and a
complete DHR disposition. Otherwise the honest outcome is a controlled
evaluation release or blocked production release.

## 7. Fail-Operational Process Question

Phase 4C intentionally continues motion when MQTT/heartbeat communication is
lost. This is correct controller doctrine for fail-operational supervision, but
its process safety depends on the final installation.

**Phase 5 consequence:** prove continued pumping is safe within the declared
process envelope or require an independent physical/controller interlock. Do not
silently change the accepted behavior and do not claim safe process operation
without resolving it.

## 8. Required Phase 5 Decisions

Before any new implementation or loaded test:

1. Choose RPM-controlled, flow-delivery, or evaluation-only release.
2. Freeze the complete hardware/BOM and driver configuration.
3. Freeze fluid, tubing, pressure, flow, temperature, duty, and service-life
   limits.
4. Freeze numeric performance and endurance criteria.
5. Decide the commissioned-device Service AP production policy.
6. Decide whether DHR-009 will be completed for production or the release remains
   controlled evaluation.
7. Define release semantic version, upgrade path, artifact naming, and support
   boundary.
8. Assign test, safety, evidence, review, and acceptance roles.

## 9. Planning Conclusion

The accepted controller is a strong software and unloaded-motion baseline. The
remaining uncertainty is not primarily another command parser or UI feature; it
is the exact physical product claim and the evidence needed to support it.

Phase 5 is therefore structured as a release qualification program with explicit
decision gates, not as an assumption that connecting a pump and observing flow
completes the project.
