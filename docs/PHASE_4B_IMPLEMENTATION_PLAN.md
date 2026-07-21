# Phase 4B — Controller-Hosted Local Browser Portal

**Status:** Phase 4B.1 through Phase 4B.3, including Phase 4B.3a, are COMPLETE AND ACCEPTED. Phase 4B.4 Step 1 is ACCEPTED at `eb1a6cc`; Step 2 authenticated HTTP admission and router dispatch is IN PROGRESS.

This document defines the implementation plan for Phase 4B of the Argus V2
Pump Controller firmware. Phase 4B adds an embedded HTTP server and
mobile-friendly browser portal hosted on the Controller's Service AP.

## Operator Decisions (Binding)

| # | Decision | Answer |
|---|----------|--------|
| Q1 | E-stop preemption | Move to Phase 4D |
| Q2 | HTTP in COMMISSIONED_STA | No |
| Q3 | Portal assets | Embed in firmware |
| Q4 | Commissioning in UNCOMMISSIONED_AP | Yes, without service entry |
| Q5 | Max HTTP connections | 4 (see rationale below) |

## Architectural Amendments

### A1. Self-Stop Deadlock Avoidance

An HTTP request handler must never synchronously call
`argus_http_server_stop()` — `esp_http_server` dispatches handlers on
its own task, and stopping from within deadlocks or destroys the calling
context.

**Resolution:** HTTP handlers that need lifecycle transitions (service
entry/exit in 4B.3+) will post events to the network manager task via
`argus_net_mgr_post_event()`. The net_mgr task executes the stop/start
from its own context. Phase 4B.1 has no mutation handlers, so this
applies only to future phases.

### A2. Browser Disconnect During Transition

A browser disconnect, refresh, or phone sleep must not abort an accepted
service transition, restore MQTT authority, or create ambiguous ownership.

**Resolution:** Once `argus_net_mgr_request_service()` is called, the
transition runs to completion regardless of the HTTP connection state.
The transition is owned by the net_mgr task context, not the HTTP session.
The browser's TCP disconnect is invisible to the orchestrator.

### A3. Coordinated Reboot for Apply/Reset/Exit

Config apply, service exit, and factory reset must use coordinated
safe-stop and reboot orchestration. HTTP handlers must not bypass that
architecture.

**Resolution (4B.2/4B.3):** Future commit/reboot handlers will call
`argus_net_mgr_request_service_exit()` which performs a controlled stop,
authority revocation, and `esp_restart()`. No direct `esp_restart()` from
HTTP handlers.

### A4. NETWORK_FAULT Portal Behavior

**Resolution:** HTTP server does NOT run in NETWORK_FAULT. The AP may
not be functional in this state. The network manager does not call
`argus_http_server_start()` when entering NETWORK_FAULT.

### A5. HTTP Server Lifecycle Serialization

**Resolution:** `s_lifecycle_mutex` serializes all start/stop calls.
`s_server` handle is read/written only under this mutex. `is_running()`
queries under the mutex. Start is idempotent (returns OK if running).
Stop is idempotent (returns OK if stopped). No double server instances.

### A6. Browser-Origin Protections

**Resolution:** No `Access-Control-Allow-Origin` header emitted (no
permissive CORS). `X-Content-Type-Options: nosniff` on all responses.
Method validation on all handlers (405 for non-GET in 4B.1).
`Cache-Control: no-store` on all API responses. Full security hardening
(CSRF, rate limiting, session management) remains Phase 4D.

### A7. Authority Invariant

ONE PUMP, ONE COMMAND AUTHORITY. Phase 4B.1 is read-only. No handler
acquires, modifies, or releases authority. No handler dispatches motion
commands. The HTTP server is a passive observer of system state.

### Q5 Rationale: Max HTTP Connections = 4

One associated AP station does not mean one TCP socket. A mobile browser
opens multiple parallel connections: one for the HTML page, concurrent
XHR requests for /api/status and /api/identity, and potentially a
prefetch or favicon request. Four connections provides headroom for one
browser client without over-committing DRAM (~2KB per connection on
esp_http_server). `lru_purge_enable=true` evicts the oldest connection
if the limit is reached.

## Independent Review Corrections (Post-3ae9b22)

