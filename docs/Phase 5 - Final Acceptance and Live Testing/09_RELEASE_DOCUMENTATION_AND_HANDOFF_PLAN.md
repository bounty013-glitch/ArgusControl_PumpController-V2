# Release Documentation and Handoff Plan

**Status:** DELIVERABLES NOT CREATED

Bench acceptance is not the end of a release. Before final Phase 5 approval, the
following product-facing documents and artifacts must be completed for the exact
release profile and reviewed against the accepted candidate.

These deliverables are Gate B production-readiness evidence. Gate A engineering
acceptance does not waive installation, manufacturing, provisioning, operator,
maintenance, training, support, customer-use, HAZLOC, or chemical-use
restrictions.

## 1. Required Product Documents

### Installation and Wiring Guide

Must include:

- approved controller, supply, motor, driver, pump, tube, hose, protection, and
  enclosure BOM;
- labeled wiring diagram and terminal/pin table;
- critical UIM344 common-anode COM-to-3.3 V warning;
- STEP/DIR/ENA polarity;
- grounding, fusing, disconnect, environmental, and enclosure requirements;
- mounting, guarding, ventilation, and liquid separation;
- pump-head, tubing, hose, relief, and containment installation;
- network topology and prohibited WAN/public exposure; and
- pre-power inspection and initial safe-state check.

### Commissioning Guide

Must include:

- exact supported firmware artifact and hash verification;
- first boot, identity, AP, STA, and security provisioning;
- account and machine-client enrollment with secret-handling rules;
- driver microstep/current/idle/missing-step settings;
- pump/tubing/fluid profile entry;
- calibration or characterization profile loading/verification;
- authority and command-path checks;
- stationary and low-speed commissioning tests;
- final production configuration backup; and
- commissioning acceptance/signature fields.

The controller-to-HMI machine enrollment and stationary provisioning procedure
is defined in `12_HMI_CONNECTION_AND_PROVISIONING_GUIDE.md`. It must remain
source-consistent with the accepted HMI candidate before field use.

### Operator Guide

Must explain in plain language:

- machine states and what the pump is doing;
- target, applied, generated, and physically measured distinctions;
- Start, Stop, Unlock, software E-stop, reset, and Recover;
- browser and MQTT authority;
- stale/unavailable status;
- fail-operational behavior during network/heartbeat loss;
- normal startup/shutdown;
- alarms/faults and immediate operator actions;
- release operating envelope and prohibited uses; and
- software E-stop versus physical disconnect.

The guide must state that the controller is `RPM-CONTROLLED`; characterized flow
does not create a commanded-flow, measured-flow-telemetry, or closed-loop-flow
feature.

### Service and Maintenance Guide

Must include:

- lockout/isolation and pressure-relief procedure;
- tube inspection, replacement, conditioning, and service-life limit;
- pump head, coupling, guard, and fitting inspection;
- cleaning and approved materials;
- driver/controller inspection and temperature/current checks;
- calibration verification and recalibration triggers;
- credential/AP recovery and authorized reset boundaries;
- log/audit retrieval without secrets;
- firmware update/rollback;
- troubleshooting by layer; and
- spare parts and escalation contacts.

### Release Notes

Must identify:

- release version, commit, tag, date, and artifact hashes;
- supported hardware/profile;
- implemented capabilities;
- changes since Phase 4D.4;
- accepted test summary;
- fixed findings and preserved correction history;
- known limitations and DHR dispositions;
- security/deployment restrictions;
- upgrade and rollback instructions; and
- support owner.

### Deployment Security Guide

Must state:

- trusted-local boundary;
- SoftAP and MQTT exposure;
- plaintext HTTP/MQTT limitation;
- credential generation, custody, rotation, revocation, and recovery;
- account/role policy;
- prohibited public routing/WAN exposure;
- physical access assumptions;
- audit retention/export;
- incident response; and
- vulnerability reporting/update process.

## 2. Required Release Artifacts

- firmware BIN and ELF/MAP retained for support/backtrace decoding;
- bootloader, partition table, OTA metadata, and approved flashing package;
- SHA-256 manifest;
- sanitized sdkconfig/build provenance;
- approved hardware/configuration profile;
- calibration certificate or characterization table;
- source archive or immutable repository/tag reference;
- dependency/license inventory and required notices;
- security-audit report or controlled-evaluation limitation;
- final evidence manifest and trace matrix;
- rollback artifact and instructions; and
- signed final acceptance record.

No provisioning secret, AP password, human password, machine credential, session
token, private key, or raw Authorization data belongs in the release package.

## 3. Manufacturing and Provisioning Handoff

Define:

- who flashes and verifies firmware;
- how hardware UID and labels are recorded;
- how unique product identity is assigned;
- how factory/recovery/AP credentials are generated and protected;
- how default/shared credentials are avoided or explicitly controlled;
- how driver settings and wiring are inspected;
- how calibration/profile data is loaded and verified;
- how the unit receives a production acceptance record; and
- how failed units are quarantined and reworked.

## 4. Field Handoff

Before shipment/deployment:

- customer/site/unit identity matches labels and MQTT root;
- approved network and security prerequisites exist;
- operating and fail-operational behavior is reviewed with operators;
- physical disconnect and process interlocks are installed;
- spare tube/maintenance materials are available;
- service and escalation contacts are assigned;
- training completion is recorded;
- backup and rollback artifacts are accessible to authorized staff; and
- site acceptance tests and owner signoff are defined.

## 5. Supportability Gate

Demonstrate that a technician who did not build the firmware can:

1. identify the exact release and hardware;
2. understand current state and authority;
3. distinguish generated output from physical delivery;
4. stop and isolate the system safely;
5. diagnose controller, network, MQTT, motor, pump, and process layers;
6. replace tubing and verify/recalibrate the profile;
7. recover local access without unintended motion or destructive reset;
8. collect sanitized logs/evidence; and
9. restore or roll back the approved artifact.

**Technician walkthrough result:** `[PENDING]`

**Documentation review result:** `[PENDING]`

## 6. Document Control

Each final document records:

- product/release version;
- applicable hardware/profile revision;
- document revision and date;
- owner/approver;
- source-of-truth location;
- superseded revision;
- safety warnings;
- release limitations; and
- change history.

Draft or template documents must never ship unlabeled as final.
