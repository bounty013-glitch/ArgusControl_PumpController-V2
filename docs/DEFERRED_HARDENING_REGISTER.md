# Deferred Hardening Register

**Document type:** Rolling record of consciously accepted limitations  
**Authority:** Operator-approved development-phase risk acceptance  
**Status:** Active — all items require reconsideration before production field release

---

> [!IMPORTANT]
> Every item in this register has been explicitly reviewed and accepted by the operator
> for the current development phase. These are deferred, not dismissed. None are approved
> for production field deployment or architectures that permit controller access from
> untrusted or publicly routed networks.

---

## DHR-001 — Dual-Interface Portal Access

| Field | Value |
|-------|-------|
| **Date Recorded** | 2026-07-18 |
| **System Area** | HTTP Service Portal |
| **Current Implementation** | Phase 4D.3 keeps the server bound in APSTA mode but requires the accepted socket's local destination to equal the active SoftAP address and the peer to belong to the SoftAP subnet before any human route performs sensitive work. Native IPv4 and IPv4-mapped IPv6 are normalized; ambiguous and native IPv6 cases fail closed. |
| **Known Limitation** | Human browser access is intentionally unavailable through the commissioned STA/LAN interface. Plain HTTP remains observable to a capable participant already on the SoftAP. |
| **Operator Rationale** | Human administration is physically and topologically scoped to the protected local SoftAP. Machine supervisory traffic remains a separate later-authentication boundary. |
| **Current Exposure Assumptions** | Controller is on a trusted local network. Not Internet-exposed. No routing from public networks to the controller. |
| **Reconsideration Trigger** | Before production field release. Before any architecture permits controller access from an untrusted or publicly routed network. |
| **Target Phase** | Phase 4D (Security Hardening) |
| **Status** | CLOSED |
| **Closure Evidence** | Phase 4D.3 live proof: SoftAP login succeeded at `192.168.4.1`; `/login` and `/api/auth/session` returned `403 local_ap_required` through STA `192.168.1.22`. See `Phase 4D.3 Tests.md`. |

---

## DHR-002 — Browser Authentication Over Unencrypted HTTP

| Field | Value |
|-------|-------|
| **Date Recorded** | 2026-07-18 |
| **System Area** | HTTP Service Portal — Authentication |
| **Current Implementation** | Phase 4D.3 retired browser Basic Auth and uses an `HttpOnly; SameSite=Strict` RAM-only session cookie plus per-session CSRF. Transport remains plaintext HTTP on port 80, so the cookie intentionally omits `Secure`. |
| **Known Limitation** | Credentials and sessions remain visible or modifiable to a capable local packet observer on the SoftAP. Session authentication is not represented as HTTPS-grade transport security. |
| **Operator Rationale** | Acceptable for direct WPA2-PSK AP connections (encrypted at the WiFi layer). LAN exposure is accepted for development. HTTPS (TLS) requires certificate management infrastructure not yet designed. |
| **Current Exposure Assumptions** | WPA2-PSK AP provides link-layer encryption for AP clients. LAN is trusted. No traffic exits the local network. |
| **Reconsideration Trigger** | Before production field release. Before any untrusted network path exists between client and controller. |
| **Target Phase** | Phase 4D (HTTPS / TLS) |
| **Status** | OPEN - TRANSPORT ENCRYPTION DEFERRED |
| **Closure Evidence** | Basic-Auth retirement is accepted in Phase 4D.3; HTTPS/TLS and secure-cookie transport remain open. |

---

## DHR-003 — Plaintext Password Storage in NVS

| Field | Value |
|-------|-------|
| **Date Recorded** | 2026-07-18 |
| **System Area** | HTTP Service Portal — Credential Storage |
| **Current Implementation** | Phase 4D.2 introduced encrypted `sec_store` and PBKDF2-HMAC-SHA-256 verifiers. Phase 4D.3 completed the separate console/human-account domains, transactional password administration, and browser-session transition; plaintext passwords are transient and zeroized. |
| **Known Limitation** | The XTS key is stored in `sec_keys` without eFuse/HMAC derivation or flash encryption and remains physically extractable. |
| **Operator Rationale** | Physical access to the ESP32 constitutes complete device compromise regardless of password storage format. Hashed storage (bcrypt/argon2) is preferred but adds complexity and is not justified for a development-phase portal. |
| **Current Exposure Assumptions** | Controller is in a physically controlled environment. Physical access implies operator trust. |
| **Reconsideration Trigger** | Before production field release. Before any deployment where physical access is not equivalent to operator trust. |
| **Target Phase** | Phase 4D (Hashed Credential Storage) |
| **Status** | OPEN - PARTIALLY RESOLVED |
| **Closure Evidence** | Logical credential storage and browser migration are accepted through Phase 4D.3. Physical-extraction resistance remains open. |