Four blocking findings were identified in independent review and corrected:

### C1. HTTP Shutdown Deadlock (Finding 1)

**Root cause:** `status_get_handler` called `argus_net_mgr_get_snapshot()` which
takes `s_net_mutex`. `request_service()` called `argus_http_server_stop()` while
holding `s_net_mutex`. `httpd_stop()` waits for active handlers -> deadlock.

**Fix (first review):** Handler replaced `argus_net_mgr_get_snapshot()` with
lock-free query functions. HTTP start/stop moved outside `s_net_mutex`.

**Fix (second review):** `argus_net_mgr_get_snapshot()` restored in the handler
because HTTP stop is now outside `s_net_mutex` (handler can safely take it).
Lock-free queries removed as they constituted a data race.

### C2. Service Portal Access Control (Finding 2)

**Root cause:** In APSTA mode, `esp_http_server` binds to `INADDR_ANY:80`,
accepting connections through both AP and STA interfaces.

**Attempted fix 1:** `open_fn` session callback using `getsockname()` to compare
local address to AP netif IP. **Failed:** lwip returns `0.0.0.0` for
`getsockname()` on accepted sockets bound to `INADDR_ANY`.

**Attempted fix 2:** `open_fn` using `getpeername()` for client IP subnet check.
**Failed:** lwip also returns `0.0.0.0` for `getpeername()` in the `open_fn`
callback (connection not yet fully established).

**Attempted fix 3:** Per-handler check using `httpd_req_to_sockfd()` +
`getpeername()`. **Failed:** lwip returns `0.0.0.0` even on the fully-established
request socket. This is a platform limitation — ESP-IDF's lwip does not populate
peer/local addresses on accepted TCP sockets.

**Final fix:** HTTP Basic Auth credential protection. All endpoints require
`Authorization: Basic` header. Default credentials `admin`/`admin` with forced
password change on first access. Password stored in NVS namespace `argus_portal`.
This is stronger than subnet filtering (credentials vs. network position) and
works regardless of interface. See `PHASE_4B1_WALKTHROUGH.md` for full details.

**Operator acceptance:** LAN-accessible portal access accepted as useful during
development. Credential protection is the preferred long-term approach.

### C3. Unsafe Browser Rendering / DOM Injection (Finding 3)

**Root cause:** `row()` function injected device-supplied values into
`innerHTML` with only JSON escaping. A `client_id` like `<img onerror=...>`
would execute as HTML.

**Fix:** Added `h()` HTML escape function in portal JavaScript that replaces
`& < > " '` with HTML entities. All values in `row()` now pass through `h()`.
Identity subtitle uses `textContent` (inherently DOM-safe).

### C4. Failed Stop Reported as Success (Finding 4)

**Root cause:** `argus_http_server_stop()` cleared `s_server = NULL` and
returned `ESP_OK` even when `httpd_stop()` failed. This would allow a
duplicate `httpd_start()` against a possibly-still-running server.

**Fix:** On `httpd_stop()` failure, `s_server` is preserved (not cleared).
The error is propagated to the caller. `is_running()` continues to return
`true`. A subsequent `start()` returns "already running" (idempotent),
which is the safest behavior for an unknown state.

### Supporting Corrections

- Authority snapshot error checked in status handler.
- URI handler registration checked with rollback on failure.
- Removed stale "bounded" shutdown claim from header doc.
- Exposed `json_escape` as test seam (`argus_http_test_json_escape`).
- Added 1 pure test: json_escape safety (8 edge cases).
- Removed 2 non-pure tests (lifecycle observation, secret exclusion) during
  second review — these read live singletons/NVS, not stack-local pure.
- Test count: 19 distinct test cases (18 Phase 4A + 1 Phase 4B.1).

---

## 1. Current-Source Baseline Audit

Audited against `main` at merge commit `bab4217`.

