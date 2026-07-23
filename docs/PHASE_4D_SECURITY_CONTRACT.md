# Phase 4D Security Contract

**Contract status:** ACCEPTED FOR IMPLEMENTATION by Phase 4D.1 on July 22, 2026

**Applies to firmware line:** `v2-phase4d.1-dev` and later Phase 4D subphases

## 1. Purpose and Authority

This contract defines authentication, authorization, operating-authority, credential, session, recovery, and audit boundaries for the current Argus Pump Controller release. Later Phase 4D subphases implement these boundaries. Phase 4D.1 does not implement them.

The controller remains authoritative for machine state. Security code decides who may request an operation; it does not replace the authority manager, command router, state manager, trajectory engine, step generator, or network lifecycle manager.

The only normal motion path remains:

`authenticated identity -> permission check -> explicit authority request -> existing command envelope -> argus_cmd_router_dispatch() -> authority manager -> state manager -> trajectory -> step generator`

No login, role, permission, session, recovery, or interface module may call a motion layer directly.

## 2. Trust Boundaries

| Boundary | Current-release trust decision |
|---|---|
| Controller process and RAM | Trusted execution boundary, subject to firmware defects and physical extraction risk. |
| Protected local AP | Required browser boundary. AP membership is necessary but never sufficient for console authorization. |
| Browser on the AP | Untrusted until a human identity authenticates and every operation is authorized. |
| Client/plant network | Trusted local deployment boundary for non-TLS MQTT only; not trusted for browser administration. |
| MQTT connection | Untrusted until a machine identity authenticates. Client ID, connection ID, command session, sequence, and heartbeat are not credentials. |
| Serial/USB | Development and last-resort disaster recovery only. It is not normal commissioning. |
| Internet, routed WAN, port-forwarded network, or hostile LAN/AP peer | Unsupported and prohibited for this release. |
| Physical flash and board access | Not protected by current repository configuration; secure boot, flash encryption, and NVS encryption are not active. |

Normal commissioning requires only power, the controller's AP, and a browser. A healthy new or replacement controller must not require serial access.

## 3. Threat Assumptions and Explicit Limitations

The current release uses plain HTTP and non-TLS MQTT. It may operate only on approved protected local networks with no Internet exposure, port forwarding, or untrusted peers.

- HTTP credentials, session cookies, CSRF tokens, and payloads are not transport encrypted. An attacker already able to observe or manipulate AP traffic may capture or alter them.
- The cookie `Secure` attribute cannot be used over plain HTTP. `HttpOnly` and `SameSite=Strict` remain required but do not replace transport encryption.
- MQTT credentials and payloads are not end-to-end encrypted. Wi-Fi link encryption is not MQTT TLS.
- A fleet-wide factory human-readable credential creates fleet-wide exposure if learned. Separate per-device records and salts do not remove that shared-secret risk.
- Physical extraction remains possible unless later device protections are implemented and verified.
- Bounded tables, queues, request limits, and throttles reduce but do not eliminate denial-of-service risk.
- Local audit storage is finite and can be lost with flash failure or destructive service.
- The controller may lack trustworthy wall-clock time. It must not invent timestamps.

HTTPS, browser certificates, certificate issuance, renewal, trust, pinning, and recovery; MQTT TLS; MQTT client certificates; cryptographic publisher identity; hostile-network operation; remote Internet exposure; and broad penetration testing are deferred to the next release.

## 4. Credential Domains

### 4.1 AP Join Credential

The AP join credential allows a device to join the controller's Wi-Fi AP. The ESP32 Wi-Fi runtime needs the recoverable secret, so a one-way verifier alone is insufficient.

The credential is a protected recoverable secret. It must not appear in source, browser assets, logs, diagnostics, MQTT, audit details, exports, screenshots, shell commands, or reports. New values must satisfy WPA passphrase syntax and the product policy. The later UI shall accept 12 through 63 printable ASCII characters for a new value; an already commissioned shorter standards-valid legacy value may remain in use until explicitly changed. Raw 64-hex PSKs are not accepted by the browser workflow.

### 4.2 Argus Console Credential

The Argus console credential authenticates Argus Personnel to the commissioning console. Only a salted verifier is stored. Recoverable plaintext is neither required nor permitted after provisioning.

The standard factory AP and console values may initially be human-readable equals by product policy, but they are always separate records with independent versions, rotation, recovery, and audit history. Changing one never changes the other.

### 4.3 Human Account Credentials

Each human account has its own verifier record, credential version, enabled state, role assignments, scope, and revocation state. Password changes invalidate that account's sessions. Disabling or deleting an account revokes all of its sessions and authority requests.