---

## DHR-004 — Bootstrap Credentials

| Field | Value |
|-------|-------|
| **Date Recorded** | 2026-07-18 |
| **System Area** | HTTP Service Portal — Initial Authentication |
| **Current Implementation** | The legacy browser `admin`/`admin` path and Basic-Auth fallback are removed. Browser authentication uses the separately provisioned console verifier and bounded human records; factory provisioning remains an offline/manufacturing boundary. |
| **Known Limitation** | Fleet bootstrap and factory-credential uniqueness still require deployment provisioning policy. No browser API can rotate the factory recovery credential. |
| **Operator Rationale** | Browser access must use a provisioned verifier; recovery-secret rotation remains an offline manufacturing responsibility. |
| **Current Exposure Assumptions** | Manufacturing and provisioning artifacts remain controlled and are not exposed through browser administration. |
| **Reconsideration Trigger** | Before production field release. If the bootstrap window duration becomes a concern. |
| **Target Phase** | Phase 4D |
| **Status** | CLOSED FOR LEGACY BROWSER BOOTSTRAP |
| **Closure Evidence** | Phase 4D.3 rejected legacy Basic headers and required the provisioned local login path. Factory credential lifecycle remains recorded under residual deployment risks. |

---

## DHR-005 — Minimum Password Length (Resolved)

| Field | Value |
|-------|-------|
| **Date Recorded** | 2026-07-18 |
| **System Area** | HTTP Service Portal — Password Policy |
| **Current Implementation** | New human passwords require 12 through 128 bytes. New active SoftAP passphrases require 12 through 63 printable ASCII characters. Accepted existing credentials are not invalidated solely by the stronger change policy. |
| **Known Limitation** | Password strength still depends on operator choice; no arbitrary composition rule substitutes for length and throttling. |
| **Operator Rationale** | Acceptable for development. Brute-force risk is mitigated by trusted-network assumption. |
| **Current Exposure Assumptions** | No untrusted network access to the controller. |
| **Reconsideration Trigger** | Before production field release. When authentication throttling is implemented. |
| **Target Phase** | Phase 4D (Stronger Password Policy) |
| **Status** | CLOSED |
| **Closure Evidence** | Phase 4D.3 policy, parser, administration, and live credential-change validation. |

---

## DHR-006 — Authentication Throttling (Resolved)

| Field | Value |
|-------|-------|
| **Date Recorded** | 2026-07-18 |
| **System Area** | HTTP Service Portal — Abuse Prevention |
| **Current Implementation** | Phase 4D.3 enforces bounded peer, principal, and global pre-KDF throttle buckets; five failures within 60 seconds begin a 30-second cooldown with bounded exponential growth. KDF admission permits at most one active and one queued request. |
| **Known Limitation** | Bounded throttling reduces brute-force and resource exhaustion but does not make plaintext HTTP safe on hostile networks. |
| **Operator Rationale** | WPA2-PSK AP with single-client limit provides natural rate restriction on the AP interface. LAN brute-force is accepted for development. |
| **Current Exposure Assumptions** | Trusted local network. AP limited to 1 client. |
| **Reconsideration Trigger** | Before production field release. Before any untrusted network access. |
| **Target Phase** | Phase 4D (Abuse Throttling) |
| **Status** | CLOSED FOR CURRENT TRUSTED-LOCAL RELEASE |
| **Closure Evidence** | Pure admission tests and live six-attempt cooldown/recovery proof in `Phase 4D.3 Tests.md`. |

---

## DHR-007 — CSRF Mechanism (Resolved)

