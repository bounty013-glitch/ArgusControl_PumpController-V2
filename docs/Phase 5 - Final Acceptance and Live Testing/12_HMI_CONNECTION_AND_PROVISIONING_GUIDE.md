# HMI Connection and Provisioning Guide

**Document status:** DRAFT OPERATOR GUIDE - BLOCKED ON HMI PHASE 2

**Controller baseline:** `v2-phase4d.4-dev`

**Controller source baseline:** `31ea4254992f296001d367cece70998659a82783`

**Control claim:** `RPM-CONTROLLED`

**Applicable HMI repository:** `ArgusControl_PumpHMI-Rotary-V1`

**Current HMI baseline commit:** `ae4fa951557517e1b3f2a24b492201cc672d0ef9`
(`Phase 1B close + Phase 1C on-device bring-up, real hardware verified`)

**Current HMI firmware identity:** `hmi-v1-phase1c-dev`

**Current HMI phase status:** Phase 1A and Phase 1B accepted; Phase 1C in
progress; Phase 2 controller integration not yet implemented

## 1. Purpose and Boundary

This guide connects one pump-mounted Argus HMI to one commissioned pump
controller, enrolls it as a distinct machine identity, provisions its network
and MQTT credentials, and proves a stationary authenticated connection.

Provisioning does not prove:

- operating authority;
- command admission;
- shaft motion;
- pump delivery;
- flow, pressure, or process performance; or
- physical or safety-rated E-stop behavior.

The controller remains authoritative. HMI motion acceptance belongs to the
applicable Phase 5 live procedures after all physical and powered gates are met.

## 2. Current HMI Readiness Gate

The rotary HMI is being built specifically for this controller. Its definitive
plan intentionally keeps all Phase 1 work mock-driven and defers live
authenticated MQTT, the HMI configuration portal, and durable pairing storage
to Phase 2. The current absence of live MQTT is therefore a planned phase
boundary, not a legacy-protocol defect.

Sections 6 through 11 define the required Phase 2 commissioning workflow. They
are not executable against the current Phase 1C firmware. Do not interpret the
presence of this guide as authorization to enroll a production credential or
claim that the HMI is provisioned.

Already present in the rotary HMI design/current work:

- a controller-contract-shaped `hmi_state`;
- bounded command-ledger handling for delayed, duplicate, timeout,
  reconnect, out-of-order, and session-replacement cases;
- controller-reported configured, trajectory, applied, generated, authority,
  session, sequence, and `feedback_available` semantics;
- a provider-neutral `hmi_command_provider` boundary under active Phase 1C
  integration preparation;
- the planned `hmi_mqtt_client` authenticated production provider;
- the planned HMI-local `hmi_portal`; and
- the planned dual-slot atomic `hmi_config_store` for network credentials,
  controller pairing, theme, and display preferences.

The HMI repository vendors frozen controller-contract snapshots. The Phase 4C
MQTT snapshot currently matches the accepted controller document byte-for-byte.
The Phase 4D security snapshot predates final Phase 4D.4 acceptance and no longer
matches the current accepted security contract. Refresh and review that snapshot
deliberately before Phase 2 implementation; do not silently overwrite its
recorded provenance.

Do not enroll a permanent HMI credential or begin Section 6 until an exact
Phase 2 HMI candidate passes this readiness gate:

1. A local provisioning mechanism for Wi-Fi SSID/password, MQTT machine ID, and
   MQTT machine secret implemented through the planned HMI-local SoftAP portal
   and dual-slot NVS pairing record, without committing credentials to source.
2. MQTT 3.1.1 clean-session CONNECT with no Will:
   - client ID = controller-generated machine ID;
   - username = the same machine ID;
   - password = controller-generated one-time machine secret.
3. Dynamic root `argus/<client_id>/<unit_id>` from the commissioned controller
   identity.
4. Read-only subscriptions to approved metadata, state, status, telemetry, and
   exact `event/pump1/command_result` paths.
