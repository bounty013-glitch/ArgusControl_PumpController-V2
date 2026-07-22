# Phase 4D.1 - Security Contract and Boundary Audit

**Status:** IN PROGRESS

**Branch:** `phase4d1-security-contract-and-boundaries`

**Accepted baseline:** `0a9f5f9c6ad48d66282c1a5df2bda4c0c51c1141`

**Accepted baseline tag:** `v2-phase4c-mqtt-hardware-verified`

**Firmware identity:** `v2-phase4d.1-dev`

## 1. Scope

Phase 4D.1 defines the security architecture and records the accepted Phase 4C security surface. It changes firmware identity and runtime labels only. Credential, login, authorization, MQTT-authentication, AP-password, recovery, and commissioning-console behavior are deferred to later bounded Phase 4D subphases.

## 2. Accepted Foundations

- Phase 4C dynamic MQTT root and exact topic ownership
- Broker-lifecycle command sessions and non-reusable connection identities
- Heartbeat lease, sequence, duplicate, replay, and result behavior
- Existing authority manager, command router, state manager, network manager, trajectory, and step generator
- Fail-operational communication loss and truthful open-loop telemetry
- Browser Local Service lifecycle and configuration factory reset

No security module may bypass these owners.

## 3. Sanitized Baseline Audit

### HTTP and Browser

- ESP-IDF HTTP server uses plain port 80 and `INADDR_ANY`; routes are reachable through AP and STA while both are active.
- All registered application handlers use one Basic-Auth check or, for logout, a Basic-Auth challenge response.
- One fixed human account model and a plaintext replacement password exist in the `argus_portal` NVS namespace.
- There are no cookie sessions, roles, permissions, CSRF tokens, Origin enforcement, login throttles, bounded lockouts, administrative revocation, or durable audit events.
- Current mutation routes include configuration save, STA reconnect, restart, Local Service entry/exit, browser commands, configuration factory reset, and portal-password change.

### Network and Commissioning

- Uncommissioned boot is AP-only; commissioned boot is APSTA `AP_DISCOVERABLE` with HTTP active.
- The Service AP has one client and uses a build-provisioned WPA secret.
- Normal browser commissioning already works with power, AP, and browser. Serial remains diagnostic and disaster-recovery tooling.
- Current configuration reset erases identity and STA configuration but intentionally preserves portal authentication.
- STA reconnect is not AP-access recovery. No browser-first credential recovery exists.

### Storage and Device Protection

- The only NVS partition is 24 KiB. Configuration uses dual slots plus selector/reset/high-water namespaces; portal authentication uses a separate plaintext namespace.
- Commissioned STA credentials are stored inside the unencrypted configuration payload and passed to the ESP-IDF Wi-Fi runtime.
- The AP secret is sourced from untracked local build configuration and compiled into the firmware image.
- Wi-Fi flash storage is enabled and no explicit RAM-only storage selection exists.
- Secure boot, flash encryption, and NVS encryption are disabled in the active ESP-IDF configuration. No `nvs_keys` partition exists.
- Existing NVS initialization may erase the whole NVS partition for selected initialization errors; this behavior is incompatible with later protected credential fail-closed requirements.
- ESP-IDF v5.5.3 provides PBKDF2-HMAC, SHA-256, runtime random generation, constant-time comparison, and NVS encryption/HMAC facilities. Actual key provisioning and device protections remain unimplemented.

### MQTT

- The broker listens on port 1883 on `INADDR_ANY`.
- CONNECT parsing accepts client ID and parses but does not authenticate username/password fields.
- Phase 4C topic policy limits external publication but is not identity authentication or per-client authorization.
- All subscriptions are currently read-only from a command-authority perspective but are not permission-filtered by machine identity.

### Recovery and Hardware

- Repository-defined GPIOs are motion outputs; no dedicated physical recovery input is implemented.
- Existing browser configuration reset, STA reconnect, serial factory reset, and full flash/NVS development tools are distinct and incomplete security-recovery mechanisms.
- The physical local recovery trigger must be selected and validated before Phase 4D.2; this plan does not assume BOOT, RESET, or an unused GPIO.

## 4. Anticipated Module Boundaries

Later subphases should introduce narrowly owned modules rather than expanding `argus_http_server.c` indefinitely:

| Module | Ownership |
|---|---|
| `argus_security_store` | Versioned users, roles, verifier metadata, machine records, protected AP secret, security epoch, corruption behavior |
| `argus_authn` | Human and machine credential verification, throttle decisions, reauthentication |
| `argus_authz` | Deny-by-default effective permissions, ceilings, scope, delegation |
| `argus_session_mgr` | Bounded browser sessions, expiry, revocation, CSRF state |
| `argus_audit` | Bounded queue and durable event ring with truthful time |
| `argus_http_security` | AP-interface gate, session cookie, CSRF/Origin/Host checks, route policy |
| `argus_mqtt_auth` | Machine CONNECT authentication and per-client subscribe/publish policy before Phase 4C admission |
| `argus_security_recovery` | Physically local network/security recovery orchestration after trigger selection |