| Field | Value |
|-------|-------|
| **Date Recorded** | 2026-07-18 |
| **System Area** | HTTP Service Portal — Request Integrity |
| **Current Implementation** | Every authenticated state-changing browser request requires a valid RAM-only session, per-session `X-Argus-CSRF` synchronizer token, exact content type, validated same-origin `Origin` and `Host`, and SoftAP socket proof. CORS is denied by default. |
| **Known Limitation** | CSRF does not provide confidentiality or integrity against a packet observer capable of attacking the plaintext HTTP transport. |
| **Operator Rationale** | Single-client AP with WPA2-PSK provides adequate isolation. The portal is accessed from a dedicated device (phone), not a shared browser. Current POST endpoints are limited to password change. |
| **Current Exposure Assumptions** | Operator uses a dedicated device for portal access. No untrusted web content is loaded in the same browser session. |
| **Reconsideration Trigger** | Before production field release. Before motion-control POST endpoints are added (Phase 4B.3). |
| **Target Phase** | Phase 4D (CSRF Hardening) |
| **Status** | CLOSED |
| **Closure Evidence** | Phase 4D.3 pure/browser tests and live session-bearing POST rejection with `403 request_protection_failed`. |


**Update:** Acknowledging the existing service-entry/service-exit POST endpoints introduced in Phase 4B.3, while retaining Phase 4D hardening.
---

## DHR-008 — Credential Recovery and Factory Reset Not Unified

| Field | Value |
|-------|-------|
| **Date Recorded** | 2026-07-18 |
| **System Area** | HTTP Service Portal — Credential Lifecycle |
| **Current Implementation** | GPIO0/KEY1 enters persistent factory-credential AP-only recovery. Phase 4D.3 adds authenticated, capability-checked, recently reauthenticated, CSRF-protected recovery exit that clears only the marker and reboots to the preserved network disposition. Recovery remains intentionally distinct from factory reset. |
| **Known Limitation** | The factory recovery credential remains an offline manufacturing/provisioning concern and is not browser-rotatable. |
| **Operator Rationale** | Field personnel will not have serial-console access. Physically local AP recovery is required without coupling credential recovery to destructive factory reset. |
| **Current Exposure Assumptions** | The operator has physical access to KEY1/BOOT and the protected service AP. |
| **Reconsideration Trigger** | Before production field release. Before any deployment where serial console access is not guaranteed. |
| **Target Phase** | Phase 4D (Unified Reset) |
| **Status** | CLOSED FOR CURRENT RECOVERY CONTRACT |
| **Closure Evidence** | Phase 4D.2 physical entry plus Phase 4D.3 authenticated browser exit, clean reboot, and preserved active credential in `Phase 4D.3 Tests.md`. |


**Update:** Field personnel will not have serial-console access.
---

## DHR-009 — Comprehensive Security Audit Deferred

| Field | Value |
|-------|-------|
| **Date Recorded** | 2026-07-18 |
| **System Area** | HTTP Service Portal — Overall |
| **Current Implementation** | Security verified through independent source review, physical testing, and operator acceptance. No formal penetration test or comprehensive security audit performed. |
| **Known Limitation** | Source review and physical workflow verification do not constitute comprehensive security verification. |
| **Operator Rationale** | Formal security audit is appropriate after all command paths are implemented (Phase 4B complete) and before production release. |
| **Current Exposure Assumptions** | Development-phase deployment on trusted networks only. |
| **Reconsideration Trigger** | After Phase 4B completion. Before production field release. |
| **Target Phase** | Phase 4D (Formal Security Review) |
| **Status | OPEN — WIP PRESERVED, NOT ACCEPTED |
| **Closure Evidence** | Not yet applicable |

---

## DHR-010 — Browser Logout Behavior (Resolved)

| Field | Value |
|-------|-------|
| **Date Recorded** | 2026-07-18 |
| **System Area** | HTTP Service Portal — Logout |
| **Current Implementation** | Logout is an authenticated CSRF-protected POST that revokes the RAM-only server session before clearing the browser cookie. Mutation-by-GET logout and Basic-Auth browser-cache behavior are retired. |
| **Known Limitation** | Plain HTTP still permits session observation by a capable local packet attacker before logout. |
| **Operator Rationale** | Operator-verified and accepted as usable. The workaround (Cancel → Log in again) reliably clears credentials and re-prompts. A session-based logout (cookie/token) would provide reliable cross-browser behavior but adds significant complexity. |
| **Current Exposure Assumptions** | Single-operator usage. The quirk is a UX inconvenience, not a security vulnerability. |
| **Reconsideration Trigger** | If session-based authentication is implemented in Phase 4D. If multi-user access is required. |
| **Target Phase** | Phase 4D (Session-Based Auth) |
| **Status** | CLOSED |
| **Closure Evidence** | Live Phase 4D.3 logout invalidated protected navigation and required a fresh login. |

