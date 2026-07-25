# Baseline and Release Profile

**Status:** INCOMPLETE - REQUIRED BEFORE EXECUTION

## 1. Immutable Input Baseline

| Item | Required value |
|---|---|
| Phase 5 planning-inclusive branch point | `6d907a11e294fde26a33fbb60b898839883e1490` |
| Accepted firmware/source baseline | `31ea4254992f296001d367cece70998659a82783` |
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
| Final semantic firmware version | `REQUIRES SHAWN CONFIRMATION` |
| Release tag | `REQUIRES SHAWN CONFIRMATION` |
| Release artifact naming | `REQUIRES SHAWN CONFIRMATION` |
| Supported upgrade source versions | `REQUIRES SHAWN CONFIRMATION` |

## 4. Product Hardware Profile

| Field | Approved value / evidence |
|---|---|
| Controller PCB and hardware revision | `REQUIRES SHAWN CONFIRMATION` |
| ESP32 module/board and serial/MAC | `REQUIRES SHAWN CONFIRMATION` |
| Power supply model, voltage, current rating | `REQUIRES SHAWN CONFIRMATION` |
| Protection, fuse, disconnect, and grounding | `REQUIRES SHAWN CONFIRMATION` |
| Motor model and serial/lot | `REQUIRES SHAWN CONFIRMATION` |
| UIM344 model/firmware | `REQUIRES SHAWN CONFIRMATION` |
| UIM working current | `REQUIRES SHAWN CONFIRMATION` |
| UIM idle-current percentage | `REQUIRES SHAWN CONFIRMATION` |
| UIM microstep setting | `REQUIRES SHAWN CONFIRMATION; must agree with firmware` |
| UIM maximum-missing-steps setting | `REQUIRES SHAWN CONFIRMATION` |
| Gearbox model and actual ratio | `REQUIRES SHAWN CONFIRMATION` |
| Pump-head model and serial/lot | `REQUIRES SHAWN CONFIRMATION` |
| Coupling, guard, and mounting | `REQUIRES SHAWN CONFIRMATION` |
| Tubing manufacturer/material/size/lot | `REQUIRES SHAWN CONFIRMATION` |
| Tube occlusion/compression setting | `REQUIRES SHAWN CONFIRMATION` |
| Suction/discharge hose and fittings | `REQUIRES SHAWN CONFIRMATION` |
| Relief/bypass device and setpoint | `REQUIRES SHAWN CONFIRMATION` |

Photographs must show labels, wiring, common-anode connection, guarding, fluid
path, relief arrangement, and instrument placement without exposing credentials.

## 5. Frozen Test Profile and Acceptance Values

Every live test records the approved profile revision and uses only values from
this table. A change invalidates affected evidence and requires a new revision
approved before retest. `REQUIRES SHAWN CONFIRMATION` means do not execute the
dependent test.

**Profile revision:** `REQUIRES SHAWN CONFIRMATION`

