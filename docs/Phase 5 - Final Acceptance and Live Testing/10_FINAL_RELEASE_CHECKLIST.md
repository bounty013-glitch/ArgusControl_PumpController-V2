# Final Release Checklist

**Status:** ALL ITEMS PENDING

Mark each item `PASS`, `FAIL`, `BLOCKED`, or `N/A - APPROVED`. Every `N/A`
requires written rationale and acceptance-authority approval.

## A. Baseline and Scope

- [ ] Accepted Phase 4D.4 tag and commit verified locally and remotely.
- [ ] Phase 5 branch starts from planning-inclusive commit
      `6d907a11e294fde26a33fbb60b898839883e1490`.
- [ ] Accepted firmware/source baseline
      `31ea4254992f296001d367cece70998659a82783` separately verified.
- [ ] Phase 5 Step 0 identity established before functional edits.
- [ ] Exact release-candidate commit frozen.
- [ ] Control claim remains `RPM-CONTROLLED`.
- [ ] Initial release classification is `CONTROLLED EVALUATION`.
- [ ] Supported and prohibited claims written explicitly.
- [ ] Hardware BOM and configuration complete.
- [ ] Operating, process, network, and security envelopes complete.
- [ ] Numeric acceptance criteria frozen before test results.
- [ ] No unexplained working-tree or generated artifacts.

## B. Review

- [ ] Architecture/doctrine review complete.
- [ ] Motion/process safety review complete.
- [ ] Security/release review complete.
- [ ] All findings corrected, evidence-rejected, bounded/deferred, or blocking.
- [ ] Corrections received independent re-review.
- [ ] Final source registration and direct-call audits complete.
- [ ] Documentation/source truthfulness discrepancies resolved.

## C. Build and Artifact

- [ ] ESP-IDF v5.5.3 verified.
- [ ] Full-clean, no-ccache release build complete.
- [ ] Compiler warnings: zero.
- [ ] Compiler errors: zero.
- [ ] `git diff --check` passes.
- [ ] Embedded JavaScript syntax/tests pass.
- [ ] Binary size, static RAM, stack budgets, and OTA headroom accepted.
- [ ] ELF/MAP/BIN/partition/OTA artifacts hashed and archived.
- [ ] Build/configuration contains no credentials.
- [ ] Release artifact boots with exactly the approved identity.

## D. Stationary and Automated Runtime

- [ ] Intended ESP32-S3 and hardware UID verified.
- [ ] Flash and boot log complete.
- [ ] Stationary boot state truthful and safe.
- [ ] Three complete pure-suite invocations pass.
- [ ] Distinct/execution counts reconciled.
- [ ] Production state and task count unchanged by pure suites.
- [ ] No panic/reset/watchdog/brownout/assertion/stack/heap/task failure.
- [ ] Browser and MQTT stationary regressions pass.
- [ ] Authentication/authorization does not grant motion authority.

## E. Electrical and Mechanical

- [ ] Wiring/polarity/common-anode inspection passes.
- [ ] Driver current, idle current, microsteps, and missing-step threshold recorded.
- [ ] STEP pulse width, idle level, and frequency pass.
- [ ] DIR setup/hold and physical direction pass.
- [ ] ENA boot and enable-before-motion behavior pass.
- [ ] Setpoint-only isolation passes.
- [ ] Start/ramp across approved unloaded range passes.
- [ ] Normal Stop/HOLDING passes.
- [ ] Unlock/driver-disable passes.
- [ ] Direction reversal through zero passes where approved.
- [ ] Software E-stop/latch/rejection/reset passes.
- [ ] Recover ends stationary and unlocked.

## F. Pump, Fluid, and Performance

- [ ] Pump/tube/fluid/pressure profile approved.
- [ ] Safe-fluid prime and zero/minimum-pressure delivery pass.
- [ ] No leak, tube walk, cavitation, uncontrolled siphon, or abnormal vibration.
- [ ] Calibration matrix complete with raw data.
- [ ] Independent shaft and volume/flow measurement used.
- [ ] Accuracy/characterization criteria pass.
- [ ] Repeatability criteria pass.
- [ ] Pressure sensitivity criteria pass.
- [ ] Direction difference criteria pass.
- [ ] Warm-up and tubing-age drift criteria pass.
- [ ] Startup delay and residual delivery criteria pass.
- [ ] Pressure/load envelope and relief behavior pass.
- [ ] Chemical/process qualification complete or explicitly excluded.

## G. Reliability and Failure

- [ ] Thermal/electrical soak duration and criteria pass.
- [ ] Start/stop cycle campaign passes.
- [ ] Direction/recovery cycle campaign passes or approved N/A.
- [ ] Wi-Fi and MQTT loss/reconnect behavior passes.
- [ ] Fail-operational continuation is safe for declared process or independently
      protected.