---

## DHR-011 — Always-Available Service AP and HTTP Portal on Commissioned Devices

| Field | Value |
|-------|-------|
| **System Area** | Network Lifecycle / Portal |
| **Phase Introduced** | Phase 4B.2 Corrections |
| **Status | OPEN — WIP PRESERVED, NOT ACCEPTED |
| **Target Phase** | Post-field-evaluation |
| **Operator Decision** | 2026-07-18 |

**Limitation:** Commissioned devices boot with the service AP and HTTP portal active by default in APSTA mode (`AP_DISCOVERABLE`). The portal is credential-protected but always advertised. No persistent enable/disable toggle exists.

**Rationale:** The operator has determined that field-accessible configuration requires the portal to be reachable without CLI service entry. This is an accepted temporary lifecycle policy for bench and field evaluation. AP visibility does not grant motor authority — the portal is read/config only. Motor commands require MQTT supervisory authority from the STA network path.

**Security posture:** The AP is WPA2-PSK protected. Human routes are SoftAP-only and use Phase 4D.3 local sessions, CSRF, and deny-by-default authorization. Public login/bootstrap routes remain bounded; protected endpoints require an authorized session. Plain HTTP and always-advertised AP exposure remain accepted only for the trusted-local development release.

**Deferred items:**
- Persistent AP enable/disable toggle per-device
- Final production default (AP on/off for commissioned devices)
- Decision on whether production deployments should default to AP-off

**Decision criteria:** Extended bench and field-use evaluation will determine whether the always-on AP is the correct production default or whether a toggle and/or default-off policy is needed.

---

## DHR-012 — App Partition Size Constraint

| Field | Value |
|-------|-------|
| **Date Recorded** | 2026-07-19 |
| **System Area** | Flash Partition Layout |
| **Phase Introduced** | Phase 4B.2 Final Corrections |
| **Status** | CLOSED |
| **Target Phase** | Phase 4B.3 (before significant UI additions) |
| **Operator Decision** | 2026-07-19 |

**Limitation:** The default ESP-IDF single-app partition table allocates a ~1 MB app partition. With the Phase 4B.2 binary, headroom is approximately 9%. Future UI HTML/CSS/JS additions and feature modules will consume remaining space.

**Impact:** Binary size must be monitored at each phase. If the app exceeds the partition, a custom partition table with a larger app partition must be created. The ESP32-S3 with 16 MB flash has ample total capacity — only the partition table allocation needs adjustment.

**Action Required:**
- Custom `partitions.csv` with increased app partition (recommend 2 MB minimum)
- Monitor `idf.py size` output at each commit
- Adjust NVS and factory partitions as needed to reclaim space

**Decision criteria:** Implement custom partition table before any phase that adds significant embedded HTML/CSS/JS or new feature modules.

**Closure:** The app-partition constraint is resolved by the dual 3 MB OTA layout introduced in Phase 4B.3.

**Closure evidence:** dual 3 MB OTA partitions; `otadata`; successful ESP32-S3 boot; final v5.5.3 build; Phase 4B.3 merge SHA (1fc356aee87f60199f4fc0ad1e0e09255ac53760).

---

## DHR-013 — Deferred Privileged Identity Modification

| Field | Value |
|-------|-------|
| **Date Recorded** | 2026-07-19 |
| **System Area** | Identity Provisioning / Portal |
| **Phase Introduced** | Phase 4B.2 |
| **Status** | CLOSED |
| **Target Phase** | Post-field-evaluation |
| **Operator Decision** | 2026-07-19 |

**Original limitation:** Identity fields (client_id, unit_id, device_name) lock after initial provisioning, and no authenticated portal workflow existed to clear that lock and recommission the controller.