| Field | Approved value | Information needed / conservative starting recommendation |
|---|---|---|
| Mechanical configuration and mounting | `REQUIRES SHAWN CONFIRMATION` | Exact controller, driver, motor, gearbox, pump head, coupling, guard, and fixture. |
| Hose/tube identity, lot, condition, occlusion | `REQUIRES SHAWN CONFIRMATION` | Record new/used condition and accumulated runtime/cycles. |
| Initial wetted test fluid | `REQUIRES SHAWN CONFIRMATION` | Start only with water or an approved benign nonhazardous surrogate. |
| Actual gearbox ratio | `REQUIRES SHAWN CONFIRMATION` | Verify nameplate/configuration; do not assume nominal 10:1. |
| Driver microstep/current/idle/missing-step settings | `REQUIRES SHAWN CONFIRMATION` | Verify physically and reconcile with firmware before power. |
| Minimum test RPM | `REQUIRES SHAWN CONFIRMATION` | Conservative proposal: begin at the accepted nominal minimum, 500 mRPM, at minimum hydraulic load. |
| Speed test points | `REQUIRES SHAWN CONFIRMATION` | Approve low/mid/high points within the physical assembly ratings. |
| Provisional upper test RPM | `REQUIRES SHAWN CONFIRMATION` | Do not use the 200 RPM firmware bound as a physical rating; advance incrementally only after lower points pass. |
| Permitted direction(s) | `REQUIRES SHAWN CONFIRMATION` | Test only directions allowed by pump, tube, and process hardware. |
| Acceleration/deceleration ramps | `REQUIRES SHAWN CONFIRMATION` | Begin with accepted firmware defaults only after confirming they are safe for the loaded assembly; tune from measured evidence. |
| Run durations and cycle counts | `REQUIRES SHAWN CONFIRMATION` | Approve shakedown, steady-state soak, endurance, start/stop, direction, reboot, and power-cycle counts. |
| Temperature limits | `REQUIRES SHAWN CONFIRMATION` | Use the lowest applicable manufacturer/component/fluid limit. |
| Current limits | `REQUIRES SHAWN CONFIRMATION` | Use supply, driver, and motor ratings plus approved trip criteria. |
| Pressure range and maximum | `REQUIRES SHAWN CONFIRMATION` | Initial wet run uses free discharge/minimum pressure; no dead-head or closed discharge. |
| Repeatability criterion | `REQUIRES SHAWN CONFIRMATION` | Define per matrix point before data are seen. |
| Allowed trajectory/RPM deviation | `REQUIRES SHAWN CONFIRMATION` | Define against independent shaft measurement. |
| Displacement/flow uncertainty limit | `REQUIRES SHAWN CONFIRMATION` | Include external revolution, mass/volume, density, and timing uncertainty. |
| Sampling cadence | `REQUIRES SHAWN CONFIRMATION` | Freeze synchronized measurement/log intervals before each campaign. |
| Warm-up/conditioning/soak | `REQUIRES SHAWN CONFIRMATION` | Specify tube conditioning and stabilization criteria, not observation after the fact. |
| Tube aging/service-life endpoint | `REQUIRES SHAWN CONFIRMATION` | Define runtime/cycle endpoint and inspection/replacement criteria. |
| Immediate-stop thresholds | `REQUIRES SHAWN CONFIRMATION` | Combine Section 03 qualitative stops with approved current, temperature, and pressure thresholds. |
| External command policy for 1-499 mRPM | `REQUIRES SHAWN CONFIRMATION` | Choose supported, rejected, or not claimed. |
| Ambient and fluid temperature range | `REQUIRES SHAWN CONFIRMATION` | Bound to instrumentation and material ratings. |
| Suction/inlet geometry and discharge arrangement | `REQUIRES SHAWN CONFIRMATION` | Record lift, hose geometry, relief, collection, and containment. |
| Flow claim | `CHARACTERIZATION ONLY` | No commanded-flow or closed-loop-flow claim. |
| Approved network topology | `REQUIRES SHAWN CONFIRMATION` | Must preserve accepted trusted-local security boundary. |
| Internet/WAN exposure | `PROHIBITED unless a later accepted security phase changes this` |
| Intended users and service access | `REQUIRES SHAWN CONFIRMATION` |

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

## 7. Control Claim and Release Classification

**Control claim:** `RPM-CONTROLLED`

**Initial release classification:** `CONTROLLED EVALUATION`

**Possible final classification:** `PRODUCTION`, `CONTROLLED EVALUATION`, or
`BLOCKED`

**Exact supported claims:** `REQUIRES SHAWN CONFIRMATION`

**Exact prohibited claims:** no commanded-flow, measured-flow-telemetry,
closed-loop-flow, universal calibration, safety-rated E-stop, customer chemical,
HAZLOC, or unrestricted production claim without separate acceptance evidence.

**Approved by:** `[NAME / ROLE / DATE]`

No acceptance criteria may be relaxed after testing begins without invalidating
the affected result and repeating the test under the newly approved criterion.