| Area | Finding |
|------|---------|
| HTTP server | **Zero** HTTP server code exists. `esp_http_server` must be added. |
| Browser owner enum | `ARGUS_AUTH_OWNER_BROWSER` (2) exists in `argus_authority_mgr.h`. Unused. |
| Portal command source | `ARGUS_CMD_SRC_LOCAL_SERVICE_PORTAL` (3) exists in `argus_cmd_parser.h`. Unused. |
| Service AP | Ready at `192.168.4.1`, WPA2-PSK, MAC-derived SSID `Argus-Service-XXYYZZ`, max 1 client. |
| NVS masking | `argus_nvs_config_mask()` replaces `sta_pass` with `"********"`. |
| Config validation | `argus_nvs_config_validate()` enforces schema, field lengths, password policy. |
| Staged commit | `argus_nvs_core_commit()` with selector-based LKG protection and readback verification. |
| Factory reset | `argus_nvs_config_factory_reset()` with `reset_pending` marker for power-loss safety. |
| Service entry | `argus_net_mgr_request_service(owner)` orchestrates full 10-callback transition. |
| Service exit | `argus_net_mgr_request_service_exit(owner)` with controlled stop, authority revoke, reboot. |
| Command dispatch | `argus_cmd_router_dispatch()` validates source, authority generation, permission. |
| Binary headroom | ~858 KB of 1 MB partition used (18% free ≈ 190 KB). |
| DRAM | 242 KB available at boot. |
| Diagnostic boundary | `CONFIG_ARGUS_DIAGNOSTIC_MODE` gates CLI/tests. Production builds have no CLI. |

---

## 2. Governing Architectural Rule

**ONE PUMP, ONE COMMAND AUTHORITY.**

The browser must **never** gain motion authority merely because:

- A device associates with the Service AP.
- A browser opens the portal URL.
- A status page is viewed.
- A commissioning form is edited or submitted.
- A transient HTTP request is received.

Browser motion commands are permitted **only** after an explicit
service-entry request completes and authority is:

```
LOCAL_SERVICE / BROWSER
```

MQTT, CLI, and browser motion authority must **never coexist**.

---

## 3. Deferred E-Stop Preemption — Placement Recommendation

### Current state

Concurrent E-stop preemption (splitting `s_state_mutex`, atomic latches,
ISR-level verification) was explicitly deferred from Phase 4A. Source
comment in `app_main.c` line 296:

```c
/* estop_timer_cb removed: concurrent E-stop preemption testing deferred to Phase 4B */
```

### Recommendation: Move to Phase 4D

**Reason:** E-stop preemption is orthogonal to the browser portal. It
requires:

1. ISR-level mutex splitting in `argus_state_mgr.c`.
2. Atomic latch implementation in step generator or trajectory engine.
3. Hardware timing measurement (oscilloscope verification of pulse
   cessation latency).
4. Concurrent motion + E-stop test sequences that are best exercised
   with the production HMI generating real MQTT motion commands.

Phase 4D end-to-end acceptance is the correct architectural boundary
because it combines HMI integration, active-motion handoff, and
safety-critical hardening.

If the operator approves this placement, the source comment in
`app_main.c` will be updated from "deferred to Phase 4B" to
"deferred to Phase 4D."

> [!IMPORTANT]
> **AWAITING OPERATOR APPROVAL** on E-stop preemption placement
> (Phase 4B vs. Phase 4D).

---

## 4. HTTP Server Lifecycle

### Network modes where HTTP server runs

| Network Mode | HTTP Server | Available Pages |
|---|---|---|
| `BOOTSTRAP` | **OFF** | N/A — transient startup state |
| `UNCOMMISSIONED_AP` | **ON** | Identity, commissioning form. No motion controls. |
| `COMMISSIONED_STA` | **OFF** | MQTT handles production telemetry (see §13 Q2) |
| `AP_DISCOVERABLE` | **ON** | Read-only status, commissioning form. No motion. |
| `SERVICE_TRANSITION` | **OFF** | Transition in progress — server stopped before STA disconnect |
| `SERVICE_AP_ONLY` | **ON** | Full portal: status, commissioning, motion controls |
| `NETWORK_FAULT` | **OFF** | No AP interface available |

### Task configuration

| Parameter | Value |
|---|---|
| Task name | `httpd_task` (ESP-IDF managed) |
| Stack size | 6144 bytes |
| Priority | 5 |
| Max URI handlers | 12 |
| Max connections | 1 (matches AP `max_connection`) |
| Recv timeout | 5 seconds |
| Send timeout | 5 seconds |

