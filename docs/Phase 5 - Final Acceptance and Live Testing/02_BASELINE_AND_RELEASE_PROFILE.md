# Baseline and Release Profile

**Status:** INCOMPLETE - REQUIRED BEFORE EXECUTION

## 1. Immutable Input Baseline

| Item | Required value |
|---|---|
| Main commit | `31ea4254992f296001d367cece70998659a82783` |
| Accepted tag | `v2-phase4d.4-machine-client-auth-accepted` |
| Phase 4D.4 implementation | `cea28c2c8476f8f991f8337699e24cb16ea217e8` |
| Baseline firmware identity | `v2-phase4d.4-dev` |
| Toolchain | ESP-IDF v5.5.3 |
| Target | ESP32-S3 |
| Baseline pure-suite inventory | 268 distinct, 804 executions per invocation |

Before Phase 5 work, record local and remote `main`, tag object and peeled commit,
branch, upstream, status, submodules if any, and repository-sensitive-data audit.

## 2. Source-Reconciled Facts

The future executor must begin from these verified facts:

- firmware commands output-shaft speed in milli-RPM;
- active defaults are 200 full steps/rev, 4 microsteps, and 10:1 gearbox;
- hardware configuration names 500 mRPM as the nominal minimum and 200,000
  mRPM as the maximum;
- the production state manager accepts configured targets from 0 through
  200,000 mRPM, while the pulse generator accepts nonzero generated rates from
  1 through 200,000 mRPM so a trajectory can traverse below the nominal minimum;
- trajectory defaults are 10,000 milli-RPM/s acceleration and deceleration;
- STEP is active-low on GPIO3 with 15 microsecond active pulse;
- DIR is GPIO4 and inverted;
- ENA is GPIO5 and active-low;
- the UIM344 COM connection is the verified 3.3 V common-anode arrangement;
- the UIM344 manual requires greater than 4 microseconds STEP pulse in its pin
  table and describes protection for overcurrent, overvoltage, and overheating;
- the driver can internally lock after its configured maximum missing-step
  threshold, but the controller has no accepted direct driver-fault input;
- `feedback_available=false`; the controller has no direct shaft, flow,
  pressure, temperature, or motor-current feedback;
- `argus_conversions_flow_to_rpm()` exists as fixed-point math only;
- `CONFIG_ARGUS_DISPLACEMENT_GAL_PER_REV` defaults to provisional `0.04`;
- no production `argus_displacement` module exists despite the architecture
  diagram naming one;
- no accepted production flow command, persistent calibrated displacement,
  measured-flow telemetry, or closed-loop flow path exists; and
- 200 RPM is a firmware limit and earlier unloaded test point, not proof that
  every pump head, tube, fluid, pressure, or duty cycle is safe at 200 RPM.

Any contradiction discovered during Phase 5 is a finding, not permission to pick
the more convenient statement.

## 3. Phase Identity

If Phase 5 includes functional implementation, Step 0 is:

- branch: `phase5-final-acceptance-and-live-testing` unless owner changes it;
- development firmware identity: `v2-phase5-dev`;
- runtime and suite labels updated to Phase 5; and
- active evidence identifiers updated before functional work.

The final release version and tag are separate decisions:

| Decision | Approved value |
|---|---|
| Final semantic firmware version | `[REQUIRED]` |
| Release tag | `[REQUIRED]` |
| Release artifact naming | `[REQUIRED]` |
| Supported upgrade source versions | `[REQUIRED]` |

## 4. Product Hardware Profile

| Field | Approved value / evidence |
|---|---|
| Controller PCB and hardware revision | `[REQUIRED]` |
| ESP32 module/board and serial/MAC | `[REQUIRED]` |
| Power supply model, voltage, current rating | `[REQUIRED]` |
| Protection, fuse, disconnect, and grounding | `[REQUIRED]` |
| Motor model and serial/lot | `[REQUIRED]` |
| UIM344 model/firmware | `[REQUIRED]` |
| UIM working current | `[REQUIRED]` |
| UIM idle-current percentage | `[REQUIRED]` |
| UIM microstep setting | `[REQUIRED; must agree with firmware]` |
| UIM maximum-missing-steps setting | `[REQUIRED]` |
| Gearbox model and actual ratio | `[REQUIRED]` |
| Pump-head model and serial/lot | `[REQUIRED]` |
| Coupling, guard, and mounting | `[REQUIRED]` |
| Tubing manufacturer/material/size/lot | `[REQUIRED]` |
| Tube occlusion/compression setting | `[REQUIRED]` |
| Suction/discharge hose and fittings | `[REQUIRED]` |
| Relief/bypass device and setpoint | `[REQUIRED]` |

Photographs must show labels, wiring, common-anode connection, guarding, fluid
path, relief arrangement, and instrument placement without exposing credentials.

## 5. Operating and Deployment Envelope

| Field | Approved value |
|---|---|
| Minimum/maximum commanded RPM | `[REQUIRED]` |
| External command policy for 1-499 mRPM | `[SUPPORTED / REJECTED / NOT CLAIMED - REQUIRED]` |
| Permitted direction(s) | `[REQUIRED]` |
| Maximum continuous run duration | `[REQUIRED]` |
| Duty cycle and restart interval | `[REQUIRED]` |
| Ambient temperature range | `[REQUIRED]` |
| Test/release fluid and density range | `[REQUIRED]` |
| Fluid temperature range | `[REQUIRED]` |
| Viscosity range | `[REQUIRED]` |
| Suction lift/inlet condition | `[REQUIRED]` |
| Discharge pressure range | `[REQUIRED]` |
| Maximum permitted pressure | `[REQUIRED]` |
| Expected flow range | `[REQUIRED]` |
| Flow accuracy/repeatability claim | `[REQUIRED or explicitly NONE]` |
| Tubing service-life limit | `[REQUIRED]` |
| Approved network topology | `[REQUIRED]` |
| Internet/WAN exposure | `PROHIBITED unless a later accepted security phase changes this` |
| Intended users and service access | `[REQUIRED]` |

## 6. Chemical and Process Boundary

The first wetted campaign uses water or a documented nonhazardous surrogate.
No hydrogen peroxide, treatment chemical, pressurized process, or field injection
is authorized until all of the following exist:

- concentration and temperature range;
- current manufacturer compatibility evidence for every wetted material;
- decomposition, off-gassing, reaction, exposure, and spill assessment;
- required ventilation and PPE;
- pressure relief and containment;
- disposal procedure;
- owner/EHS approval; and
- a separately approved chemical/process test procedure.

If chemical qualification is not completed, the release statement must exclude
that chemical and process use explicitly.

## 7. Release Claim Freeze

**Selected profile:** `[RPM-CONTROLLED / FLOW-DELIVERY / EVALUATION-ONLY]`

**Exact supported claims:** `[REQUIRED]`

**Exact prohibited claims:** `[REQUIRED]`

**Approved by:** `[NAME / ROLE / DATE]`

No acceptance criteria may be relaxed after testing begins without invalidating
the affected result and repeating the test under the newly approved criterion.