5. Current-session heartbeat every two seconds using the strict JSON contract.
6. QoS 1, non-retained commands with current session, newer nonzero sequence,
   unique command ID, and topic-specific value.
7. No replay of buffered commands after reconnect or broker/controller restart.
8. Truthful separation of configured, trajectory, applied, and generated RPM.
   Generated values must not be displayed as measured shaft RPM.
9. Safe UI behavior while disconnected, stale, unauthorized, or awaiting a
   command result.
10. An accepted HMI build and stationary protocol-validation record.
11. A reviewed reconciliation against the accepted controller Phase 4C MQTT and
    Phase 4D/4D.4 security contracts.

**Readiness result:** `[PASS / FAIL / INCOMPLETE]`

**Accepted Phase 2 HMI integration commit:** `[PENDING]`

**Accepted Phase 2 HMI firmware identity:** `[PENDING]`

## 3. Required Equipment and Access

- accepted pump controller and its admin credentials;
- Waveshare ESP32-S3 1.8-inch rotary-knob HMI hardware with the reviewed HMI
  candidate;
- HMI USB data cable and freshly identified ESP32-S3 serial port;
- workstation with ESP-IDF v5.5.3;
- physical access to both devices;
- controller SoftAP SSID and active AP password;
- secure temporary credential-transfer method;
- serial logs for both controller and HMI; and
- evidence location that will not store reusable credentials.

Before provisioning:

- machine state is `UNLOCKED`;
- configured, applied, and generated outputs are zero;
- driver is disabled;
- motor power is physically isolated;
- no pressure or process energy exists; and
- controller identity, firmware commit, hardware UID, `client_id`, and `unit_id`
  are recorded.

COM18 was observed during Phase 1C HMI testing and COM5 is the pump controller,
but neither port is permanent. Identify the intended device before every flash.
Never flash the controller port while preparing the HMI.

## 4. Select the Network Path

### Recommended: Direct Controller SoftAP

Use this for a pump-local HMI that remains usable without site Wi-Fi.

| Setting | Required value |
|---|---|
| HMI Wi-Fi mode | STA |
| HMI Wi-Fi network | Active controller SoftAP |
| MQTT broker | `mqtt://192.168.4.1:1883` |
| Enrolled receiving interface | `SoftAP` only |
| Internet dependency | None |

The HMI has two mutually exclusive network states:

- **Local provisioning mode:** the HMI exposes its own temporary SoftAP and
  local HTTP portal. MQTT is not active.
- **Normal operating mode:** the HMI disables its portal, acts as a Wi-Fi STA,
  joins the controller SoftAP, and runs authenticated MQTT.

Do not confuse the HMI configuration SoftAP with the controller SoftAP. The
workstation may need to switch between them during first provisioning.

Do not copy an old development SSID or password from source or documentation.
Use the active controller AP values. The AP password is a credential and does
not belong in this guide, screenshots, logs, or repository evidence.

### Alternate: Shared Trusted STA Network

Use only when the deployment deliberately places the HMI and controller on the
same approved trusted LAN.

| Setting | Required value |
|---|---|
| HMI Wi-Fi mode | STA |
| HMI Wi-Fi network | Approved trusted deployment LAN |
| MQTT broker | `mqtt://<current-controller-STA-address>:1883` |
| Enrolled receiving interface | `STA` only |

Discover the controller address after every reboot or network change. Do not
hardcode a previously observed address as permanent. Do not select both
interfaces merely for convenience; grant only the path the HMI will use.

**Selected path:** `[DIRECT SOFTAP / TRUSTED STA]`

## 5. Record the Controller Identity

From the authenticated controller Overview/Commissioning pages, record:

| Field | Value |
|---|---|
| Controller hardware UID | `[PENDING]` |
| Device name | `[PENDING]` |
| Commissioned client ID | `[PENDING]` |
| Commissioned unit ID | `[PENDING]` |
| MQTT root | `argus/<client_id>/<unit_id>` |
| Controller scope shown by the enrollment form | `[PENDING]` |
| Controller firmware identity | `[PENDING]` |
| Controller source commit | `[PENDING]` |