These names are anticipated ownership boundaries, not source added by Phase 4D.1.

## 5. Storage and Provisioning Plan

Before implementation:

1. Measure user, role, machine-client, credential, and audit record sizes with explicit population limits.
2. Reserve separate protected security and bounded audit capacity rather than silently consuming the 24 KiB legacy NVS partition.
3. Select and test the ESP-IDF v5.5.3 NVS encryption scheme and key lifecycle on the exact ESP32-S3 hardware.
4. Define manufacturing provisioning that supplies AP recoverable material and writes the console verifier without placing plaintext in source, logs, generated evidence, or shell history.
5. Explicitly configure Wi-Fi storage so the application does not create uncontrolled plaintext credential copies.
6. Add migration that reads legacy records once, writes the new records atomically, verifies them, and removes legacy plaintext only after success.
7. Make corruption fail closed without automatic fleet bootstrap or unrelated namespace erase.

Human verifiers use measured PBKDF2-HMAC-SHA-256 parameters and unique salts. AP secrets use protected recoverable storage. Browser sessions remain RAM-only. Machine credentials are independently versioned and revocable. Audit uses its own bounded ring.

## 6. Session, Task, and Lock Boundaries

- Authentication and authorization never run in the motion ISR or broker socket critical section.
- A bounded security worker performs expensive verifier operations and storage updates.
- Session-table and security-metadata locks are short and never held while calling NVS, HTTP stop/start, network lifecycle, authority, router, state manager, MQTT publish, or audit storage.
- Audit uses a bounded queue and one storage owner task. Administrative mutations reserve audit capacity before commit; safety commands never wait for audit storage.
- Authorization captures identity, effective permission, scope, credential version, and security epoch, releases its lock, then revalidates the epoch at the protected operation boundary.
- Existing lifecycle and dispatch lock order remains authoritative. Security code enters existing public orchestrators rather than taking motion locks itself.

## 7. Authorization Boundaries

Every protected operation performs, in order:

1. AP/interface admission where required;
2. bounded request framing checks;
3. authentication/session or machine-connection lookup;
4. CSRF and origin checks for browser mutation;
5. effective permission and scope check;
6. recent reauthentication when required;
7. security-epoch revalidation;
8. existing operation-specific lifecycle/authority/state policy;
9. existing router dispatch for commands;
10. bounded audit/result publication.

Rejection before step 9 produces zero command dispatch. Login and authority acquisition are never commands.

## 8. Route Migration

- `/` becomes a session-aware redirect.
- `/login` replaces browser Basic Auth.
- `/operate` replaces `/controls` and presents role-filtered operation.
- `/commission` absorbs existing identity, Wi-Fi, password, reset, diagnostics, and later administrative pages.
- Existing APIs migrate behind a centralized AP/session/CSRF/permission policy; no unguarded compatibility route remains.
- Machine clients use assigned MQTT or future versioned API permissions and never browser pages.

The current release contract makes all human browser traffic AP-only. HTTP is not an administrative interface on STA.

## 9. Recovery Decision

Network-access recovery, configuration factory reset, and full security reset are separate transactions. Each preserves machine state and never creates authority or a command. The repository does not establish a physical trigger, so hardware selection, debounce, local-presence proof, and physical validation are required before Phase 4D.2. Serial remains last-resort disaster recovery, not normal commissioning.

## 10. Later Subphase Progression

1. **Phase 4D.2:** select/validate physical recovery trigger; establish protected storage, provisioning, record schemas, migration, and credential-verifier seam.
2. **Phase 4D.3:** implement human authentication, sessions, CSRF, throttling, roles, permissions, `/login`, `/operate`, and `/commission` route policy.
3. **Phase 4D.4:** implement machine enrollment, MQTT authentication, per-client permissions, and security audit integration while preserving Phase 4C protocol semantics.
4. **Future release:** HTTPS, MQTT TLS/certificates, hostile-network operation, and broader penetration testing.

Subphase labels may be adjusted by a later approved plan, but boundaries must remain explicit and independently accepted.

## 11. Test Strategy

- Pure record, parser, permission-ceiling, delegation, session-expiry, throttle, CSRF, audit, migration, corruption, and recovery-policy tests
- Production-seam tests proving authorization precedes dispatch and every rejection dispatches zero times
- AP-vs-STA route admission tests
- Storage power-loss, capacity, wear, and encryption/key-lifecycle tests
- MQTT CONNECT authentication and per-client topic-policy tests before Phase 4C callbacks
- Three complete controller suites with production-state isolation
- Live browser and MQTT tests only in the subphase that implements behavior
- Physical trigger and recovery validation only after hardware selection and explicit authorization

## 12. Current Disposition

Phase 4D.1 acceptance requires the contract, audit record, identity-only build, unchanged Phase 4C tests and boundaries, sanitized secret scan, and eligible non-motion pure-suite evidence. It does not accept security implementation, transport encryption, recovery hardware, physical security, or penetration resistance.