### Synchronization

The HTTP server does not introduce any new mutexes. All state access
uses existing read-only snapshot APIs:

- `argus_state_mgr_get_snapshot()`
- `argus_authority_mgr_get_snapshot()`
- `argus_net_mgr_get_snapshot()`
- `argus_nvs_config_get()` / `argus_nvs_config_mask()`
- `argus_identity_get()`

Mutations route through existing serialized APIs:

- `argus_cmd_router_dispatch()` — motion commands
- `argus_net_mgr_request_service()` — service entry
- `argus_net_mgr_request_service_exit()` — service exit
- `argus_nvs_config_commit()` — config apply

### Startup and shutdown

- **Start:** Network manager calls `argus_http_server_start()` when
  entering `UNCOMMISSIONED_AP`, `AP_DISCOVERABLE`, or `SERVICE_AP_ONLY`.
- **Stop:** Network manager calls `argus_http_server_stop()` before
  entering `SERVICE_TRANSITION` or `COMMISSIONED_STA`. Bounded 2-second
  shutdown timeout. If stop fails, log error and proceed (non-fatal).

---

## 5. Browser Authority Entry

### Sequence

1. Browser sends `POST /api/service/enter`.
2. HTTP handler calls `argus_net_mgr_request_service(ARGUS_AUTH_OWNER_BROWSER)`.
3. Orchestrator executes the full 10-callback transition:
   - `prepare_transition` → `request_normal_stop` → `verify_stopped` →
     `stop_broker` → `verify_broker_stopped` → `disconnect_sta` →
     `verify_sta_disconnected` → `verify_sta_ip_released` →
     `set_wifi_ap_only` → `verify_ap_active` → `verify_machine_safe` →
     `grant_local(BROWSER)`
4. HTTP handler returns JSON result to browser.

### Edge cases

| Scenario | Behavior |
|---|---|
| Transition timeout (>10s) | Abort transition, return `503 Service Unavailable` |
| Browser disconnects during transition | Abort transition, revert to previous state |
| Page refresh during transition | Return `409 Conflict` with transition status |
| Duplicate submission | Reject with `409 Conflict` if already transitioning |
| Stale authority generation | Reject with `409 Conflict` |
| Already LOCAL_SERVICE/BROWSER | Return `200 OK` (idempotent) |

---

## 6. Local Motion Controls

Minimum browser-accessible controls:

| Control | Command Type | Payload |
|---|---|---|
| Set target speed | `ARGUS_CMD_TYPE_SET_TARGET` | `target_rpm_milli` (integer) |
| Start | `ARGUS_CMD_TYPE_START` | None |
| Normal stop | `ARGUS_CMD_TYPE_STOP_NORMAL` | None |
| Unlock | `ARGUS_CMD_TYPE_UNLOCK` | None |
| Software E-stop | `ARGUS_CMD_TYPE_ESTOP` | None |
| Reset E-stop | `ARGUS_CMD_TYPE_RESET_ESTOP` | None |
| Recover | `ARGUS_CMD_TYPE_RECOVER` | None |
| Status refresh | (GET endpoint) | None |

Every browser motion command becomes an `argus_command_envelope_t`:

```c
argus_command_envelope_t env = {
    .source = ARGUS_CMD_SRC_LOCAL_SERVICE_PORTAL,
    .command_type = <parsed from request>,
    .authority_generation = <current generation>,
    .target_rpm_milli = <parsed if SET_TARGET>,
    .forward = true
};
esp_err_t result = argus_cmd_router_dispatch(&env);
```

**No HTTP handler may directly call `argus_state_mgr_*` functions.**

---

## 7. Commissioning Workflow

### Endpoints

| Action | Method | URI | Notes |
|---|---|---|---|
| Read config | GET | `/api/config` | Returns masked config (`sta_pass` = `"********"`) |
| Stage config | POST | `/api/config/stage` | Stores in RAM buffer, validates fields |
| Validate staged | POST | `/api/config/validate` | Runs `argus_nvs_config_validate()` |
| Apply and reboot | POST | `/api/config/apply` | Commits via `argus_nvs_config_commit()`, reboots |
| Exit without change | POST | `/api/service/exit` | Revokes authority, reboots |
| Factory reset | POST | `/api/factory-reset` | Calls `argus_nvs_config_factory_reset()`, reboots |

