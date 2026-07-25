# Calibration and Performance Plan

**Status:** CRITERIA NOT FROZEN

## 1. Measurement Doctrine

The controller is open-loop. Calibration uses independent physical measurement.

- Generated RPM means commanded STEP rate.
- External tachometer/revolution count establishes actual shaft behavior.
- Collected mass or a traceable flow standard establishes delivered volume.
- A calibrated pressure instrument establishes discharge pressure.
- No value is labeled `actual_rpm`, `actual_flow`, or `actual_pressure` unless a
  real, current measurement produced it.

The provisional `0.04 gal/rev` value is not acceptance evidence and must not be
used as the release calibration without physical proof.

## 2. Prerequisites

Complete before collecting calibration data:

- approved hardware and operating profile;
- approved safe test fluid and known density at test temperature;
- instrument identities and valid calibration;
- tubing conditioning/warm-up procedure;
- defined suction and discharge geometry;
- pressure measurement and relief arrangement;
- approved speed, pressure, temperature, and run-time limits;
- stable firmware candidate and configuration snapshot; and
- blank raw-data sheets with criteria already frozen.

## 3. Preferred Gravimetric Method

For water or another fluid with a traceable density:

1. Tare the collection vessel.
2. Prime the full fluid path and remove visible gas.
3. Stabilize at the test speed and pressure for the approved pre-run interval.
4. Divert flow into the vessel and start timing at the same defined event.
5. Collect for a duration sufficient to exceed instrument-resolution error.
6. Stop timing and weigh the collected mass.
7. Record fluid temperature and use the approved density.
8. Repeat without changing the setup except where the matrix requires it.

Calculations:

```text
volume_mL = collected_mass_g / density_g_per_mL
flow_mL_per_min = volume_mL * 60 / collection_time_s
shaft_revolutions = independently measured output-shaft revolutions
displacement_mL_per_rev = volume_mL / shaft_revolutions
error_percent = 100 * (measured - expected) / expected
```

Do not derive shaft revolutions solely from generated STEP count when validating
slip or missing-step behavior.

## 4. Test Matrix

The approved matrix must include at least:

- low, middle, and high points within the actual release speed range;
- zero-flow/stopped leakage or siphon observation;
- minimum, nominal, and maximum release discharge pressure;
- fresh conditioned tubing and tubing at the approved service-life endpoint;
- each approved direction;
- minimum and maximum approved fluid temperature when material and equipment
  permit; and
- repeat runs sufficient to quantify repeatability.

The acceptance authority must approve the final matrix, sample count, order,
conditioning, and teardown/reassembly scope in the frozen test profile before
testing. No sample count is inferred from this planning document.

## 5. Criteria to Freeze

| Metric | Approved criterion |
|---|---|
| All physical criteria | Approved revision of the frozen test profile in Section 02 |
| Flow behavior | Characterization only; no commanded-flow or closed-loop claim |

## 6. Statistical Reporting

For every matrix point report:

- all raw observations;
- mean;
- minimum and maximum;
- standard deviation;
- coefficient of variation when meaningful;
- absolute and percent error against the frozen expectation;
- pressure, temperature, tubing age, direction, and setup identity; and
- any excluded run with reason and approval.

Reported significant digits must not exceed the least precise contributing
measurement. The uncertainty statement must include external revolution
measurement, mass or volume measurement, density, timing, pressure, and
temperature effects where applicable.

Do not remove outliers merely because they cause failure. A run may be excluded
only for a documented test-execution fault that is independently identifiable.
Preserve excluded raw data.

## 7. Release Interpretation

The fixed `RPM-CONTROLLED` claim means calibration becomes a characterization
table tied to the exact test conditions. UI and documentation must not imply
closed-loop flow. Any published flow estimate must be labeled
calculated/estimated and bounded by the accepted conditions and uncertainty.

An accepted displacement calibration requires independent external revolution
measurement. If that measurement is unavailable, insufficiently resolved, or
not traceable to its uncertainty, the run is
`CHARACTERIZATION/PROVISIONAL` only and cannot establish accepted displacement.
Generated steps and generated RPM remain useful controller evidence but are not
independent shaft proof.

## 8. Calibration Artifact

The final calibration artifact records:

- candidate commit and firmware version;
- controller, motor, driver, gearbox, pump head, tubing, and fluid identities;
- instrument identities and calibration status;
- approved matrix and criteria;
- raw-data file hashes;
- calculation method and script hash/version;
- final coefficients or characterization table;
- external revolutions, measurement method, and uncertainty;
- volume measurement method and uncertainty;
- tube runtime or cycle count;
- generated steps and generated RPM, explicitly labeled as non-independent
  controller evidence;
- uncertainty and restrictions;
- reviewer approval; and
- effective date and recalibration triggers.

Recalibration triggers include pump-head replacement, tubing type/size change,
gear ratio or microstep change, motor/driver change, calibration-path firmware
change, material process change, unexplained drift, or owner-defined interval.