For the currently accepted controller identity, the expected root is
`argus/paladin/pump_001`. Verify live identity rather than assuming it remains
unchanged.

## 6. Enroll the HMI Machine Identity

1. Connect the administration workstation to the controller SoftAP.
2. Open `http://192.168.4.1/`.
3. Log in with an account authorized to enroll machine clients.
4. Open the Security/Commissioning administration page.
5. Complete recent reauthentication when requested.
6. Open **Machines** and **Enroll machine client**.
7. Enter:

| Enrollment field | Value |
|---|---|
| Display name | A unique physical label, for example `Pump HMI - pump_001` |
| Machine type | `Pump HMI` (`PUMP_HMI`) |
| Receiving interface | `SoftAP` for the recommended direct path, otherwise `STA` |
| Controller scope | Preserve the exact narrow controller scope supplied by the authenticated console |
| MQTT topic scope | Exact root `argus/<client_id>/<unit_id>` |
| Future API scope | Leave empty |
| Capability IDs | `view_status, request_authority, motion` |

`view_status` permits approved read subscriptions. `request_authority` permits
the Phase 4C heartbeat/lease path. `motion` permits normal set-target, Start,
Stop, Unlock, and Recover topics, subject to the controller's authority, session,
sequence, router, and machine-state checks.

Do not grant administrative capabilities. The rotary HMI plan currently includes
a guarded software E-stop request, but the final Phase 1C E-stop strategy remains
an explicit product-owner decision. Add `software_estop` only if the accepted
Phase 2 candidate retains that control. Add `reset_software_estop` only if the
accepted UI intentionally exposes reset and its separate acceptance passes.

8. Select **Enroll** once.
9. Confirm the returned machine record is enabled, not revoked, and has the
   intended interface, topic scope, and capabilities.
10. Keep the one-time credential dialog open until provisioning succeeds.

## 7. Handle the One-Time Credential

The controller returns:

- `machine_id`;
- `machine_secret`;
- credential version; and
- principal revision.

The secret is disclosed once. The controller retains only its verifier.

Use these MQTT fields:

| MQTT field | Provisioned value |
|---|---|
| Client ID | `machine_id` |
| Username | the same `machine_id` |
| Password | `machine_secret` |
| Clean session | enabled |
| Will | disabled |

Credential rules:

- do not photograph or screenshot the secret;
- do not paste it into chat, tickets, documentation, shell history, source,
  committed `sdkconfig`, build logs, or evidence;
- do not use a human password or controller AP password as the machine secret;
- do not reuse the HMI credential for Node-RED or another client;
- clear the clipboard and temporary transfer buffer after provisioning;
- restrict any local provisioning artifact and remove it after verification; and
- if the secret is lost before provisioning, rotate the machine credential and
  use only the newly returned secret.

## 8. Provision the HMI

Use the accepted rotary HMI candidate's `hmi_portal` local provisioning
mechanism.

1. While still connected to the controller administration page, copy the
   one-time machine credential into the approved temporary transfer mechanism.
2. Close the controller credential dialog only after both fields are captured.
3. Enter the rotary HMI Status/service view.
4. Use the accepted long-press configuration entry to start the HMI-local
   SoftAP portal.
5. Confirm normal STA/MQTT operation is inactive while the portal is active.
6. Move the workstation from the controller SoftAP to the HMI provisioning
   SoftAP.
7. Open the HMI local portal at the address documented by the accepted Phase 2
   HMI candidate.
8. Provision:

   - selected Wi-Fi SSID;
   - selected Wi-Fi password;
   - MQTT broker URI;
   - MQTT machine ID as both client ID and username;
   - one-time machine secret as MQTT password;
   - commissioned controller `client_id`;
   - commissioned controller `unit_id`; and
   - dynamic MQTT root.