### 4.4 Machine Credentials

Each HMI, Node-RED supervisor, service tool, backup interface, or future Argus Command client is a distinct machine identity with an independently revocable credential. A machine credential is shown or transferred only through the approved enrollment process. Replacing one client must not rotate human passwords or unrelated machine credentials.

### 4.5 Non-Credentials

Browser session IDs are bearer session credentials but are not passwords. MQTT client IDs, connection IDs, Phase 4C command sessions, heartbeat leases, sequences, and command IDs are freshness, correlation, and replay controls. They do not authenticate a publisher cryptographically.

## 5. Credential Verification and Storage

ESP-IDF v5.5.3 includes Mbed TLS PBKDF2-HMAC, SHA-256, `esp_fill_random()`, and `mbedtls_ct_memcmp()`. The initial console and human-password verifier shall use PBKDF2-HMAC-SHA-256 with:

- a unique random salt of at least 16 bytes per credential;
- a 32-byte derived verifier;
- a versioned algorithm identifier and cost field;
- a cost selected by measurement on the production ESP32-S3, targeting 250 to 500 ms for an interactive verification while proving watchdog, heap, and concurrent control behavior remain acceptable;
- no cost reduction below the measured release floor without a recorded security decision;
- constant-time verifier comparison;
- immediate zeroization of plaintext and intermediate buffers;
- rehash-on-success when an older accepted cost is below the current policy.

Argon2 and bcrypt are not selected because no repository-supported ESP-IDF v5.5.3 facility was established. The selection may change only through a documented, benchmarked contract revision.

Storage classes are separate:

| Class | Required treatment |
|---|---|
| Human password verifier | Protected security storage; salted one-way verifier only. |
| Recoverable AP secret | Encrypted protected storage; readable only by the network/security owner. |
| Machine-client verifier/secret metadata | Protected security storage; independent credential version and revocation. |
| Browser sessions | Bounded RAM table only; never persisted across boot. |
| User, role, permission, and identity metadata | Versioned non-secret records with integrity and security epoch. |
| Audit events | Dedicated bounded append/ring storage; no reusable credential content. |

The existing 24 KiB `nvs` partition is not sufficient evidence of capacity or protection for this model. Before credential implementation, Phase 4D must size and add protected security and audit storage, select the ESP-IDF NVS encryption scheme, define key provisioning, and test power-loss recovery and wear. `CONFIG_NVS_ENCRYPTION`, secure boot, and flash encryption are currently off; no document may claim otherwise.

The current Wi-Fi driver uses flash storage by default. Later implementation must prevent unplanned duplicate plaintext storage by explicitly selecting Wi-Fi storage behavior and making the protected Argus record authoritative.

If protected storage is corrupt or unavailable, authentication and protected mutations fail closed. The firmware must not silently restore a public bootstrap credential, erase unrelated namespaces, or claim successful recovery. Existing motion state and fail-operational behavior remain untouched; a sanitized security-storage fault is exposed and physically local recovery is required.

## 6. Identity Models

### 6.1 Human User Record

A human record contains a stable non-secret user ID, normalized login name, display name, enabled flag, protected-identity flag, scope, role IDs, direct permission constraints if supported, verifier metadata, credential version, security epoch, and boot-relative/optional wall-time lifecycle metadata. It never contains a recoverable password.

Argus Personnel records are protected product identities. A Client Admin cannot create, modify, demote, delete, impersonate, or grant one.

### 6.2 Machine Client Record

A machine record contains a stable machine ID, display name, client type, enabled flag, allowed transports/interfaces, roles/permissions, topic/API scope, verifier metadata, credential version, enrollment actor, and revocation state. It is not a human account and cannot use human browser pages.

### 6.3 Runtime Identities

Human accounts, browser sessions, machine clients, MQTT sockets/connections, Phase 4C broker sessions, and heartbeat leases remain distinct records. Links between them are explicit IDs, never object or token substitution.

## 7. Built-In Security Levels

1. **ARGUS PERSONNEL** is the highest protected product level. It owns full commissioning, recovery, identity, security, firmware, protected configuration, calibration, and machine-client administration. It cannot be created or granted by Client Admin.
2. **CLIENT ADMIN** administers client users and custom roles below the Client Admin ceiling. It delegates only permissions it possesses and is allowed to delegate. It cannot alter controller identity, factory recovery policy, protected Argus identities, or the Argus level. Network access is available only when explicitly granted.
3. **SUPERVISOR** performs scoped operational control. It may manage ordinary Operator/Viewer access only when explicitly granted and may not define roles above its ceiling.
4. **OPERATOR** views state, requests operating authority, and issues approved operational commands. It has no user administration or protected configuration access.
5. **VIEWER** is read-only and cannot acquire command authority.
6. **MACHINE IDENTITY** is an identity type, not an automatic privilege level. Every machine receives an explicit bounded role/permission set and transport/topic scope.