**Phase 4B.6 disposition:** CLOSED for configuration factory reset. Authenticated `POST /api/factory-reset`, available only in safe browser-owned Local Service, now erases configuration slots, selector metadata, the provisioning high-water marker, identity configuration, and STA configuration through the durable reset transaction. Live acceptance proved `identity_provisioned:false`, truthful uncommissioned recovery, exact one-time identity reprovisioning, and rejection of a second mutation after relock.

Portal authentication in `argus_portal` is intentionally outside configuration-reset scope. Live acceptance proved the existing nondefault portal credential remained valid and default `admin/admin` remained rejected after reset.

Direct privileged mutation of a locked identity remains intentionally unsupported; the supported path is configuration factory reset followed by first provisioning. Unified configuration-and-portal-credential recovery remains open under the existing Phase 4D credential-recovery and security-hardening entries. CSRF, rate limiting, session management, and other recorded security limitations are unchanged by this closure.

---

## DHR-014 — Wi-Fi Connection Observability and Manual Reconnection

| Field | Value |
|-------|-------|
| **Date Recorded** | 2026-07-20 |
| **System Area** | Network Manager / API |
| **Phase Introduced** | Phase 4B.3a (initial pass reverted; corrected implementation accepted) |
| **Status** | CLOSED - COMPLETE AND PHYSICALLY ACCEPTED JULY 21, 2026 |
| **Target Phase** | Phase 4B.3a |
| **Operator Decision** | 2026-07-20 |

**Original limitation:** The device connected to Wi-Fi automatically without operator-visible failure classification, bounded authentication retry, or a manual recovery API.

**Correction history:** An initial Phase 4B.3a implementation was reverted to protect the Phase 4B.3 acceptance baseline. The feature was then reintroduced on `phase4b3a-wifi-observability`. Independent reviews found undefined dashboard fields, invalid embedded JavaScript, a synchronous disconnect/connect race, no preserved pending apply transaction, invalid authority pairs, placeholder tests, production-singleton mutation by a pure test, contradictory test-count evidence, stale build provenance, and premature readiness language. The current closure retains that history and does not imply first-pass correctness.

**Implemented on the feature branch:**
- Raw disconnect classification and operator-facing telemetry
- Bounded authentication retry and generation-checked timers
- Nonblocking generation-tagged configuration-apply/manual-reconnect transaction
- `POST /api/network/reconnect`
- Failure-evidence preservation until successful IP acquisition
- Pure authority-pair validation and isolated transaction tests

**Final acceptance:** Independent source review and the physical checklist were completed. Two hardware executions of diagnostic `t` each produced 142 distinct tests repeated three times, 426 passed, and 0 failed. Physical Tests 1-10 passed. Runtime acceptance applies to firmware commit `87eff30f36c9d264351ee939ff4061116c0dd128`; screenshot commit `62674ad26312cab040cbe6f72661b7d6f1593db5` is evidence-only; accepted feature-branch record head `4766d96d3845483828dfbfc1aa83eb77a72dd52e` contains the reviewed record. The Test 3 wording difference was non-blocking and remained truthful, understandable, and actionable.

---

## DHR-015 — Flat JSON Parser Scope and Ambiguity Hardening

| Field | Value |
|-------|-------|
| **Date Recorded** | 2026-07-20 |
| **System Area** | HTTP Configuration API / JSON Parser |
| **Phase Introduced** | Phase 4B.3a closure review |
| **Status** | DEFERRED |
| **Target Phase** | Before API schema expansion or untrusted-network exposure |

**Current scope:** `argus_json` is a bounded flat-object extractor used with request-size limits and a small accepted configuration schema. Nested objects and arrays are unsupported by design.

**Deferred hardening:** Define and enforce a duplicate-key policy, reject or correctly disambiguate escaped key-like text inside string values, make unsupported nested structures fail explicitly, and migrate to a strict parser if the API expands beyond the current flat schema.

**Acceptance boundary:** This closure does not replace the parser because no current schema correctness failure was established. Existing request-size limits, field bounds, content-type checks, and accepted flat schema remain required controls.

**Phase 4C note:** The MQTT heartbeat and command-envelope contracts do not use `argus_json`. They use a separate strict bounded decoder that rejects duplicate, unknown, nested, array, malformed, truncated, embedded-NUL, ambiguous, and trailing input. This does not close the existing HTTP configuration-parser debt described above.

