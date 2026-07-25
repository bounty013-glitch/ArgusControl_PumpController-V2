# Safety Setup and Hard Stops

**Status:** NOT APPROVED

## 1. Safety Authority

Physical safety is highest authority. Software, browser controls, MQTT, serial
tools, and software E-stop are not substitutes for:

- an immediately reachable physical power disconnect;
- guarding;
- pressure relief;
- containment;
- correct wiring and overcurrent protection; or
- trained operator judgment.

The Phase 5 test lead may stop any test without completing the current step.

## 2. Required Roles

| Role | Responsibility | Assigned |
|---|---|---|
| Test lead | Runs procedure and owns stop decision | `[REQUIRED]` |
| Safety observer | Watches hardware/fluid path and controls disconnect | `[REQUIRED for powered/fluid tests]` |
| Evidence recorder | Captures synchronized observations and raw data | `[REQUIRED]` |
| Acceptance authority | Approves criteria and final disposition | `[REQUIRED]` |

One person may not operate the UI, observe all physical hazards, and serve as the
independent safety observer during loaded or pressurized testing.

## 3. Required Equipment

- guarded and securely mounted controller, driver, motor, coupling, and pump;
- correctly rated power supply, fuse/protection, and physical disconnect;
- GFCI/RCD where liquids and mains-powered instruments are present;
- leak-resistant test reservoir and secondary containment;
- approved tubing, hose, fittings, clamps, and relief/bypass path;
- calibrated scale or traceable flow standard;
- calibrated pressure gauge/transducer at the defined measurement point;
- independent tachometer or shaft-revolution measurement;
- current measurement suitable for the motor supply;
- temperature measurement for controller, driver, motor, pump head, tubing, and
  fluid where applicable;
- oscilloscope or logic analyzer for timing gates;
- stopwatch/data logger and evidence workstation;
- guarding, eye protection, gloves, spill kit, and any profile-specific PPE; and
- fire extinguisher and emergency response equipment appropriate to the setup.

Instrument model, serial, range, resolution, calibration date, and due date are
recorded in the evidence manifest.

## 4. Pre-Power Inspection

Confirm and photograph:

- UIM344 COM to controller 3.3 V, never GND or 5 V;
- STEP GPIO3, DIR GPIO4, ENA GPIO5;
- active-low/inverted behavior matches the accepted wiring doctrine;
- controller and motor power polarity, voltage, grounding, and current capacity;
- UIM microstep setting agrees with firmware;
- UIM current and maximum-missing-step settings are approved;
- coupling and shaft are guarded;
- pump head and tubing are installed to manufacturer procedure;
- tubing is undamaged, correctly seated, and not beyond service life;
- suction and discharge are secured;
- discharge cannot be dead-headed without an approved relief path;
- containment can hold the full test volume;
- physical disconnect is reachable by the safety observer; and
- the controller boots stationary with zero output and driver disabled.

Any mismatch is a hard stop.

## 5. Powered Gate

Immediately before the first powered motion command, a human must explicitly
confirm:

- the exact motor/pump assembly is connected as documented;
- hardware is physically secured and guarded;
- the area is clear;
- the approved fluid/load state matches the current test;
- the relief path and containment are ready;
- the disconnect is under the safety observer's control; and
- powered testing may begin.

The confirmation, time, operator, candidate commit, and next authorized maximum
speed/pressure are recorded. Authorization is bounded to that test stage and does
not carry automatically into a higher speed, pressure, fluid, or duration.

## 6. Command Discipline

- Issue one command at a time.
- Observe response, authoritative state, generated output, and physical result
  before continuing.
- Begin at the lowest approved speed with zero or minimum hydraulic load.
- Increase only through preapproved increments.
- Never select the diagnostic 200 RPM option merely because it exists.
- Do not use a timing race, test backdoor, direct lower-layer call, or stale
  authority to manufacture a condition.
- Use production browser/MQTT paths for end-to-end acceptance. Diagnostic CLI
  tests are supplemental lower-layer checks only.
- Keep a physical stop method independent of Wi-Fi, MQTT, browser, and MCU.

## 7. Immediate Stop Conditions

Remove motor power immediately for:

- unexpected start, no-stop, restart, speed, or direction;
- shaft, coupling, guard, hose, fitting, or tubing movement;
- buzz, stall, step loss, abrupt jump, grinding, abnormal vibration, or driver lock;
- leak, tube walkout, rupture, spray, cavitation, loss of prime, or uncontrolled
  siphon;
- pressure above the approved test limit or relief malfunction;
- abnormal current, temperature, odor, discoloration, or smoke;
- controller panic/reset loop, watchdog, brownout, stack/heap corruption, or
  repeated network lifecycle instability;
- software state inconsistent with physical behavior;
- inability to read critical instruments or capture evidence; or
- safety observer instruction.

After an immediate stop:

1. make pressure and stored energy safe;
2. isolate electrical power;
3. preserve logs and the physical configuration;
4. record the last command and all observations;
5. do not use Recover or reset merely to continue;
6. classify the event and identify root cause; and
7. obtain review approval before any rerun.

## 8. Software E-Stop Boundary

The browser and MQTT E-stop commands are software-level functions. They must be
tested for application behavior, but must never be described as safety-rated or
used as the only emergency protection.

The accepted active-low hold behavior can retain driver holding torque after a
software E-stop. The procedure must account for stored mechanical force and must
not assume E-stop means electrically de-energized or freely movable.

## 9. Final Safe State

Every test block ends with:

- normal Stop where safe and applicable;
- confirmed generated/applied output zero;
- Unlock and driver disabled;
- physical shaft stopped;
- pressure relieved;
- no leak or tube damage;
- target recorded or reset according to procedure;
- no latched E-stop/fault unless intentionally under test; and
- motor power isolated before setup changes.
