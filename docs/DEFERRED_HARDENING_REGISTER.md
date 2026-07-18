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
| **Current Implementation** | Portal is accessible through both the Service AP and the commissioned STA/LAN interface. `esp_http_server` binds to `INADDR_ANY:80` in APSTA mode. |
| **Known Limitation** | No interface-level access restriction. Any device on the commissioned LAN can reach the portal if they know the controller's STA IP. |
| **Operator Rationale** | Dual access is intentional and useful during development. The operator explicitly accepted LAN portal access as a secondary access path. Socket-level filtering was attempted (open_fn + getsockname, open_fn + getpeername, per-handler + getpeername) but lwip on ESP32 returns `0.0.0.0` for all peer/local address queries on accepted sockets in this project configuration. |
| **Current Exposure Assumptions** | Controller is on a trusted local network. Not Internet-exposed. No routing from public networks to the controller. |
| **Reconsideration Trigger** | Before production field release. Before any architecture permits controller access from an untrusted or publicly routed network. |
| **Target Phase** | Phase 4D (Security Hardening) |
| **Status** | OPEN |
| **Closure Evidence** | Not yet applicable |

---

## DHR-002 — HTTP Basic Auth Over Unencrypted Transport

| Field | Value |
|-------|-------|
| **Date Recorded** | 2026-07-18 |
| **System Area** | HTTP Service Portal — Authentication |
| **Current Implementation** | HTTP Basic Auth (`Authorization: Basic <base64>`). Credentials are base64-encoded, not encrypted. Transport is plaintext HTTP on port 80. |
| **Known Limitation** | Credentials are visible to any device that can observe network traffic between the client and the controller (AP or LAN). Base64 is encoding, not encryption. |
| **Operator Rationale** | Acceptable for direct WPA2-PSK AP connections (encrypted at the WiFi layer). LAN exposure is accepted for development. HTTPS (TLS) requires certificate management infrastructure not yet designed. |
| **Current Exposure Assumptions** | WPA2-PSK AP provides link-layer encryption for AP clients. LAN is trusted. No traffic exits the local network. |
| **Reconsideration Trigger** | Before production field release. Before any untrusted network path exists between client and controller. |
| **Target Phase** | Phase 4D (HTTPS / TLS) |
| **Status** | OPEN |
| **Closure Evidence** | Not yet applicable |

---

## DHR-003 — Plaintext Password Storage in NVS

| Field | Value |
|-------|-------|
| **Date Recorded** | 2026-07-18 |
| **System Area** | HTTP Service Portal — Credential Storage |
| **Current Implementation** | Portal password stored as plaintext string in NVS namespace `argus_portal`, key `pw`. |
| **Known Limitation** | Anyone with physical access to the ESP32's flash can read the password via serial tools or flash dump. NVS encryption is not enabled in this configuration. |
| **Operator Rationale** | Physical access to the ESP32 constitutes complete device compromise regardless of password storage format. Hashed storage (bcrypt/argon2) is preferred but adds complexity and is not justified for a development-phase portal. |
| **Current Exposure Assumptions** | Controller is in a physically controlled environment. Physical access implies operator trust. |
| **Reconsideration Trigger** | Before production field release. Before any deployment where physical access is not equivalent to operator trust. |
| **Target Phase** | Phase 4D (Hashed Credential Storage) |
| **Status** | OPEN |
| **Closure Evidence** | Not yet applicable |

---

## DHR-004 — Bootstrap Credentials

| Field | Value |
|-------|-------|
| **Date Recorded** | 2026-07-18 |
| **System Area** | HTTP Service Portal — Initial Authentication |
| **Current Implementation** | Default credentials are `admin`/`admin`. Forced password change is implemented: the portal serves a password-change page instead of the dashboard when the default password is active. After password change, the default `admin` password is explicitly rejected (defense-in-depth in `check_auth()`). |
| **Known Limitation** | The bootstrap credentials are hardcoded and publicly documented. Between device boot and first portal access, anyone who connects can authenticate with the default. The forced-change mechanism requires the operator to navigate to the portal and complete the form — it does not prevent default-credential authentication on API endpoints before that occurs. |
| **Operator Rationale** | The time window between boot and first operator access is short in practice. The forced-change flow is sufficient for development. |
| **Current Exposure Assumptions** | Operator accesses the portal promptly after enabling service mode. No untrusted actors have access during the bootstrap window. |
| **Reconsideration Trigger** | Before production field release. If the bootstrap window duration becomes a concern. |
| **Target Phase** | Phase 4D |
| **Status** | OPEN |
| **Closure Evidence** | Not yet applicable |