### Password security

The STA password must **never** appear in:

- HTML source
- JavaScript source
- JSON GET responses
- Console logs
- Telemetry payloads
- NVS configuration read-back

Password handling:

- `POST /api/config/stage` accepts `sta_pass` as a write-only field.
- `GET /api/config` returns `"********"` via `argus_nvs_config_mask()`.
- If staged password is `"********"`, the existing password is preserved
  (not overwritten with the mask string — already enforced by
  `argus_nvs_core_commit()` rejection of `ARGUS_CONFIG_MASK_STRING`).

### Power-loss safety

Handled by existing NVS dual-slot architecture:

- `argus_nvs_config_commit()` writes to inactive slot, verifies readback,
  updates selector only on success.
- `argus_nvs_config_factory_reset()` sets `reset_pending` marker before
  erasing, clears marker after completion.

---

## 8. Portal UI/UX

### Design principles

- **Mobile-first**: Primary use case is operator's phone connected to
  Service AP.
- **Embedded assets**: All HTML, CSS, and JavaScript embedded in firmware
  as C string constants or `EMBED_FILES`. No CDN or Internet dependency.
- **No blind entry**: Every destructive action (apply, reset, service
  entry/exit) requires explicit confirmation dialog.
- **No auto-submit**: No form submission triggered by typing, page load,
  or refresh.

### Portal sections

| Section | Visibility | Content |
|---|---|---|
| Device Identity | Always | UID, firmware version, service SSID |
| Network Status | Always | Mode, STA status, AP status, broker status |
| Authority Status | Always | Mode, owner, generation |
| Machine State | Always | State, target/applied/generated speeds (truthful labels) |
| Commissioning | UNCOMMISSIONED or LOCAL_SERVICE | Config form with validation |
| Motion Controls | LOCAL_SERVICE/BROWSER only | Start, stop, speed, E-stop |
| Service Controls | AP_DISCOVERABLE or SERVICE_AP_ONLY | Enter/exit service, factory reset |

### Speed display labels

All speed values must use truthful labels that distinguish controller
intent from physical motor movement:

| Label | Meaning |
|---|---|
| Target Speed | Requested setpoint (operator intent) |
| Applied Speed | Trajectory-limited speed sent to pulse generator |
| Generated Speed | Active rate output by GPTimer scheduling |
| *(Note)* | "Generated pulses are not proof of physical shaft motion" |

---

## 9. HTTP and Browser Security Boundaries

### Phase 4B protections

| Control | Value |
|---|---|
| **HTTP Basic Auth** | **All endpoints require `Authorization: Basic` header** |
| **Default credentials** | **`admin`/`admin`, forced password change on first access** |
| **Credential storage** | **NVS namespace `argus_portal`, keys `pw` and `pw_set`** |
| Max request body | 1024 bytes |
| Max field length | Per NVS constants (CLIENT_ID: 32, SSID: 32, etc.) |
| Content-Type | `application/json` required for all POST |
| Method restrictions | GET for reads, POST for mutations |
| Numeric parsing | Strict integer parsing, reject NaN/Inf/overflow |
| String escaping | HTML and JSON escaping for all user-supplied strings |
| Request timeout | 5 seconds (recv + send) |
| Duplicate requests | Reject if transition in progress |
| Cache-Control | `no-store` for config and authority responses |
| Password masking | `argus_nvs_config_mask()` on all GET config responses |
| Credential hygiene | All credential buffers zeroed after use |
| AP security | WPA2-PSK, single client, no open fallback |
| WAN exposure | None — AP-only, no routing, no Internet gateway |
| Interface filtering | **Deferred** (lwip platform limitation, see C2) |

### Deferred to Phase 4D

| Control | Reason |
|---|---|
| CSRF tokens | Single-client AP with WPA2 provides adequate isolation for Phase 4B |
| Rate limiting | Single-client AP limits request rate naturally |
| Session management | Single-client AP makes sessions redundant for Phase 4B |
| Security audit | Comprehensive review after all command paths are implemented |

---

## 10. Endpoint Contract

