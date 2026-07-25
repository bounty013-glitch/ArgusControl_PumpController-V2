# Evidence and Traceability

**Status:** TEMPLATE READY

## 1. Evidence Principles

- Evidence identifies the exact candidate.
- Raw observations are preserved.
- Criteria are recorded before results.
- Screenshots supplement; they do not replace logs or measured data.
- A result from one commit is not transferred silently to another.
- Secrets, credentials, session tokens, CSRF tokens, private keys, and raw
  Authorization data never enter evidence.
- Historical failures and corrections remain visible.

## 2. Evidence Folder Structure

Create under this Phase 5 folder when execution begins:

```text
Evidence/
  00_manifest/
  01_baseline/
  02_reviews/
  03_build/
  04_flash_boot/
  05_pure_suites/
  06_electrical_motion/
  07_calibration_raw/
  08_pressure_load/
  09_browser_mqtt/
  10_fault_reliability/
  11_security/
  12_final_state/
  13_release_artifacts/
```

Large binary/raw artifacts may use an approved external evidence store when repo
size would be unreasonable. The manifest must contain the stable location,
access owner, retention policy, and SHA-256.

## 3. Naming

Use:

```text
P5-<test-id>-<candidate-short-sha>-<UTC-or-local-qualified-time>-<description>.<ext>
```

Examples:

```text
P5-T05-abcdef1-20260725T143000-0500-pure-suite-run1.log
P5-T10-abcdef1-20260725T151500-0500-calibration-raw.csv
P5-T20-abcdef1-20260725T190000-0500-final-pure-suite-run1.log
```

Record the timezone explicitly. Do not fabricate synchronized time if the
controller lacks a trustworthy source.

## 4. Manifest Fields

Every artifact entry contains:

- artifact ID;
- test ID;
- file name/location;
- SHA-256;
- byte size;
- candidate commit and firmware version;
- controller hardware UID;
- equipment/profile revision;
- instrument IDs;
- operator and observer;
- capture time and timezone/time-validity statement;
- description;
- contains sensitive data: `NO` required before repository inclusion; and
- reviewer disposition.

## 5. Test Record Template

```text
Test ID:
Candidate commit:
Firmware identity:
Profile revision:
Initial condition:
Frozen expected result:
Operator action:
Actual observation:
Measurements and uncertainty:
Controller/UI/MQTT/serial state:
Physical result:
Artifacts:
Anomalies:
Result: PASS / FAIL / INVALID / INCOMPLETE
Operator/date:
Independent reviewer/date:
```

## 6. Build Provenance

Preserve:

- `idf.py --version`;
- clean-tree and source commit;
- full-clean/no-ccache commands;
- complete build log;
- exact warning/error audit;
- ELF, MAP, BIN, bootloader, partition table, OTA metadata where applicable;
- `idf.py size` output;
- hashes and byte sizes;
- sdkconfig and sdkconfig.defaults identity without secrets;
- compiler/linker versions; and
- reproducibility comparison when required.

## 7. Physical Data

Calibration/reliability CSV files use immutable columns and units. At minimum:

```text
test_id,run,point,time,timezone_valid,candidate,controller_uid,
motor_id,pump_id,tube_id,fluid_id,direction,command_mrpm,
generated_mrpm,generated_step_count,external_rpm,external_revolutions,
external_revolution_measurement_method,external_revolution_uncertainty,
volume_measurement_method,volume_measurement_uncertainty,
tube_runtime_or_cycle_count,pressure_kpa,mass_g,density_g_ml,
collection_s,flow_ml_min,voltage_v,current_a,
ambient_c,controller_c,driver_c,motor_c,pump_c,result,notes
```

Use blank/NA for unavailable measurements; never substitute zero.
Generated RPM and generated step count are retained as controller-output
evidence, not independent proof of shaft motion. If external revolution
measurement is missing, inadequately resolved, or lacks an uncertainty
statement, displacement results are `CHARACTERIZATION/PROVISIONAL` and cannot be
accepted as displacement calibration. Report no more significant digits than
the least precise contributing measurement supports.

## 8. Defect and Rerun Traceability

For every finding record:

- first affected candidate and test;
- symptom and preserved evidence;
- root cause;
- safety classification;
- correction commit;
- regression tests;
- independent review;
- invalidated evidence;
- required rerun scope; and
- final disposition.

Failed attempts remain in the record and are not renamed as PASS.

## 9. Acceptance Trace Matrix

The final trace matrix maps:

```text
Requirement -> Test ID -> Frozen criterion -> Candidate -> Evidence IDs
-> Result -> Reviewer -> Residual limitation
```

Every release claim and every open/closed DHR disposition needs at least one row.

## 10. Evidence Closure Audit

Before final acceptance:

- hash every artifact;
- confirm manifest/artifact one-to-one consistency;
- confirm all tests identify the same accepted candidate or explicitly explain
  candidate transitions;
- scan repository evidence for credentials and personal/sensitive data;
- confirm raw data and calculation results agree;
- confirm no `[PENDING]` remains in the final acceptance record;
- confirm failures/corrections are preserved;
- confirm final state and COM/network resource release; and
- obtain independent evidence review.
