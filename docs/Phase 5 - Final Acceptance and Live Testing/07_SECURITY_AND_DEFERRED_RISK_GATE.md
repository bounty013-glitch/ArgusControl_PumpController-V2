# Security and Deferred-Risk Gate

**Status:** OPEN - RELEASE DECISION REQUIRED

## 1. Release Boundary

This document is the principal Gate B security input. Gate A engineering
acceptance may pass while this gate remains open, but the final release
classification must remain `CONTROLLED EVALUATION` or `BLOCKED`.

The accepted baseline provides local human authentication, CSRF-protected
browser sessions, machine enrollment, MQTT CONNECT authentication, per-packet
authorization, credential rotation/revocation, and bounded audit records.

It does not provide:

- HTTPS;
- MQTT TLS;
- certificate lifecycle;
- hostile-network or public-Internet acceptance;
- physical flash-extraction resistance;
- secure boot or flash encryption;
- broad penetration-test acceptance;
- unlimited audit retention; or
- trustworthy wall-clock time without an accepted source.

Wi-Fi encryption is not end-to-end HTTP or MQTT transport security.

## 2. Deferred Register Review

Every DHR entry must receive a Phase 5 disposition. At minimum, resolve:

| Entry | Phase 5 issue | Required disposition |
|---|---|---|
| DHR-002 | Browser authentication over plaintext HTTP | Restrict deployment or implement/accept transport security |
| DHR-003 | Software-stored XTS keys remain physically extractable | Accept physical-trust boundary or implement hardware-backed protection |
| DHR-009 | Comprehensive security audit explicitly required before production release | Complete independent audit or do not claim production release |
| DHR-011 | Always-advertised Service AP production default undecided | Approve on/off/toggle policy from field evaluation |
| DHR-015 | Flat HTTP JSON parser limitations | Confirm no schema expansion/untrusted exposure or harden before release |
| DHR-016 | MQTT transport/adversarial hardening remains open | Keep trusted-local boundary explicit |
| DHR-017 | Certificates and hostile-network security deferred | Exclude WAN/public/hostile operation |
| DHR-018 | Residual credential, DoS, audit, time, and physical risks | Update for Phase 4D.4 machine auth and approve remaining risks |

Closed entries are also rechecked for regression. A historical `CLOSED` status is
not proof that the release candidate still implements the behavior.

## 3. Security Audit Gate

DHR-009 states that formal security review is required before production field
release. Therefore:

- a production release cannot pass Phase 5 while DHR-009 remains open;
- a controlled evaluation release may proceed only with an explicit
  evaluation-only classification, trusted-local topology, no routed WAN/public
  exposure, named operators, and owner-approved risk acceptance; and
- calling the result "production accepted" is prohibited unless the gate is
  actually closed with evidence.

The audit scope should include:

- browser authentication/session/CSRF and route inventory;
- account, role, permission, delegation, reauthentication, and audit behavior;
- AP-password and local recovery lifecycle;
- machine enrollment, one-time disclosure, verifier storage, rotation,
  disable/revoke/delete, and invalidation races;
- MQTT CONNECT parser, interface proof, topic scope, capabilities, duplicate
  identity, subscription/publication ordering, and Phase 4C freshness;
- HTTP/MQTT body and resource bounds;
- network exposure and service-AP policy;
- storage corruption, rollback, reset, and migration;
- secret zeroization and logging;
- denial-of-service/resource exhaustion;
- firmware/update and physical-debug exposure; and
- release artifact and provisioning handling.

## 4. Live Security Regression

Using temporary identities and no reusable secret in evidence:

- reject unauthenticated and unauthorized human routes;
- reject STA access to SoftAP-only human routes;
- prove login throttle and recovery;
- prove CSRF/same-origin rejection;
- prove logout and session revocation;
- enroll a least-privilege machine;
- reject missing/wrong credentials and disallowed interface;
- enforce exact subscription and publication scope;
- prove authentication does not grant operating authority;
- prove rotation and revocation close or make inert the exact connection;
- prove unrelated machine and human sessions remain unaffected;
- delete all temporary records; and
- audit prepared/terminal outcomes and pagination.

## 5. Service AP Decision

Before final release, record:

- commissioned-device AP default: `[ON / OFF / CONFIGURABLE]`;
- recovery path when AP is off: `REQUIRES SHAWN CONFIRMATION`;
- operational need for field discoverability: `REQUIRES SHAWN CONFIRMATION`;
- exposure/risk rationale: `REQUIRES SHAWN CONFIRMATION`;
- test evidence from representative field use:
  `REQUIRES SHAWN CONFIRMATION`; and
- acceptance authority approval: `REQUIRES SHAWN CONFIRMATION`.

If code is needed to implement the decision, it creates a new reviewed candidate.

## 6. Deployment Security Profile

| Property | Approved release value |
|---|---|
| Browser network | Controller SoftAP only |
| MQTT network | Approved trusted local network only |
| Routed WAN/public Internet | Prohibited |
| Untrusted peer on AP/LAN | Not accepted |
| Physical access equivalence | `REQUIRES SHAWN CONFIRMATION` |
| Credential provisioning/custody | `REQUIRES SHAWN CONFIRMATION` |
| AP credential uniqueness/rotation | `REQUIRES SHAWN CONFIRMATION` |
| Machine credential rotation/revocation | `REQUIRES SHAWN CONFIRMATION` |
| Audit export/retention process | `REQUIRES SHAWN CONFIRMATION` |
| Firmware update provenance | `REQUIRES SHAWN CONFIRMATION` |
| Vulnerability response owner | `REQUIRES SHAWN CONFIRMATION` |

## 7. Gate Result

**Initial release classification:** `CONTROLLED EVALUATION`

**Final release classification:** `[PRODUCTION / CONTROLLED EVALUATION / BLOCKED]`

**DHR dispositions complete:** `[YES/NO]`

**Independent audit reference:** `REQUIRES SHAWN CONFIRMATION FOR PRODUCTION`

**Residual-risk approval:** `[NAME / ROLE / DATE]`

**Result:** `[PENDING]`