## 8. Permission Matrix

Legend: `Y` built-in; `G` may be explicitly granted within the role ceiling; `N` prohibited; `A` assigned per machine enrollment and bounded by the enrolling actor.

| Capability | Argus Personnel | Client Admin | Supervisor | Operator | Viewer | Machine Identity |
|---|:---:|:---:|:---:|:---:|:---:|:---:|
| View operating status | Y | Y | Y | Y | Y | A |
| Request operating authority | Y | G | Y | Y | N | A |
| Issue ordinary motion commands | Y | G | Y | Y | N | A |
| Issue software E-stop | Y | G | Y | Y | N | A |
| Reset software E-stop | Y | G | Y | Y | N | A |
| Acknowledge alarms | Y | G | Y | Y | N | A |
| Manage users below caller ceiling | Y | Y | G, Operator/Viewer only | N | N | A, service-tool only |
| Manage Client Admins (`manage_client_admins`) | Y | G, only when explicitly assigned by Argus Personnel | N | N | N | N |
| Manage custom roles below caller ceiling | Y | Y | N | N | N | A, service-tool only |
| Enroll machine clients | Y | Y | N | N | N | A, service-tool only |
| Revoke machine clients | Y | Y | N | N | N | A, service-tool only |
| View audit history | Y | Y, client scope | G, operational scope | N | N | A, scoped |
| Manage AP/network access | Y | G, explicit | N | N | N | A, Argus service-tool only |
| Change AP password | Y | G only with Manage Network Access | N | N | N | A, Argus service-tool only |
| Manage client-network configuration | Y | G, explicit | N | N | N | A, Argus service-tool only |
| Manage MQTT client configuration | Y | G, explicit | N | N | N | A, Argus service-tool only |
| Modify device/customer identity | Y | N | N | N | N | A, Argus service-tool only |
| Modify protected controller configuration | Y | N | N | N | N | A, Argus service-tool only |
| Commission the controller | Y | N | N | N | N | A, Argus service-tool only |
| Calibrate the pump | Y | N | N | N | N | A, Argus service-tool only |
| Manage firmware | Y | N | N | N | N | A, Argus service-tool only |
| Invoke non-destructive network recovery | Y | G, explicit | N | N | N | A, Argus service-tool only |
| Perform full security reset | Y | N | N | N | N | A, factory service-tool only |

Software E-stop remains a software command, not a safety-rated physical E-stop. Authentication and `ISSUE_SOFTWARE_ESTOP` authorization occur before the request reaches the existing safety-preemption route. Once authorized and decoded, audit or result-publication failure must not delay the E-stop.

## 9. Permission Ceilings and Delegation

Authorization is deny by default. UI visibility never grants permission. Every protected operation checks permission in firmware at the operation boundary.

- A caller can grant only a permission it currently possesses, that its role marks delegable, within its own scope, and below its immutable ceiling.
- A custom role's effective permissions are the intersection of its definition, creator's delegable permissions, target scope, and current policy.
- Client Admin can create only Client Admin-or-lower identities and cannot create or promote Argus Personnel.
- Supervisor user-management grants, when present, are limited to ordinary Operator/Viewer accounts and existing roles within scope.
- Protected Argus identities and permissions are immutable to all client-level actors.
- Permission or role changes increment a security epoch and revoke affected sessions and pending authority requests.
- Machine enrollment cannot produce a credential with greater permissions or broader topic/API scope than the enrolling actor may delegate.
- Ordinary Client Admin authority is downward-only: every Client Admin may manage permitted Supervisor, Operator, and Viewer accounts beneath the Client Admin ceiling.
- Creating, promoting, modifying, disabling, or deleting a Client Admin requires the separate `manage_client_admins` capability.
- Argus Personnel always possess `manage_client_admins` and may assign or revoke it for an individual Client Admin within client scope.
- `manage_client_admins` is protected and non-delegable by Client Admins. A Client Admin cannot grant, copy, inherit through a custom role, or delegate that capability merely because the Client Admin possesses it.
- `manage_client_admins` never permits creating, modifying, impersonating, demoting, deleting, or delegating Argus Personnel access.

## 10. Authentication, Authorization, and Operating Authority

These are independent gates:

1. **Authentication:** establish the human or machine identity.
2. **Authorization:** evaluate the requested operation against effective permissions and scope.
3. **Operating authority:** use the existing authority manager to determine which approved interface controls the machine.