| Method | URI | Allowed Modes | Required Authority | Request | Response | Mutation | Idempotent | Failure Codes | Secret Rule |
|---|---|---|---|---|---|---|---|---|---|
| GET | `/` | UNCOMM, AP_DISC, SVC_AP | Basic Auth | None | HTML portal | No | Yes | 200, 401 | None |
| GET | `/api/status` | UNCOMM, AP_DISC, SVC_AP | Basic Auth | None | JSON status | No | Yes | 200, 401, 500 | None |
| GET | `/api/identity` | UNCOMM, AP_DISC, SVC_AP | Basic Auth | None | JSON identity | No | Yes | 200, 401, 500 | None |
| **POST** | **`/api/portal-password`** | **Any** | **Basic Auth** | **JSON** | **JSON result** | **NVS write** | **No** | **200, 400, 401, 500** | **Password write-only** |
| GET | `/api/config` | UNCOMM, AP_DISC, SVC_AP | Basic Auth | None | JSON masked config | No | Yes | 200, 401, 500 | Password masked |
| POST | `/api/config/stage` | UNCOMM, SVC_AP | NONE or LOCAL_SERVICE | JSON fields | JSON result | RAM only | No | 200, 400, 401, 403 | Password write-only |
| POST | `/api/config/validate` | UNCOMM, SVC_AP | NONE or LOCAL_SERVICE | None | JSON validation | No | Yes | 200, 400, 401, 403 | None |
| POST | `/api/config/apply` | UNCOMM, SVC_AP | NONE or LOCAL_SERVICE | None | JSON + reboot | NVS commit | No | 200, 400, 401, 403, 500 | Password committed |
| POST | `/api/service/enter` | AP_DISC | Basic Auth (becomes BROWSER) | None | JSON result | Auth transition | No | 200, 401, 409, 503 | None |
| POST | `/api/service/exit` | SVC_AP | LOCAL_SERVICE | None | JSON + reboot | Auth revoke | No | 200, 401, 403, 500 | None |
| POST | `/api/command` | SVC_AP | LOCAL_SERVICE/BROWSER | JSON envelope | JSON result | Motion cmd | No | 200, 400, 401, 403, 500 | None |
| POST | `/api/factory-reset` | SVC_AP | LOCAL_SERVICE | None | JSON + reboot | NVS erase | No | 200, 401, 403, 500 | None |

### JSON schemas (planned)

**POST /api/command request:**
```json
{
  "command": "start" | "stop" | "unlock" | "estop" | "reset_estop" | "recover" | "set_target",
  "target_rpm_milli": 1000,
  "generation": 4
}
```

**POST /api/config/stage request:**
```json
{
  "client_id": "acme_corp",
  "unit_id": "pump_001",
  "device_name": "Main Process Pump",
  "sta_ssid": "FactoryWiFi",
  "sta_pass": "SecurePassword123"
}
```

---

## 11. Verification Plan

### Pure unit tests (planned)

- HTTP JSON request parsing and field validation
- Envelope construction from JSON command payload
- Authority permission checks for `ARGUS_CMD_SRC_LOCAL_SERVICE_PORTAL`
- Config staging field validation (using existing mock NVS)
- Password masking verification in JSON responses
- Content-Type enforcement
- Field length enforcement
- Numeric boundary testing

### Mock authority-transition tests (planned)

- Browser service-entry via mock orchestrator (`mock_orchestration_ctx_t`)
  with `ARGUS_AUTH_OWNER_BROWSER`
- Failure injection at each transition stage
- Timeout and abort behavior
- Duplicate request rejection

### Build verification (planned)

- Full clean build, 0 errors, 0 warnings
- Binary size within partition budget (<1 MB)

### Operator physical browser acceptance (planned)

- Phone connects to Service AP
- Portal loads in mobile browser
- Read-only status displays correctly
- Commissioning form validates and commits
- Service entry transitions authority to LOCAL_SERVICE/BROWSER
- Motion controls work under browser authority
- Non-owner rejection verified (CLI and MQTT probes rejected)
- Service exit reboots cleanly
- Post-reboot commissioned recovery confirmed

### Deferred to Phase 4C

- Dynamic MQTT topic namespace
- MQTT payload schemas and retain policy

### Deferred to Phase 4D