9. Commit the configuration through the dual-slot atomic
   `hmi_config_store` path.
10. Require validation/readback before the new slot becomes active.
11. Exit provisioning mode and reboot into normal STA/MQTT operation.

Do not place the machine secret in the HMI repository, build-time Kconfig,
committed `sdkconfig`, or source. If the Phase 2 candidate lacks the planned
portal or dual-slot pairing store, classify commissioning `INCOMPLETE`.

After local provisioning:

- clear clipboard contents;
- remove temporary plaintext files;
- verify ignored/generated files do not contain a credential intended for
  archive or commit;
- do not archive an unsanitized build directory; and
- preserve only non-secret provisioning metadata.

## 9. First Boot and Stationary Connection Proof

Keep motor power physically isolated.

1. Start controller and HMI serial capture.
2. Boot the controller and wait for stable commissioned operation.
3. Boot the HMI.
4. Confirm HMI Wi-Fi obtains an address on the selected interface.
5. Confirm the controller logs one authenticated machine connection.
6. Confirm MQTT CONNACK succeeds.
7. Confirm approved subscriptions succeed and broad/disallowed subscriptions
   are not attempted.
8. Confirm retained metadata/state/status/telemetry populate the HMI.
9. Confirm `feedback_available=false` is represented truthfully.
10. Read the retained `status/core/command_session`.
11. Begin non-retained QoS 1 heartbeats:

```json
{"session":"<current-16-lowercase-hex-session>","counter":1}
```

12. Increment the nonzero uint32 counter every two seconds.
13. Confirm retained supervisor link becomes `ONLINE`.
14. Confirm authentication/heartbeat caused no target, state, driver, E-stop,
    fault, authority-generation, command-sequence, or physical-motion change.
15. Disconnect/reconnect the HMI once.
16. Confirm it rereads the current session and does not replay a command.

**Stationary connection result:** `[PASS / FAIL / INVALID / INCOMPLETE]`

## 10. Command-Path Readiness Proof

Do not issue a motion-capable command during provisioning.

Before Phase 5 MQTT motion testing, verify from HMI source/tests that:

- commands use exact canonical topics;
- QoS is 1 and RETAIN is false;
- the current broker session is included;
- sequence is nonzero and strictly newer;
- command ID is unique and bounded;
- topic-specific `value` is exact;
- PUBACK is treated as transport receipt only;
- `command_result` determines application acceptance;
- exact duplicate results do not cause redispatch;
- stale/session-mismatched requests are not retried as new commands;
- reconnect clears pending command state and rereads the session; and
- HMI controls remain disabled until status and authority are coherent.

The rotary HMI's provider-neutral boundary must allow the Phase 2 MQTT provider
to replace the Phase 1 mock provider without making the UI transport-aware.
Powered HMI command acceptance is performed under Phase 5 Test 13 and its
physical safety gates.

## 11. Credential Lifecycle

### Rotate

1. Put the controller and process in the approved stationary safe state.
2. Open local HMI provisioning mode.
3. Reauthenticate in the controller administration page.
4. Select **Rotate** for the exact HMI machine record.
5. Expect the existing MQTT connection to close immediately.
6. Provision the newly disclosed secret; the machine ID remains unchanged.
7. Reconnect and repeat the stationary proof.
8. Clear the one-time secret from every temporary location.

### Disable

Use **Disable** for a temporarily removed or suspect HMI. The current connection
must close or become policy-inert, and reconnect must fail. Enable only after the
reason is resolved and recorded.

### Revoke and Delete

Use **Revoke** when the HMI or credential is retired or compromised. Revocation
is not reversible. Delete only after revocation and after preserving the
non-secret audit/commissioning record.

Replacing an HMI requires a new machine identity and credential. Do not transfer
the retired HMI's secret.

## 12. Troubleshooting