---

## DHR-005 — Minimum Password Length

| Field | Value |
|-------|-------|
| **Date Recorded** | 2026-07-18 |
| **System Area** | HTTP Service Portal — Password Policy |
| **Current Implementation** | Minimum password length is 4 characters. Maximum is 64. Cannot reuse "admin". No complexity requirements. |
| **Known Limitation** | Short passwords are vulnerable to brute-force attacks. No authentication throttling (see DHR-006) compounds this. |
| **Operator Rationale** | Acceptable for development. Brute-force risk is mitigated by trusted-network assumption. |
| **Current Exposure Assumptions** | No untrusted network access to the controller. |
| **Reconsideration Trigger** | Before production field release. When authentication throttling is implemented. |
| **Target Phase** | Phase 4D (Stronger Password Policy) |
| **Status** | OPEN |
| **Closure Evidence** | Not yet applicable |

---

## DHR-006 — No Authentication Throttling

| Field | Value |
|-------|-------|
| **Date Recorded** | 2026-07-18 |
| **System Area** | HTTP Service Portal — Abuse Prevention |
| **Current Implementation** | No rate limiting, lockout, or bounded backoff on failed authentication attempts. |
| **Known Limitation** | Unlimited brute-force attempts are possible from any client that can reach the portal. |
| **Operator Rationale** | WPA2-PSK AP with single-client limit provides natural rate restriction on the AP interface. LAN brute-force is accepted for development. |
| **Current Exposure Assumptions** | Trusted local network. AP limited to 1 client. |
| **Reconsideration Trigger** | Before production field release. Before any untrusted network access. |
| **Target Phase** | Phase 4D (Abuse Throttling) |
| **Status** | OPEN |
| **Closure Evidence** | Not yet applicable |

---

## DHR-007 — No CSRF Mechanism

| Field | Value |
|-------|-------|
| **Date Recorded** | 2026-07-18 |
| **System Area** | HTTP Service Portal — Request Integrity |
| **Current Implementation** | No CSRF tokens. POST endpoints accept requests with valid Basic Auth credentials regardless of origin. |
| **Known Limitation** | A malicious page loaded in the same browser could potentially forge requests to the portal if Basic Auth credentials are cached. |
| **Operator Rationale** | Single-client AP with WPA2-PSK provides adequate isolation. The portal is accessed from a dedicated device (phone), not a shared browser. Current POST endpoints are limited to password change. |
| **Current Exposure Assumptions** | Operator uses a dedicated device for portal access. No untrusted web content is loaded in the same browser session. |
| **Reconsideration Trigger** | Before production field release. Before motion-control POST endpoints are added (Phase 4B.3). |
| **Target Phase** | Phase 4D (CSRF Hardening) |
| **Status** | OPEN |
| **Closure Evidence** | Not yet applicable |

---

## DHR-008 — Credential Recovery and Factory Reset Not Unified

| Field | Value |
|-------|-------|
| **Date Recorded** | 2026-07-18 |
| **System Area** | HTTP Service Portal — Credential Lifecycle |
| **Current Implementation** | Portal credentials are stored in NVS namespace `argus_portal`. Factory reset via the diagnostic menu erases the commissioning NVS namespace but may not erase the portal credential namespace. Full NVS erase (`idf.py erase-flash` or `nvs_flash_erase()`) resets both. `--erase-otadata` does NOT erase the portal credential namespace. |
| **Known Limitation** | Operator may lose portal access if they forget the password and do not have serial console access. No in-product credential recovery mechanism exists. Factory reset and credential reset are not unified. |
| **Operator Rationale** | Serial console access is always available during development. A formal credential recovery flow (e.g., button-hold reset) is deferred to product hardening. |
| **Current Exposure Assumptions** | Operator always has serial console access. |
| **Reconsideration Trigger** | Before production field release. Before any deployment where serial console access is not guaranteed. |
| **Target Phase** | Phase 4D (Unified Reset) |
| **Status** | OPEN |
| **Closure Evidence** | Not yet applicable |

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
| **Status** | OPEN |
| **Closure Evidence** | Not yet applicable |

---

## Register Maintenance

This register is maintained as a living document. New entries are appended as limitations are identified. Entries are closed when the limitation is resolved, with closure evidence documenting the specific change, test, or audit that addressed the item.

All items must be reviewed before:
- Production field release
- Any architecture that permits controller access from untrusted networks
- Any configuration where the controller is accessible from publicly routed networks