- CSRF/session hardening
- Rate limiting
- Active-motion HMI-to-local authority handoff
- Concurrent E-stop preemption (if approved by operator)
- Comprehensive security audit

---

## 12. Implementation Subsections

### Recommended ordering

```
4B.1 → 4B.2 → 4B.3 → 4B.4 → 4B.5 → 4B.6
```

**Rationale:** Server lifecycle must exist before any endpoint.
Commissioning is needed before motion (device must be commissioned).
Service entry is needed before motion commands. UI ties everything
together. Final acceptance is last.

---

### 4B.1 — HTTP Server Lifecycle and Read-Only Status/Identity API

**Scope:** Add `esp_http_server` dependency. Implement server start/stop
lifecycle tied to network modes. Implement `GET /api/status`,
`GET /api/identity`, and `GET /` (minimal HTML shell).

**Files:**
- `main/CMakeLists.txt` — add `esp_http_server` to `REQUIRES`
- NEW `main/argus_http_server.c` — server lifecycle and handlers
- NEW `main/argus_http_server.h` — public API
- `main/argus_net_mgr.c` — call server start/stop on mode transitions

**APIs introduced:**
```c
esp_err_t argus_http_server_init(void);
esp_err_t argus_http_server_start(void);
esp_err_t argus_http_server_stop(void);
bool argus_http_server_is_running(void);
```

**Lock impact:** None — read-only snapshot APIs only.

**Failure behavior:** Server start failure logged, non-fatal. Network
manager proceeds without HTTP.

**Tests:** Build verification. Manual browser load test on phone.

**Stop gate:** `GET /api/status` returns valid JSON in browser on
Service AP.

---

### 4B.2 — Commissioning Staging and Validation API

**Scope:** Implement `GET /api/config`, `POST /api/config/stage`,
`POST /api/config/validate`, `POST /api/config/apply`,
`POST /api/factory-reset`.

**Files:**
- `main/argus_http_server.c` — add commissioning handlers

**APIs introduced:** Internal staged config buffer (module-static
`argus_config_payload_t`). Uses existing `argus_nvs_config_validate()`,
`argus_nvs_config_commit()`, `argus_nvs_config_factory_reset()`.

**Lock impact:** None — staged config is local to HTTP server module.
Commit goes through existing NVS serialization.

**Failure behavior:** Validation errors returned as JSON with specific
field names. Commit failure returns 500. Mask-string password rejection
returns 400.

**Tests:** Pure field validation tests. Mock NVS commit/readback tests.

**Stop gate:** Commissioning form submits, validates, commits, and
reboots successfully from phone browser.

---

### 4B.3 — Explicit Browser Service-Entry and Authority Ownership

**Status:** COMPLETE AND ACCEPTED on July 21, 2026, including the Phase 4B.3a Wi-Fi observability and recovery close-out. See [Phase 4B.3 and 4B.3a - Final Acceptance](Phase%204B.3%20and%204B.3a%20-%20Final%20Acceptance.md).

**Scope:** Implement `POST /api/service/enter` and
`POST /api/service/exit`.

**Files:**
- `main/argus_http_server.c` — add service entry/exit handlers

**APIs used:**
- `argus_net_mgr_request_service(ARGUS_AUTH_OWNER_BROWSER)`
- `argus_net_mgr_request_service_exit(ARGUS_AUTH_OWNER_BROWSER)`

**Lock impact:** None new — uses existing lock hierarchy through
orchestrator.

**Failure behavior:** Transition timeout returns 503. Mode violation
returns 403. Already-transitioning returns 409.

**Tests:** Mock orchestrator service-entry test with `BROWSER` owner.
Failure injection at each stage. Timeout and abort behavior.

**Stop gate:** Browser can enter and exit local service via HTTP API.
Authority snapshot confirms `LOCAL_SERVICE/BROWSER`.

---

### 4B.4 — Browser-Local Motion Command API

**Status:** ACTIVE - Step 1 accepted; Step 2 authenticated HTTP admission and router dispatch in progress. Connected-motor and physical acceptance remain pending. See [Phase 4B.4 Implementation Plan](Phase%204B.4%20-%20Implementation%20Plan%20-%20Browser-Local%20Motion%20Command%20API.md).