Login never acquires operating authority. Authority acquisition never emits Start, Stop, Unlock, setpoint, E-stop reset, Recover, or any other machine command. Browser/MQTT session loss preserves Phase 4C fail-operational behavior.

A replacement interface authenticates, receives permissions, reads a coherent machine/authority snapshot, explicitly requests authority, and then uses the existing router. It never inherits an earlier browser session, MQTT socket, heartbeat lease, or authority generation.

## 11. Browser Session Contract

The current release uses plain HTTP only on the protected local AP.

- `/login` accepts credentials and creates a bounded opaque session. Login failures do not disclose whether an account exists.
- Session IDs contain at least 256 random bits from the ESP-IDF runtime RNG, are placed only in an `HttpOnly; SameSite=Strict; Path=/` cookie, omit `Domain`, and are never placed in URLs, page source, logs, audit details, or `localStorage`.
- The `Secure` cookie attribute is unavailable over HTTP; this limitation is displayed truthfully in security documentation, not hidden.
- The controller stores only a constant-time-comparable token digest in a bounded RAM table. Initial capacity is eight sessions total and two per human account, subject to measured memory validation.
- Idle expiry is 15 minutes; absolute expiry is eight hours; recent reauthentication for sensitive operations is five minutes.
- Logout is an authenticated CSRF-protected POST that revokes the server-side record before clearing the cookie.
- Administrators may revoke sessions within their ceiling. Password, enabled-state, role, permission, AP-password, or relevant policy changes revoke affected sessions immediately.
- Reboot revokes all browser sessions.

Every state-changing browser request requires a per-session synchronizer CSRF token in a non-cookie request header, exact expected content type, valid session, and same-origin `Origin`/`Host` checks. CORS is deny by default. `SameSite` is defense in depth, not the only CSRF control.

Authentication attempts are bounded by both account key and source address. Five failures in 60 seconds trigger a 30-second delay; repeated windows double the delay to a maximum of five minutes. No lockout is permanent. Throttle time uses monotonic boot-relative time, generates audit events, and never blocks physically local recovery or an already authorized software E-stop.

## 12. Browser Routes and AP-Only Enforcement

Logical human interfaces are:

- `/login` - public login form and login operation, AP only;
- `/operate` - authenticated operating view, AP only;
- `/commission` - authenticated administrative/commissioning console, AP only and permission-filtered.

`/` redirects to `/login` or `/operate`. The existing `/controls`, configuration pages, password page, and Basic-Auth logout are migration sources, not parallel security paths.

Viewer receives read-only operation. Operator receives permitted backup operation. Supervisor receives scoped supervisory functions. Client Admin receives downstream user/role and explicitly delegated network/MQTT administration. Argus Personnel receives the full commissioning console. Machine identities use assigned APIs or MQTT permissions, never human pages.

All human browser routes and APIs are AP-only in this release. At minimum, credential, user, role, permission, device identity, recovery, firmware, AP/network, MQTT configuration, commissioning, calibration, protected configuration, and audit mutation routes reject requests received through STA before parsing a sensitive body. Existing `INADDR_ANY` listeners make an explicit local-interface check mandatory.

The commissioning console includes device/customer identity, AP configuration, client-network configuration, AP-password management, users, roles, permissions, machine enrollment/revocation, credential rotation, MQTT security configuration, firmware, recovery, calibration/protected settings, diagnostics, audit history, and non-secret export. Each panel and API operation has its own permission check.

## 13. AP Password Change

The later AP-password workflow is atomic and independent of console-password changes:

1. Authenticate an identity with `MANAGE_NETWORK_ACCESS`; Argus Personnel always possess it and Client Admin only by explicit grant.
2. Require reauthentication within five minutes.
3. Confirm the current AP credential without logging or retaining it.
4. Enter and confirm the new credential.
5. Validate WPA syntax and product policy.
6. Warn that AP clients and the active browser will disconnect.
7. Reserve an audit record and atomically store the protected recoverable secret.
8. Never expose old/new values in logs, diagnostics, MQTT, exports, browser source, or audit details.
9. Restart only required AP/network components.
10. Preserve machine state, configured target, trajectory, state-manager state, driver state, E-stop, fault, and operating authority unless the existing network lifecycle independently requires authority transition.
11. Revoke affected browser sessions.
12. Require reconnection with the new AP credential.
13. Record actor, credential record/version, outcome, and truthful time fields without either password.

Changing a web account password, changing the factory Argus console credential, network-access recovery, configuration factory reset, and full security reset are separate operations.

## 14. Recovery Contract

Recovery is browser-first and physically local. Three operations remain distinct:

1. **Network-access recovery** restores the AP credential record to the approved factory AP policy and restores local AP reachability. It preserves device/customer identity, client network configuration, users, roles, calibration, machine clients, machine state, target, trajectory, driver, E-stop, fault, and authority. It does not start or stop motion.
2. **Configuration factory reset** is the existing explicit operation that erases commissioned identity and STA configuration while preserving the security domain. It remains stationary, Local-Service, and policy gated.
3. **Full security reset** is an Argus Personnel/factory-service operation that resets security records and sessions. It is not an implicit consequence of network recovery or configuration reset and must not alter motion, clear E-stop/fault, or acquire authority.

The repository proves no dedicated physical recovery input. Existing GPIO definitions are motion outputs; serial/USB and software APIs are the only implemented recovery paths. Selection and physical validation of a locally accessible recovery trigger is a required hardware/product decision before Phase 4D.2 implementation. No BOOT button, reset pin, or invented GPIO is authorized by this contract.

When selected, the trigger must be sampled through a bounded, debounced, boot/local-presence procedure that cannot be invoked remotely or during accidental transients. Serial remains disaster recovery only.

## 15. MQTT Security and Phase 4C

Phase 4C dynamic topics, ownership-before-storage/delivery, broker sessions, heartbeat leases, sequence arithmetic, duplicate caching, replay rejection, command results, one router entry, retained truth, fail-operational loss, and open-loop telemetry remain unchanged.

Each MQTT client later authenticates as an enrolled machine identity before subscribe, heartbeat, or publish admission. Per-client policy defines exact allowed interface, subscribe filters, heartbeat permission, command permissions, and topic scope. Authentication and permission rejection occur before Phase 4C topic policy, retained storage, subscriber delivery, heartbeat mutation, sequence consumption, or dispatch.

The current broker accepts CONNECT packets without validating supplied username/password and binds port 1883 to all active interfaces. Until later Phase 4D implementation, it remains restricted to the approved trusted local-network boundary. Non-TLS MQTT credentials will be observable to a network attacker; this is accepted only as a documented current-release limitation, not a solved risk.

## 16. Audit Event Contract

Audit storage is a bounded append/ring ledger with monotonic event sequence and explicit overflow accounting. Every event contains:

- schema version and event sequence;
- random boot ID and monotonic uptime;
- optional synchronized wall time plus validity/source, never fabricated time;
- actor type and stable actor ID, or sanitized anonymous/source identity for failed login;
- session or machine ID reference, never its credential;
- interface and bounded peer metadata;
- stable event type, target type/ID, outcome, and reason code;
- relevant permission/security/authority generation where applicable;
- bounded non-secret detail fields.

Required events include successful/failed login, rate-limit activation, logout, session creation/revocation, user lifecycle, role/permission change, machine enrollment/revocation, AP-password change, network change, identity change, firmware action, recovery action, protected configuration change, authority acquisition/denial/transfer/release, and security-storage failure.

Audit records never contain passwords, verifiers, salts where disclosure adds risk, session/CSRF tokens, machine secrets, MQTT passwords, Authorization headers, raw request bodies, or reusable credentials.

Security-sensitive administrative mutations reserve audit capacity before commit and record the outcome afterward. If durable audit reservation is unavailable, those mutations fail closed. Software E-stop, logout, and safety actions are never delayed or reversed by audit failure; they emit best-effort bounded diagnostics and a storage-failure indication.

## 17. Sanitized Authorization Examples

- An authenticated Viewer requests Start: authorization rejects before authority lookup or dispatch.
- An Operator logs in: session creation succeeds, but no machine authority or command is generated.
- An Operator explicitly requests browser authority: permission and current snapshots are checked, then the existing service-entry/authority manager decides.
- A Client Admin without delegated network permission submits an AP change: reject before reading the credential body.
- A Client Admin attempts to create Argus Personnel: reject due immutable ceiling and audit the denial.
- A stale MQTT command from an authenticated machine: Phase 4C returns stale/conflict behavior with zero redispatch.
- A revoked HMI reconnects: machine authentication fails before heartbeat lease or topic policy.
- Protected storage is corrupt: do not restore public defaults; deny protected operations and expose sanitized recovery guidance.

## 18. Explicit Exclusions

Phase 4D.1 does not implement credentials, users, roles, sessions, login APIs, CSRF, throttling, audit storage, AP-password mutation, MQTT authentication, recovery actions, HTTPS, TLS, certificates, firmware management, calibration, or physical recovery. It performs no motion, pump, hose, fluid, chemical, pressure, flow, load, process, endurance, or penetration acceptance.