- [ ] Controlled reboot and cold power cycles pass.
- [ ] Approved running power-loss test passes or is explicitly excluded with
      release consequence.
- [ ] Storage, reset, and local recovery behavior pass.
- [ ] No unacceptable wear, drift, leak, thermal, current, pressure, or task/heap
      trend.

## H. Security and Deployment

- [ ] All DHR entries reviewed against candidate.
- [ ] DHR-009 independent security audit closed for production release.
- [ ] Service AP production policy approved.
- [ ] Plain HTTP/MQTT deployment boundary explicit.
- [ ] WAN/public/hostile-network operation prohibited unless separately accepted.
- [ ] Credential provisioning, custody, rotation, and revocation process approved.
- [ ] Temporary users/machines/secrets removed.
- [ ] Audit retention/export process approved.
- [ ] Firmware update provenance and rollback policy approved.
- [ ] Residual-risk owner and response process assigned.

## I. Evidence and Documentation

- [ ] Evidence manifest complete and hashed.
- [ ] Raw data preserved.
- [ ] Instrument calibration records included.
- [ ] Requirement-to-test trace matrix complete.
- [ ] Failure/correction history preserved.
- [ ] No credentials or sensitive data in repository evidence.
- [ ] README, architecture, implementation plan, security contract, DHR, release
      notes, and operator/service documents match the accepted product.
- [ ] Installation, commissioning, operation, maintenance, tube replacement,
      calibration, troubleshooting, recovery, and decommissioning instructions
      are complete.
- [ ] Final acceptance record contains no `[PENDING]`.
- [ ] Independent evidence/documentation review complete.

## J. Final State and Source Control

- [ ] Test 19 retained MQTT discovery/configuration behavior passes.
- [ ] Test 20 final pure-suite/isolation proof passes after every applicable
      campaign in Tests 1-19.
- [ ] Test 21 controlled final state and reboot runs last.
- [ ] Pump stopped physically.
- [ ] Generated/applied output zero.
- [ ] Driver unlocked/disabled.
- [ ] Pressure relieved and fluid path safe.
- [ ] No E-stop/fault unless intentionally documented.
- [ ] Production network/security configuration restored.
- [ ] Clean reboot with no stale command or automatic motion.
- [ ] Serial/network test resources released.
- [ ] Feature branch clean and synchronized.
- [ ] Final normal commits pushed without history rewrite.
- [ ] Final supervisory review accepts exact release candidate.
- [ ] Merge to `main` completed only after acceptance.
- [ ] Annotated acceptance tag points to the merge commit.
- [ ] Main and tag pushed; no branch deletion or release publication unless
      separately authorized.

## K. Gate A - Phase 5 Engineering Acceptance

- [ ] Loaded controller/driver/motor/gearbox/pump/tube configuration passes.
- [ ] Loaded trajectory and independently measured RPM pass.
- [ ] Displacement/flow characterization and uncertainty are accepted.
- [ ] Approved pressure/load fixture behavior passes.
- [ ] Stop, software E-stop, authority loss, and communications loss pass.
- [ ] Restart, persistence, storage, and recovery pass.
- [ ] Thermal/endurance/tube-aging campaigns pass.
- [ ] Validated operating limits are frozen and traceable.

**Gate A result:** `[ACCEPTED / FAILED / INCOMPLETE]`

## L. Gate B - Production Readiness

- [ ] Security audit and DHR dispositions support production.
- [ ] Manufacturing and provisioning controls are complete.
- [ ] Installation, operator, maintenance, and training packages are complete.
- [ ] Support, incident, update, rollback, and vulnerability owners are assigned.
- [ ] Customer-use, HAZLOC, and chemical restrictions are complete and truthful.

**Gate B result:** `[READY / OPEN / BLOCKED]`

Gate A may be accepted while Gate B remains open. That combination permits
`CONTROLLED EVALUATION`, not `PRODUCTION`.

## Final Decision

| Field | Value |
|---|---|
| Release candidate | `[PENDING]` |
| Firmware version | `[PENDING]` |
| Control claim | `RPM-CONTROLLED` |
| Initial release classification | `CONTROLLED EVALUATION` |
| Gate A engineering result | `[PENDING]` |
| Gate B production-readiness result | `[PENDING]` |
| Checklist exceptions | `[PENDING]` |
| Final release classification | `[PRODUCTION / CONTROLLED EVALUATION / BLOCKED]` |
| Acceptance authority | `[PENDING]` |
| Date | `[PENDING]` |