**Scope:** Implement `POST /api/command`. Parse JSON, construct
`argus_command_envelope_t` with `source=LOCAL_SERVICE_PORTAL`, dispatch
through router.

**Files:**
- `main/argus_http_server.c` — add command handler

**APIs used:**
- `argus_cmd_router_dispatch()`
- `argus_authority_mgr_get_snapshot()` (for current generation)

**Lock impact:** None new — dispatch goes through existing
`s_dispatch_mutex`.

**Failure behavior:** JSON parse error returns 400. Authority rejection
returns 403. Dispatch error returns 500.

**Tests:** Envelope construction tests. Authority rejection tests for
wrong source/owner combinations.

**Stop gate:** Browser can start/stop motor through HTTP commands while
holding `LOCAL_SERVICE/BROWSER` authority.

---

### 4B.5 — Embedded Mobile UI

**Scope:** Create embedded HTML/CSS/JS portal. Mobile-first responsive
layout. All assets embedded in firmware. Status display, commissioning
form, motion panel, service controls.

**Files:**
- NEW `main/argus_portal.h` — embedded HTML/CSS/JS as C string constants
- `main/argus_http_server.c` — serve portal from `GET /`

**Lock impact:** None — static content.

**Failure behavior:** N/A (static content serving).

**Tests:** Visual acceptance on operator's phone. Responsive layout
verification.

**Stop gate:** Portal renders correctly on mobile browser. All panels
functional. Motion panel visible only with `BROWSER` authority.

---

### 4B.6 — Exit, Apply/Reboot, Reset, and Physical Acceptance

**Scope:** Final integration testing. Exit/apply/reboot workflows.
Factory reset. Full operator physical acceptance matching the acceptance
criteria in §11.

**Files:** Integration across all Phase 4B files.

**Tests:** Full operator physical acceptance sequence:
1. Connect phone to Service AP.
2. Load portal, verify status display.
3. Commission device (stage, validate, apply, reboot).
4. Enter service mode, verify browser authority.
5. Execute motion controls, verify command dispatch.
6. Run non-owner rejection probes.
7. Exit service, verify reboot and recovery.
8. Factory reset, verify uncommissioned state.

**Stop gate:** All physical acceptance scenarios pass. Documentation
updated with operator-supplied evidence.

---

## 13. Open Decisions Requiring Operator Approval

> [!IMPORTANT]
> The following decisions require operator input before implementation
> begins. Each includes a recommendation.

### Q1. E-stop preemption placement

**Question:** Should concurrent E-stop preemption testing remain in
Phase 4B or move to Phase 4D?

**Recommendation:** Move to Phase 4D. E-stop preemption is orthogonal
to the browser portal and requires ISR-level work, hardware timing, and
production HMI integration.

### Q2. HTTP server in COMMISSIONED_STA mode

**Question:** Should the HTTP server run when the device is in normal
`COMMISSIONED_STA` mode for read-only status?

**Recommendation:** No. The browser portal is a local service tool, not
a production monitoring interface. MQTT handles production telemetry.
Running HTTP in STA mode would expose the portal on the production
network, which is a Phase 4D security consideration.

### Q3. Embedded assets vs. SPIFFS

**Question:** Should portal assets be embedded as C string constants or
stored on a SPIFFS partition?

**Recommendation:** Embedded C string constants. SPIFFS adds flash
partition complexity and a second failure domain. The portal should be
small enough (<50 KB) to embed directly. If the portal grows beyond
embedded limits during development, revisit this decision.

### Q4. Browser commissioning in UNCOMMISSIONED_AP

**Question:** Should commissioning be allowed via browser in
`UNCOMMISSIONED_AP` mode without requiring service entry?

**Recommendation:** Yes. This is the primary use case for initial device
setup. The device has no MQTT supervisor in this mode, so no authority
conflict exists. The browser does not need `LOCAL_SERVICE` authority to
commission an uncommissioned device.

### Q5. Maximum concurrent HTTP connections

**Question:** Should the HTTP server support more than 1 concurrent
connection?

**Recommendation:** 1. This matches the AP `max_connection=1` setting.
A single phone is the intended client. Multiple connections would
complicate authority tracking without adding value.