---

## DHR-016 — MQTT Trusted-Network Security Boundary

| Field | Value |
|-------|-------|
| **Date Recorded** | 2026-07-22 |
| **System Area** | Embedded MQTT Broker / Supervisory Contract |
| **Phase Introduced** | Phase 4C acceptance |
| **Status** | CONTRACTED; IMPLEMENTATION DEFERRED |
| **Target Phase** | Later Phase 4D implementation subphase |

**Current scope:** Phase 4C assumes a trusted local network. Its broker-lifecycle session, command sequence, connection identity, client ID, heartbeat lease, and topic-ownership policy provide freshness, replay control, and deterministic local ownership. They do not authenticate a human or publisher cryptographically.

**Deferred hardening:** Define MQTT authentication and credential lifecycle, transport encryption, cryptographic publisher identity where required, authorization policy, rate limits, connection and publish abuse handling, denial-of-service bounds, security event observability, and a general adversarial security audit. Coordinate this work with the Phase 4D HTTP, TLS, credential, CSRF, and deployment-security design.

**Acceptance boundary:** Do not describe client IDs, command sessions, sequence counters, topic ACLs, or connection IDs as security credentials. Phase 4C acceptance proves protocol correctness on the trusted local network; it does not claim resistance to a hostile network participant.

**Phase 4D.1 disposition:** The machine-identity, credential, per-client permission, revocation, and audit boundaries are now defined in `PHASE_4D_SECURITY_CONTRACT.md`. No MQTT authentication, credential, authorization, or transport-encryption behavior was implemented, so this item remains open.

---

## DHR-017 - Certificate and Hostile-Network Security Deferred

| Field | Value |
|-------|-------|
| **Date Recorded** | 2026-07-22 |
| **System Area** | Browser and MQTT Transport Security |
| **Phase Introduced** | Phase 4D.1 contract acceptance |
| **Status** | DEFERRED TO NEXT RELEASE |
| **Target Phase** | Post-current-release security hardening |

**Deferred work:** HTTPS, browser certificate issuance, renewal, trust/pinning, and recovery; MQTT TLS and any selected client-certificate lifecycle; cryptographic publisher identity; hostile-network operation; remote Internet exposure; and broader penetration testing.

**Current boundary:** Human browser use is restricted to the protected controller AP. Non-TLS MQTT is restricted to the approved trusted local network. Wi-Fi link encryption is not end-to-end HTTP or MQTT transport security. No port forwarding, routed WAN, public-network, or hostile-peer deployment is accepted.

---

## DHR-018 - Residual Current-Release Security Risks

| Field | Value |
|-------|-------|
| **Date Recorded** | 2026-07-22 |
| **System Area** | Current-Release Deployment and Device Protection |
| **Phase Introduced** | Phase 4D.1 contract acceptance |
| **Status** | OPEN |
| **Target Phase** | Later Phase 4D subphases and next release, by risk class |

**Known residual risks:** Fleet-wide factory credential exposure; HTTP credential and session observation by an attacker already present on the AP; non-TLS MQTT credential and payload observation by an attacker on the trusted network; physical extraction because Phase 4D.2 software-stored NVS XTS keys are readable without eFuse/HMAC derivation or flash encryption; denial-of-service and resource-exhaustion limits; bounded local audit capacity; and the absence of trustworthy wall-clock time until a verified source is available.

**Acceptance boundary:** Phase 4D.2 adds protected logical storage, verifier, migration, provisioning, and local recovery foundations but does not claim physical-extraction resistance. Phase 4D.3/4D.3a add accepted local human authentication, authorization, truthful bounded audit lifecycle/pagination, and response-before-transition behavior. Machine authentication, encrypted HTTP/MQTT transport, physical-extraction resistance, hostile-network operation, and broad penetration acceptance remain open. Later implementations must preserve truthful deployment limits and fail closed where protected security state is unavailable or corrupt.

## Register Maintenance

This register is maintained as a living document. New entries are appended as limitations are identified. Entries are closed when the limitation is resolved, with closure evidence documenting the specific change, test, or audit that addressed the item.

All items must be reviewed before:
- Production field release
- Any architecture that permits controller access from untrusted networks
- Any configuration where the controller is accessible from publicly routed networks