| Symptom | Check |
|---|---|
| Wi-Fi does not connect | Active SSID/password, selected topology, signal, and HMI STA logs |
| MQTT CONNACK code 2 | Client ID must exactly equal the machine ID; check duplicate live client ID and clean-session/Will flags |
| MQTT CONNACK code 4 | Machine secret, enabled/revoked state, receiving interface, or authentication throttle |
| SUBACK `0x80` | Requested filter is outside approved metadata/state/status/telemetry or exact command-result policy, or outside topic scope |
| MQTT connects but supervisor stays `OFFLINE`/`STALE` | Current session, heartbeat topic, strict JSON, QoS 1, RETAIN false, counter progression, and `request_authority` capability |
| PUBACK arrives but command is not accepted | Read `command_result`; check authority, session, sequence, command value, and machine state |
| HMI shows RPM but feedback is unavailable | Fix the HMI label; generated/applied output is not measured shaft RPM |
| Reconnect causes a command attempt | Stop commissioning; buffered-command replay violates the accepted contract |
| Credential appears in logs or evidence | Stop, sanitize evidence, rotate immediately, and document the incident without preserving the secret |

Do not work around rejection by widening topic scope, selecting both interfaces,
granting administrative capabilities, restoring legacy topics, or bypassing the
controller router.

## 13. Commissioning Record

Record no reusable credentials.

| Item | Result |
|---|---|
| Controller commit/firmware/hardware UID | `[PENDING]` |
| Rotary HMI commit/firmware/hardware identity | `[PENDING]` |
| Controller-contract snapshot reconciliation | `[PENDING]` |
| Controller client/unit identity and MQTT root | `[PENDING]` |
| Selected network topology/interface | `[PENDING]` |
| Machine ID | `[PENDING - NON-SECRET]` |
| Machine type | `PUMP_HMI` |
| Topic and controller scopes | `[PENDING]` |
| Capability set | `[PENDING]` |
| Credential version/principal revision | `[PENDING - NON-SECRET]` |
| Wi-Fi connection | `[PENDING]` |
| Authenticated MQTT CONNECT | `[PENDING]` |
| Approved subscriptions | `[PENDING]` |
| Current-session heartbeat/link | `[PENDING]` |
| Retained state truthfulness | `[PENDING]` |
| Reconnect/no-replay proof | `[PENDING]` |
| Credential removed from temporary handling | `[PENDING]` |
| Controller remained stationary and uncontaminated | `[PENDING]` |
| Provisioning disposition | `[PASS / FAIL / INVALID / INCOMPLETE]` |
| Operator/reviewer/date | `[PENDING]` |

The HMI is **provisioned** after this stationary record passes. It is
**commissioned for control** only after its reviewed firmware and applicable
Phase 5 HMI/MQTT live tests pass against the exact controller and physical
release candidate.

## 14. Authoritative References

- `docs/PHASE_4C_MQTT_CONTRACT.md`
- `docs/PHASE_4D_SECURITY_CONTRACT.md`
- `docs/PHASE_4D_4_IMPLEMENTATION_PLAN.md`
- `docs/Phase 4D.4 Tests.md`
- `docs/Phase 5 - Final Acceptance and Live Testing/03_SAFETY_SETUP_AND_HARD_STOPS.md`
- `docs/Phase 5 - Final Acceptance and Live Testing/05_LIVE_ACCEPTANCE_PROCEDURES.md`
- separate `ArgusControl_PumpHMI-Rotary-V1` repository:
  - `docs/plan/ROTARY_HMI_DEFINITIVE_PLAN.md`
  - `contracts/controller-snapshot/PROVENANCE.md`
  - `contracts/controller-snapshot/PHASE_4C_MQTT_CONTRACT.md`
  - `contracts/controller-snapshot/PHASE_4D_SECURITY_CONTRACT.md`
  - `common/hmi_state.[ch]`
  - `common/hmi_ledger.[ch]`
  - `common/hmi_command_provider.h`
  - future Phase 2 `hmi_mqtt_client`, `hmi_portal`, and `hmi_config_store`
