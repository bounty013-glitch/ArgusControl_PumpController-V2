# Phase 4D.3 Chronological Test Record

**Status:** IN PROGRESS

**Branch:** `phase4d3-browser-auth-and-administration`

**Firmware identity:** `v2-phase4d.3-dev`

## 1. Starting Repository State

Phase 4D.3 began from clean, synchronized `main` at `5cbfbbfaa513567a66e97919e55f2e9c7158566a`. The annotated `v2-phase4d.2-security-foundation-accepted` tag peels to that commit. The accepted Phase 4D.1 and Phase 4C tags remain ancestors. No unexplained user changes were present.

The feature branch was created as `phase4d3-browser-auth-and-administration`. Identity was established first in `9d5201a` (`chore: establish Phase 4D.3 identity`) and pushed before functional implementation.

## 2. Contract Reconciliation

The pre-implementation audit found two accepted Phase 4D.2 facts that the Phase 4D.1 contract still described as future work: encrypted `sec_store` is active with software-stored, physically extractable XTS keys, and GPIO0/KEY1 is the physically validated recovery input. The contract was corrected without weakening its physical-extraction limitation.

The accepted Client Admin amendment makes `manage_client_admins` the twenty-third stable capability. Session capacity remains the contract-defined eight total and two per account.

## 3. Route and Capability Inventory

The complete existing route inventory, SoftAP-only classification, migration disposition, administration routes, and command capability matrix are recorded in `PHASE_4D_3_IMPLEMENTATION_PLAN.md`. Every human route is deny-by-default and must prove both SoftAP local destination and SoftAP peer before parsing sensitive bodies or entering KDF work.

## 4. Implementation Chronology

- Authentication implementation: [PENDING]
- Session and CSRF implementation: [PENDING]
- Authorization implementation: [PENDING]
- Human-account and custom-role administration: [PENDING]
- Legacy Basic-Auth retirement: [PENDING]
- Security audit implementation: [PENDING]
- Active AP-password administration: [PENDING]
- Authenticated recovery exit: [PENDING]

## 5. Failures, Diagnosis, and Corrections

[PENDING]

## 6. Host and Pure Validation

[PENDING]

## 7. Browser and JavaScript Validation

[PENDING]

## 8. MQTT and Command-Boundary Regression

[PENDING]

## 9. Build Evidence

[PENDING]

## 10. Hardware Validation

[PENDING]

No powered-motion command is authorized by this test record.

## 11. Credential-Safe Human Interaction

Any real credential is entered only through a local browser or non-echoing terminal prompt. No real credential may appear in chat, command-line arguments, tracked files, logs, screenshots, or this record.

## 12. Final Repository and Device Disposition

[PENDING]

## 13. Deferred Phase 4D.4 Work

Machine enrollment, MQTT CONNECT authentication, HTTPS, TLS, certificates, hostile-network acceptance, and irreversible device-security provisioning remain deferred.
