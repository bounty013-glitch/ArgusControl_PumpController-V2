#include "argus_mqtt_runtime.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "mbedtls/sha256.h"

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "argus_authority_mgr.h"
#include "argus_cmd_router.h"
#include "argus_config.h"
#include "argus_identity.h"
#include "argus_machine_service.h"
#include "argus_mqtt_security.h"
#include "argus_security_audit.h"
#include "argus_net_mgr.h"
#include "argus_state_mgr.h"
#include "argus_nvs_config.h"
#include "argus_security_store.h"

static const char *TAG = "argus_mqtt_runtime";

#define ARGUS_MQTT_RUNTIME_QUEUE_DEPTH 12U
#define ARGUS_MQTT_RUNTIME_TASK_STACK 6144U
// Bounded startup window during which, under ARGUSCORE_PREFERRED, only
// ArgusCore may acquire authority. Long enough for a normally booting
// supervisor to authenticate and request; short enough that an operator is
// not left staring at a panel that will not take control. TUNING VALUE -
// exercised only under ARGUSCORE_PREFERRED, and the bench unit is
// STANDALONE_HMI, so it is unverified in the field as yet.
#define ARGUS_MQTT_CORE_ACQUISITION_WINDOW_MS 45000U

typedef enum {
    RUNTIME_EVENT_MESSAGE = 0,
    RUNTIME_EVENT_PUBLISH_BASELINE,
} runtime_event_type_t;

typedef struct {
    runtime_event_type_t type;
    union {
        argus_mqtt_broker_message_t message;
    } data;
} runtime_event_t;

typedef struct {
    SemaphoreHandle_t mutex;
    QueueHandle_t queue;
    TaskHandle_t task;
    bool initialized;
    bool prepared;
    argus_identity_t identity;
    argus_mqtt_topics_t topics;
    argus_mqtt_session_core_t session;
} runtime_state_t;

static runtime_state_t s_runtime;
static uint64_t s_last_mqtt_policy_audit_us;
static uint64_t s_last_mqtt_auth_failure_audit_us;

static uint64_t now_ms(void)
{
    return (uint64_t)esp_timer_get_time() / 1000U;
}

static void publish_value(const char *topic, const char *payload, bool retain)
{
    esp_err_t err = argus_mqtt_broker_publish(topic, payload, retain);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "publish failed: topic=%s err=%s", topic, esp_err_to_name(err));
    }
}

static void publish_number(const char *topic, int64_t value)
{
    char payload[32];
    snprintf(payload, sizeof(payload), "%" PRId64, value);
    publish_value(topic, payload, true);
}

static void publish_authority_state(void);
static void set_authority_reason(const char *reason);
static void compose_authority_result_json(
    char *out, size_t out_size, const char *request_id, const char *session,
    const char *outcome, const char *reason, const char *owner_kind,
    uint32_t authority_epoch, const char *lease_status, const char *local_status);

static void publish_operational_snapshot(void)
{
    argus_state_snapshot_t state = {0};
    argus_authority_snapshot_t authority = {0};
    argus_net_snapshot_t network = {0};
    argus_state_mgr_get_snapshot(&state);
    (void)argus_authority_mgr_get_snapshot(&authority);
    (void)argus_net_mgr_get_snapshot(&network);

    argus_mqtt_link_state_t link;
    uint32_t last_sequence;
    bool has_sequence;
    xSemaphoreTake(s_runtime.mutex, portMAX_DELAY);
    link = s_runtime.session.link;
    last_sequence = s_runtime.session.last_sequence;
    has_sequence = s_runtime.session.has_sequence;
    xSemaphoreGive(s_runtime.mutex);

    publish_value(s_runtime.topics.state_online, "true", true);
    publish_value(s_runtime.topics.state_supervisor_link, argus_mqtt_link_name(link), true);
    publish_value(s_runtime.topics.state_mode,
                  argus_state_mgr_get_state_name(state.machine_state), true);
    publish_value(s_runtime.topics.state_driver,
                  state.driver_enabled ? "ENABLED" : "DISABLED", true);
    publish_value(s_runtime.topics.state_direction,
                  state.applied_forward ? "FORWARD" : "REVERSE", true);
    publish_value(s_runtime.topics.state_estop,
                  state.estop_latched ? "LATCHED" : "CLEAR", true);
    publish_number(s_runtime.topics.state_fault, state.fault_code);

    publish_value(s_runtime.topics.status_wifi,
                  network.sta_connected && network.sta_ip_acquired ? "CONNECTED" : "DISCONNECTED", true);
    publish_value(s_runtime.topics.status_mqtt,
                  argus_mqtt_broker_is_running() ? "RUNNING" : "STOPPED", true);
    publish_value(s_runtime.topics.status_network_mode,
                  argus_net_mgr_get_mode_name(network.mode), true);
    publish_value(s_runtime.topics.status_authority_mode,
                  argus_authority_mgr_get_mode_name(authority.mode), true);
    publish_value(s_runtime.topics.status_authority_owner,
                  argus_authority_mgr_get_owner_name(authority.owner), true);
    publish_number(s_runtime.topics.status_uptime_s, now_ms() / 1000U);
    publish_number(s_runtime.topics.status_last_accepted_sequence,
                   has_sequence ? last_sequence : 0U);

    publish_number(s_runtime.topics.telemetry_configured_target,
                   state.configured_target_rpm_milli);
    publish_number(s_runtime.topics.telemetry_trajectory_target,
                   state.trajectory_target_rpm_milli);
    publish_number(s_runtime.topics.telemetry_applied, state.applied_rpm_milli);
    publish_number(s_runtime.topics.telemetry_generated, state.generated_rpm_milli);
    publish_number(s_runtime.topics.telemetry_step_count, state.generated_step_count);
    publish_value(s_runtime.topics.telemetry_feedback_available,
                  state.feedback_available ? "true" : "false", true);
}

static void publish_baseline(void)
{
    publish_value(s_runtime.topics.metadata_device_name,
                  s_runtime.identity.device_name, true);
    publish_value(s_runtime.topics.metadata_model,
                  s_runtime.identity.device_model, true);
    publish_value(s_runtime.topics.metadata_firmware_version,
                  s_runtime.identity.fw_version, true);
    publish_value(s_runtime.topics.metadata_hardware_uid,
                  s_runtime.identity.mac_uid, true);
    publish_value(s_runtime.topics.status_command_session,
                  s_runtime.session.session, true);
    // Retained from startup so a client connecting before anyone has acquired
    // authority still learns the profile and that control is unowned, rather
    // than seeing nothing and having to guess.
    publish_authority_state();
    publish_operational_snapshot();
}


static const char *decode_reason(argus_mqtt_decode_result_t result)
{
    switch (result) {
    case ARGUS_MQTT_DECODE_UNKNOWN_FIELD: return "unknown_field";
    case ARGUS_MQTT_DECODE_DUPLICATE_FIELD: return "duplicate_field";
    case ARGUS_MQTT_DECODE_MISSING_FIELD: return "missing_field";
    case ARGUS_MQTT_DECODE_INVALID_VALUE: return "invalid_value";
    case ARGUS_MQTT_DECODE_TOO_LARGE: return "payload_too_large";
    case ARGUS_MQTT_DECODE_SCHEMA_UNSUPPORTED: return "schema_unsupported";
    case ARGUS_MQTT_DECODE_INVALID_REQUEST_ID: return "invalid_request_id";
    default: return "malformed_json";
    }
}

static void build_result(char *out, size_t out_size,
                         const argus_mqtt_command_t *command,
                         const char *outcome, const char *reason)
{
    argus_authority_snapshot_t authority = {0};
    argus_state_snapshot_t state = {0};
    (void)argus_authority_mgr_get_snapshot(&authority);
    argus_state_mgr_get_snapshot(&state);
    int written = snprintf(
        out, out_size,
        "{\"session\":\"%s\",\"sequence\":%" PRIu32
        ",\"command_id\":\"%s\",\"action\":\"%s\",\"outcome\":\"%s\""
        ",\"reason\":\"%s\",\"authority_generation\":%" PRIu32
        ",\"command_generation\":%" PRIu32 ",\"machine_state\":\"%s\"}",
        command->session, command->sequence, command->command_id,
        argus_mqtt_action_name(command->action), outcome, reason,
        authority.generation, state.command_generation,
        argus_state_mgr_get_state_name(state.machine_state));
    if (written < 0 || (size_t)written >= out_size) {
        strlcpy(out, "{\"outcome\":\"REJECTED\",\"reason\":\"result_overflow\"}", out_size);
    }
}

static argus_cmd_type_t command_type(argus_mqtt_action_t action)
{
    switch (action) {
    case ARGUS_MQTT_ACTION_SET_TARGET: return ARGUS_CMD_TYPE_SET_TARGET;
    case ARGUS_MQTT_ACTION_START: return ARGUS_CMD_TYPE_START;
    case ARGUS_MQTT_ACTION_STOP: return ARGUS_CMD_TYPE_STOP_NORMAL;
    case ARGUS_MQTT_ACTION_UNLOCK: return ARGUS_CMD_TYPE_UNLOCK;
    case ARGUS_MQTT_ACTION_E_STOP: return ARGUS_CMD_TYPE_ESTOP;
    case ARGUS_MQTT_ACTION_RESET_E_STOP: return ARGUS_CMD_TYPE_RESET_ESTOP;
    case ARGUS_MQTT_ACTION_RECOVER: return ARGUS_CMD_TYPE_RECOVER;
    default: return ARGUS_CMD_TYPE_STOP_NORMAL;
    }
}

static void reject_decoded(const argus_mqtt_command_t *command,
                           const char *reason)
{
    char result[ARGUS_MQTT_BROKER_PAYLOAD_CAP];
    build_result(result, sizeof(result), command, "REJECTED", reason);
    publish_value(s_runtime.topics.command_result, result, false);
}

static void handle_heartbeat(const argus_mqtt_broker_message_t *message)
{
    if (!message->policy_admitted) return;
    argus_mqtt_heartbeat_t heartbeat;
    if (argus_mqtt_decode_heartbeat(message->payload, message->payload_len,
                                    &heartbeat) != ARGUS_MQTT_DECODE_OK) return;
    // Amendment A1: a heartbeat RENEWS a lease, it never acquires one. A
    // heartbeat from a principal that does not already hold authority is not
    // an acquisition and is refused - otherwise connecting and heartbeating
    // would still be enough to take control of the pump, which is exactly
    // what the commissioned-profile model exists to prevent.
    xSemaphoreTake(s_runtime.mutex, portMAX_DELAY);
    bool is_holder = s_runtime.session.lease_machine_id[0] != '\0' &&
                     strcmp(s_runtime.session.lease_machine_id,
                            message->principal.identifier) == 0;
    bool was_online = s_runtime.session.link == ARGUS_MQTT_LINK_ONLINE;
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (is_holder) {
        err = argus_mqtt_session_accept_heartbeat(
            &s_runtime.session, message->connection_id,
            message->principal.identifier, message->principal.client_type,
            &heartbeat, now_ms());
    }
    xSemaphoreGive(s_runtime.mutex);
    if (!is_holder) {
        ESP_LOGD(TAG, "heartbeat ignored from non-holder %s",
                 message->principal.identifier);
        return;
    }
    if (err == ESP_OK) {
        publish_value(s_runtime.topics.state_supervisor_link, "ONLINE", true);
        // §4 "rebind": the holder's link moving from non-ONLINE back to
        // ONLINE - a reconnect within the lease term, or the transport
        // recovering after a blip - moves core_lease_status back from
        // EXPIRING toward ACTIVE even though owner and epoch are unchanged.
        // Republished only on that actual transition, not on every routine
        // renewal, so a healthy 2-second heartbeat cadence does not turn into
        // a steady stream of retained republishes for values that did not
        // change.
        if (!was_online) {
            // Final-focused-pass item 2: the same rebind the explicit
            // request path reports as CORE_REACQUISITION, reached here via
            // heartbeat instead. Either entry point can be how a principal
            // reconnects; the reason must agree regardless of which one it
            // used.
            set_authority_reason("CORE_REACQUISITION");
            publish_authority_state();
        }
    }
}

// Reads the COMMISSIONED authority profile. Deliberately re-read per request
// rather than cached: commissioning can change it, and a stale cache would let
// a recommissioned unit keep arbitrating by its old profile. Falls back to
// STANDALONE_HMI only if the store is unreadable, which is the choice that
// keeps a pump operable from its own panel rather than waiting on a supervisor
// it may not have.
static uint8_t commissioned_authority_profile(void)
{
    argus_config_payload_t cfg;
    bool has_persisted = false;
    if (argus_nvs_config_get_effective(&cfg, &has_persisted) != ESP_OK) {
        ESP_LOGW(TAG, "authority profile unreadable; assuming STANDALONE_HMI");
        return ARGUS_AUTHORITY_PROFILE_STANDALONE_HMI;
    }
    if (cfg.authority_profile > ARGUS_AUTHORITY_PROFILE_MAX) {
        ESP_LOGW(TAG, "authority profile out of range (%u); assuming STANDALONE_HMI",
                 (unsigned)cfg.authority_profile);
        return ARGUS_AUTHORITY_PROFILE_STANDALONE_HMI;
    }
    return cfg.authority_profile;
}

// True while the bounded startup window for ArgusCore is still open. Only
// meaningful under ARGUSCORE_PREFERRED; it is what keeps a fast-booting panel
// view-only until the supervisor has had its chance.
// Anchored to the moment acquisition became operationally possible - the
// broker accepting connections - NOT to raw MCU uptime.
//
// Measuring from uptime was wrong in a way that defeats the window's purpose:
// broker and authority-service startup take seconds, and any of that time was
// silently consumed before a supervisor could physically send a request. A
// slow start could have burned the whole window before the first request was
// even possible, handing control to the local fallback for a reason that has
// nothing to do with ArgusCore being absent.
//
// Zero means "not yet ready", and the window is treated as OPEN in that
// state: before acquisition is possible, nobody may acquire, which fails
// closed toward the commissioned preference rather than toward the fallback.
static uint64_t s_authority_ready_ms;

// §4 "fallback" transition tracking. The ARGUSCORE_PREFERRED acquisition
// window closing with nobody having acquired is a state transition the A2.10
// snapshot must reflect (local_control_status WAITING -> AVAILABLE), but
// unlike every other listed transition (acquisition, release, disconnect,
// rebind, expiry) it has no discrete triggering EVENT - core_acquisition_window_open()
// is a pure function of elapsed time, evaluated on demand. Without this,
// nothing would republish the snapshot at the moment the window closes if no
// client happened to touch anything else at that instant, leaving
// local_control_status stuck at WAITING past the point it became true.
// Polled once per argus_mqtt_runtime_tick() rather than timer-driven: tick()
// already runs on a bounded cadence for heartbeat expiry, so this reuses that
// cadence instead of adding a second one.
static bool s_fallback_window_was_open = true;

// Why authority is where it is. Set at every transition so the published
// snapshot can explain itself; the HMI must never have to reconstruct this
// from transient events it may have missed while disconnected.
static const char *s_authority_reason = "STARTUP";
static void set_authority_reason(const char *reason) { s_authority_reason = reason; }

const char *argus_mqtt_runtime_get_authority_reason(void)
{
    return s_authority_reason;
}

// Pure A2.10 authority_reason decision rules (final-focused-pass item 2),
// extracted so a test can assert the exact transition independent of the
// runtime wiring (mutex, queue, broker) that invokes set_authority_reason()
// with whatever these return. A NULL return means "leave the current reason
// alone" - a denial does not describe who owns things now, so it must not
// overwrite whatever reason correctly describes the state that denial left
// unchanged.
const char *argus_mqtt_authority_reason_for_request(
    argus_mqtt_authority_result_t result, bool was_online_before,
    uint8_t requester_client_type, uint8_t authority_profile)
{
    if (result == ARGUS_MQTT_AUTHORITY_GRANTED) {
        if (requester_client_type == ARGUS_MACHINE_CLIENT_ARGUS_COMMAND) {
            return "CORE_ACQUISITION";
        }
        return authority_profile == ARGUS_AUTHORITY_PROFILE_STANDALONE_HMI
                   ? "STANDALONE_ACQUISITION" : "OPERATOR_TRANSFER";
    }
    if (result == ARGUS_MQTT_AUTHORITY_ALREADY_HELD && !was_online_before) {
        // A same-principal reacquisition reached while the link was NOT
        // already ONLINE is a rebind after transport loss, not an ordinary
        // renewal - the contract names this transition distinctly.
        return "CORE_REACQUISITION";
    }
    return NULL;
}

const char *argus_mqtt_authority_reason_for_release(bool released)
{
    return released ? "RELEASED" : NULL;
}

// Always TRANSPORT_LOST: argus_mqtt_session_disconnect() only reports
// `changed` when the departing connection matched the CURRENT lease's or
// heartbeat's binding, so a caller that already checked `changed` has
// nothing further to decide. A function rather than a literal at the call
// site so the two share one place to change if that ever needs a condition.
const char *argus_mqtt_authority_reason_for_disconnect(void)
{
    return "TRANSPORT_LOST";
}

const char *argus_mqtt_authority_reason_for_expiry(void)
{
    return "CORE_LEASE_EXPIRED";
}

// The session-side admission rule for an operational command, verbatim from
// what handle_command() used to inline - see argus_mqtt_runtime.h for why it
// is exported. Order matters and is contract surface: session binding first
// (a prior-session command is meaningless regardless of who sent it), holder
// identity + live-connection binding second, and the A2.9 epoch check LAST -
// after identity, before anything that consumes state - so a stale-epoch
// command must already have been the bound holder's, and its rejection burns
// no sequence number.
argus_mqtt_command_admission_t argus_mqtt_command_admission_check(
    const argus_mqtt_session_core_t *core, const char *principal_identifier,
    uint64_t connection_id, const argus_mqtt_command_t *command)
{
    if (core == NULL || principal_identifier == NULL || command == NULL ||
        strcmp(command->session, core->session) != 0) {
        return ARGUS_MQTT_COMMAND_ADMIT_SESSION_MISMATCH;
    }
    // The lease belongs to the authenticated principal; the connection id
    // proves that holder is still on this socket. Both must agree, so a
    // second connection authenticated as the same machine cannot command
    // alongside the holder.
    if (core->link != ARGUS_MQTT_LINK_ONLINE ||
        core->lease_machine_id[0] == '\0' ||
        strcmp(core->lease_machine_id, principal_identifier) != 0 ||
        core->lease_connection_id != connection_id) {
        return ARGUS_MQTT_COMMAND_ADMIT_NOT_HOLDER;
    }
    // A2.9 epoch admission. Supplements the checks above, replaces none.
    // This catches what the identity gate alone cannot: a principal that
    // lost authority and later regained it under a NEW epoch would otherwise
    // have a delayed command from its OWN previous epoch accepted, because
    // identity and connection binding both match again.
    if (command->authority_epoch != core->authority_epoch) {
        return ARGUS_MQTT_COMMAND_ADMIT_STALE_EPOCH;
    }
    return ARGUS_MQTT_COMMAND_ADMIT_OK;
}

void argus_mqtt_runtime_mark_authority_ready(void)
{
    // Only on a transition into readiness with no owner. A broker restart
    // while a valid owner and lease exist must NOT silently reset authority
    // or hand another principal a fresh window.
    xSemaphoreTake(s_runtime.mutex, portMAX_DELAY);
    bool owned = (s_runtime.session.lease_machine_id[0] != '\0' &&
                  s_runtime.session.link == ARGUS_MQTT_LINK_ONLINE);
    if (!owned) {
        s_authority_ready_ms = now_ms();
        ESP_LOGI(TAG, "authority acquisition window opened (%u ms)",
                 (unsigned)ARGUS_MQTT_CORE_ACQUISITION_WINDOW_MS);
    } else {
        ESP_LOGI(TAG, "authority service ready; existing owner retained, window not reset");
    }
    xSemaphoreGive(s_runtime.mutex);
}

static bool core_acquisition_window_open(void)
{
    uint64_t anchor;
    xSemaphoreTake(s_runtime.mutex, portMAX_DELAY);
    anchor = s_authority_ready_ms;
    xSemaphoreGive(s_runtime.mutex);
    if (anchor == 0U) return true;   // not ready yet: nobody acquires
    return (now_ms() - anchor) < ARGUS_MQTT_CORE_ACQUISITION_WINDOW_MS;
}

// The SOLE mapping from arbitration result to wire reason string, used for
// both the published A2.4 result and the audit log line, so the two can
// never disagree. Previously there were two independent mappings - this one
// and a parallel `reasons[]` array indexed by `(int)result`, bounds-clamped
// at 4 - and adding ARGUS_MQTT_AUTHORITY_TRANSFER_UNSUPPORTED_RUNNING as a
// fifth value would have silently fallen through that array's clamp to the
// wrong string. A switch has no such failure mode: an unhandled new value is
// a compiler warning, not a silent misreport.
//
// Strings corrected to the exact A2.5 vocabulary: "denied_by_profile", not
// the previous "denied_by_commissioned_profile", which was never valid
// contract surface.
static const char *authority_result_reason(argus_mqtt_authority_result_t r)
{
    switch (r) {
    case ARGUS_MQTT_AUTHORITY_GRANTED:        return "granted";
    case ARGUS_MQTT_AUTHORITY_ALREADY_HELD:   return "already_held";
    case ARGUS_MQTT_AUTHORITY_DENIED_HELD:    return "denied_held_by_other";
    case ARGUS_MQTT_AUTHORITY_DENIED_PROFILE: return "denied_by_profile";
    case ARGUS_MQTT_AUTHORITY_TRANSFER_UNSUPPORTED_RUNNING:
        return "transfer_unsupported_running";
    case ARGUS_MQTT_AUTHORITY_DENIED_WINDOW_OPEN:
        return "denied_window_open";
    case ARGUS_MQTT_AUTHORITY_TRANSFER_EPOCH_REQUIRED:
        // Reuses the existing "stale_epoch" wire reason rather than minting
        // new contract surface: from the requester's point of view this IS
        // a stale-epoch refusal - it never had a valid current epoch to
        // supply in the first place, which is the same defect a mismatched
        // nonzero epoch reports.
        return "stale_epoch";
    default:
        // Reachable only through argument validation inside the pure core
        // (null/empty machine_id, out-of-range profile) - not through any
        // input an authenticated, capability-checked principal can produce.
        // Not part of the A2.5 vocabulary because it is not reachable from
        // the wire.
        return "denied_invalid";
    }
}


// Computes core_lease_status and local_control_status per A2.10, from lease
// presence/identity/link/deadline together, so the two consumers of this
// (the retained snapshot and the per-request result message) can never
// disagree with each other. Pure: touches no global state, which is what
// makes it callable from argus_mqtt_authority_admit() - and directly from a
// test, which is why it is exported rather than kept static - without a
// broker, s_runtime, or a mutex.
//
// FIX, correcting a real defect this replaces: the retained-snapshot path
// used to require link == ONLINE to consider the lease "owned" at all, which
// made status/core/control_owner and status/core/local_control_status report
// NONE/AVAILABLE for a lease that was merely disconnected but NOT expired -
// directly contradicting A2.11 ("published ownership must remain truthful
// even while the link is offline"). Ownership here is
// `lease_machine_id[0] != '\0'` alone, exactly as the pure core defines it:
// disconnect (argus_mqtt_session_disconnect) preserves that field, and only
// expiry (argus_mqtt_session_tick) clears it.
void authority_status_from_core(
    const argus_mqtt_session_core_t *core, uint8_t authority_profile,
    bool core_window_open, uint64_t now_ms,
    char *lease_status_out, size_t lease_status_cap,
    char *local_status_out, size_t local_status_cap)
{
    bool owned = core->lease_machine_id[0] != '\0';

    const char *lease_status;
    if (!owned) {
        lease_status = (core->link == ARGUS_MQTT_LINK_STALE) ? "EXPIRED" : "NONE";
    } else if (core->link != ARGUS_MQTT_LINK_ONLINE) {
        // Owner still holds it, but the transport is down and the deadline
        // is running. Honest intermediate state rather than a false ACTIVE.
        lease_status = "EXPIRING";
    } else {
        uint64_t age = now_ms - core->last_heartbeat_ms;
        lease_status = (age * 2U >= ARGUS_MQTT_HEARTBEAT_TIMEOUT_MS)
                           ? "EXPIRING" : "ACTIVE";
    }
    strlcpy(lease_status_out, lease_status, lease_status_cap);

    // What the panel needs in order to decide whether to offer controls at
    // all, without it having to reason about profiles and windows itself.
    const char *local_status;
    if (owned && core->lease_client_type == ARGUS_MACHINE_CLIENT_HMI) {
        local_status = "ACTIVE";
    } else if (owned) {
        local_status = "UNAVAILABLE";
    } else if (authority_profile == ARGUS_AUTHORITY_PROFILE_ARGUSCORE_PREFERRED &&
               core_window_open) {
        local_status = "WAITING";
    } else {
        local_status = "AVAILABLE";
    }
    strlcpy(local_status_out, local_status, local_status_cap);
}

// (A former authority_status_snapshot() wrapper lived here. It took the
// mutex a SECOND time, independently of publish_authority_state()'s own
// acquisition, which is exactly the incoherence the single-snapshot fix in
// publish_authority_state() removed - two observations published as one.
// Deleted rather than left unused: keeping a second, racy way to obtain
// these values is how the two callers drift apart again.)

// A2.6 duplicate/replay cache: bounded, fixed-size, FIFO-evicted, keyed on
// (session, request_id), each entry tagged with a monotonic admission
// ordinal.
//
// MEMORY DISCIPLINE, and the reason this entry looks the way it does.
//
// This cache is static, and every byte of it is a byte the rest of the
// firmware does not get. The first cut of the enlarged cache stored the
// request payload (513 B) AND the composed result JSON (385 B) per entry -
// ~968 B x 32 = ~31 KB - which consumed exactly the heap margin the 4D.4
// machine-directory tests' multi-kilobyte calloc()s live in, and six of
// them began failing allocation on hardware whenever an MQTT client was
// connected (each broker client is an 8 KB heap task stack). Two
// reductions, neither of which changes A2.6 behaviour:
//
// 1. Payload identity is (length, SHA-256), not the payload bytes.
//    Length plus SHA-256 is byte-identity for every purpose this
//    comparison serves - it only disambiguates replay-vs-conflict among
//    authenticated, capability-checked principals, and constructing a
//    same-length SHA-256 collision is not a capability this threat model
//    contains.
// 2. The result is stored as the FIELDS it is composed from, not as the
//    composed JSON. A replay recomposes byte-identical output because
//    compose_authority_result_json() is a pure function of exactly these
//    fields, and every string among them is a static literal or a bounded
//    vocabulary value. Storing the fields is not a summary of the result -
//    it is the result's complete input set.
//
// Entry ~968 -> ~152 B. At ARGUS_SECURITY_MAX_MACHINES entries the whole
// table is ~2.4 KB, SMALLER than the original 8-entry table it replaces
// (~7.7 KB), so this pass now returns more heap than its diagnostic-stack
// raise consumed.
#define AUTH_DUP_HASH_LEN 32U
#define AUTH_DUP_STATUS_CAP 16U

typedef struct {
    bool in_use;
    uint32_t ordinal;
    char session[ARGUS_MQTT_SESSION_HEX_LEN + 1U];
    char request_id[ARGUS_MQTT_AUTHORITY_REQUEST_ID_MAX + 1U];
    // The KIND of operation is part of the identity, not just the id.
    // A2.6 names the key as (session, request_id); taken literally that
    // collides the two topics, and an acquisition and a release CAN be
    // byte-identical (the schemas differ only in that a release may not
    // carry epoch 0 - an acquisition may carry a nonzero epoch too). An
    // owner that reused one id for "acquire, then release" therefore had
    // its RELEASE answered from the acquisition's cached entry: replayed
    // as ACCEPTED/already_held, never executed, authority silently
    // retained. Keying on the kind as well closes that without weakening
    // A2.6 - a genuine redelivery still carries the same kind.
    bool release;
    uint8_t payload_hash[AUTH_DUP_HASH_LEN];
    size_t payload_len;
    // The complete input set of compose_authority_result_json(). outcome,
    // reason and owner_kind are always static string literals - every
    // caller passes either a literal or the return of decode_reason() /
    // authority_result_reason(), both of which return literals - so storing
    // the pointer is safe for the lifetime of the firmware image.
    const char *outcome;
    const char *reason;
    const char *owner_kind;
    uint32_t authority_epoch;
    char lease_status[AUTH_DUP_STATUS_CAP];
    char local_status[AUTH_DUP_STATUS_CAP];
} auth_dup_entry_t;

static auth_dup_entry_t s_auth_dup_cache[ARGUS_MQTT_AUTH_DUP_CACHE_SIZE];
static size_t s_auth_dup_next;
// Monotonic count of every DISTINCT (session, request_id) ever admitted past
// decode+session-check, whether or not it is still cache-resident. Exposed
// for tests and diagnostics; the honest limit on what it can prove is
// documented on argus_mqtt_runtime_duplicate_admission_count() below.
static uint32_t s_auth_dup_ordinal_next;

void argus_mqtt_runtime_reset_duplicate_cache(void)
{
    memset(s_auth_dup_cache, 0, sizeof(s_auth_dup_cache));
    s_auth_dup_next = 0U;
    s_auth_dup_ordinal_next = 0U;
}

uint32_t argus_mqtt_runtime_duplicate_admission_count(void)
{
    return s_auth_dup_ordinal_next;
}

static auth_dup_entry_t *auth_dup_find(const char *session, const char *request_id,
                                       bool release)
{
    for (size_t i = 0U; i < ARGUS_MQTT_AUTH_DUP_CACHE_SIZE; ++i) {
        auth_dup_entry_t *entry = &s_auth_dup_cache[i];
        if (entry->in_use && entry->release == release &&
            strcmp(entry->session, session) == 0 &&
            strcmp(entry->request_id, request_id) == 0) {
            return entry;
        }
    }
    return NULL;
}

// EVICTION, stated explicitly: round-robin over ARGUS_MQTT_AUTH_DUP_CACHE_SIZE
// slots. When the cache is full, the OLDEST entry (by insertion order, not by
// last use) is overwritten regardless of whether it is still "in flight" -
// deterministic, and the same rule at every rollover, not a special case.
//
// RESIDUAL LIMITATION, stated honestly rather than glossed over: an opaque,
// client-chosen `request_id` string carries no ordering information of its
// own. Once an entry is evicted, the controller has no way to tell "a
// request_id I have genuinely never seen" apart from "a request_id whose
// prior entry I have forgotten" - that distinction is fundamentally
// unavailable without either unbounded memory or a client-supplied monotonic
// field the current A2.2 schema does not define. This function therefore
// cannot implement a literal "outside-window" refusal for an arbitrary
// evicted key, and does not claim to.
//
// What IS bounded-safe, and is the actual answer to the dangerous case
// (final-focused-pass item 4's "old ArgusCore acquisition redelivery after
// HMI fallback acquisition" scenario): a cache-miss falls through to full,
// ordinary admission - including the item-1 stale-epoch check and the
// TRANSFER_EPOCH_REQUIRED rule in argus_mqtt_session_request_authority_with_state().
// A stale, evicted redelivery that would TRANSFER the lease away from
// whoever holds it now is refused there regardless of whether its cache
// entry survived: a nonzero epoch that no longer matches is stale (item 1);
// an epoch of 0 cannot transfer at all once a different owner exists (item
// 4's TRANSFER_EPOCH_REQUIRED). Only requests that could NOT disturb an
// existing different owner - a grant onto an unowned lease, or a
// same-principal renewal - can succeed on a cache-miss, and neither of those
// outcomes is dangerous to re-arbitrate. See
// argus_mqtt_session_request_authority_with_state()'s own comment for the
// full argument.
static void auth_dup_hash(const char *payload, size_t payload_len,
                          uint8_t out[AUTH_DUP_HASH_LEN])
{
    // 0 = SHA-256 (not SHA-224). One-shot; failure cannot occur for RAM
    // input on this target, but zero the digest defensively so a failure
    // could never alias another entry's stored hash.
    if (mbedtls_sha256((const unsigned char *)payload, payload_len, out, 0) != 0) {
        memset(out, 0, AUTH_DUP_HASH_LEN);
    }
}

static void auth_dup_store(const char *session, const char *request_id,
                           bool release,
                           const char *payload, size_t payload_len,
                           const char *outcome, const char *reason,
                           const char *owner_kind, uint32_t authority_epoch,
                           const char *lease_status, const char *local_status)
{
    auth_dup_entry_t *slot = &s_auth_dup_cache[s_auth_dup_next];
    s_auth_dup_next = (s_auth_dup_next + 1U) % ARGUS_MQTT_AUTH_DUP_CACHE_SIZE;
    memset(slot, 0, sizeof(*slot));
    slot->ordinal = s_auth_dup_ordinal_next++;
    strlcpy(slot->session, session, sizeof(slot->session));
    strlcpy(slot->request_id, request_id, sizeof(slot->request_id));
    slot->release = release;
    auth_dup_hash(payload, payload_len, slot->payload_hash);
    slot->payload_len = payload_len;
    slot->outcome = outcome;
    slot->reason = reason;
    slot->owner_kind = owner_kind;
    slot->authority_epoch = authority_epoch;
    strlcpy(slot->lease_status, lease_status, sizeof(slot->lease_status));
    strlcpy(slot->local_status, local_status, sizeof(slot->local_status));
    slot->in_use = true;
}

// Recomposes a cached entry's original result. Byte-identical to what was
// emitted when the entry was stored, because compose_authority_result_json()
// is pure and every one of its inputs was captured at that moment - the
// replay does NOT re-derive owner/epoch/status from current state, which is
// exactly what A2.6 forbids ("the cached result is republished").
static void auth_dup_replay(const auth_dup_entry_t *entry,
                            char *out, size_t out_size)
{
    compose_authority_result_json(out, out_size, entry->request_id,
                                  entry->session, entry->outcome, entry->reason,
                                  entry->owner_kind, entry->authority_epoch,
                                  entry->lease_status, entry->local_status);
}

// Pure: builds the exact A2.4 result JSON from an explicitly passed core and
// admission context. No global state, no mutex - callable from
// argus_mqtt_authority_admit() with a synthetic core in a test exactly as it
// is called in production with the real one.
// The A2.4 result document, composed from explicit values only. Pure: given
// the same eight inputs it emits the same bytes, which is what lets a
// duplicate replay recompose an earlier result byte-identically from the
// stored fields instead of keeping a 385-byte copy of the document.
static void compose_authority_result_json(
    char *out, size_t out_size, const char *request_id, const char *session,
    const char *outcome, const char *reason, const char *owner_kind,
    uint32_t authority_epoch, const char *lease_status, const char *local_status)
{
    int written = snprintf(out, out_size,
        "{\"schema\":1,\"request_id\":\"%s\",\"session\":\"%s\",\"outcome\":\"%s\""
        ",\"reason\":\"%s\",\"control_owner\":\"%s\",\"authority_epoch\":%" PRIu32
        ",\"core_lease_status\":\"%s\",\"local_control_status\":\"%s\"}",
        request_id, session, outcome, reason, owner_kind, authority_epoch,
        lease_status, local_status);
    if (written < 0 || (size_t)written >= out_size) {
        strlcpy(out, "{\"outcome\":\"REJECTED\",\"reason\":\"result_overflow\"}", out_size);
    }
}

// Derives the owner/lease/local values from the live core, then composes.
// Also hands the derived values back to the caller (when the out-params are
// non-NULL) so the duplicate cache can store exactly what was composed
// rather than recomputing it later against changed state.
static void build_authority_result_json(
    char *out, size_t out_size, const argus_mqtt_authority_request_t *req,
    const argus_mqtt_session_core_t *core, uint8_t authority_profile,
    bool core_window_open, uint64_t now_ms, const char *outcome, const char *reason,
    const char **owner_kind_out, char *lease_status_out, size_t lease_status_cap,
    char *local_status_out, size_t local_status_cap)
{
    const char *owner_kind = "NONE";
    if (core->lease_machine_id[0] != '\0') {
        switch (core->lease_client_type) {
        case ARGUS_MACHINE_CLIENT_HMI:           owner_kind = "LOCAL_HMI"; break;
        case ARGUS_MACHINE_CLIENT_ARGUS_COMMAND: owner_kind = "ARGUSCORE"; break;
        case ARGUS_MACHINE_CLIENT_SERVICE_TOOL:  owner_kind = "SERVICE_TOOL"; break;
        default:                                 owner_kind = "OTHER"; break;
        }
    }
    char lease_status[AUTH_DUP_STATUS_CAP];
    char local_status[AUTH_DUP_STATUS_CAP];
    authority_status_from_core(core, authority_profile, core_window_open, now_ms,
                               lease_status, sizeof(lease_status),
                               local_status, sizeof(local_status));

    compose_authority_result_json(out, out_size,
        req != NULL ? req->request_id : "", core->session, outcome, reason,
        owner_kind, core->authority_epoch, lease_status, local_status);

    if (owner_kind_out != NULL) *owner_kind_out = owner_kind;
    if (lease_status_out != NULL) strlcpy(lease_status_out, lease_status, lease_status_cap);
    if (local_status_out != NULL) strlcpy(local_status_out, local_status, local_status_cap);
}

// Composes the outcome's result document, marks it for publication, and -
// unless `cache` is false - records the fields under (session, request_id)
// so a later byte-identical redelivery replays this exact result. Every
// admission exit that publishes goes through here, so "what was published"
// and "what a replay will reproduce" cannot drift apart.
static void emit_authority_result(
    argus_mqtt_authority_outcome_t *out,
    const argus_mqtt_authority_request_t *req,
    const argus_mqtt_broker_message_t *message,
    const argus_mqtt_session_core_t *core, uint8_t authority_profile,
    bool core_window_open, uint64_t now_ms,
    const char *outcome, const char *reason, bool release, bool cache)
{
    const char *owner_kind = "NONE";
    char lease_status[AUTH_DUP_STATUS_CAP];
    char local_status[AUTH_DUP_STATUS_CAP];
    build_authority_result_json(out->result_json, sizeof(out->result_json),
                                req, core, authority_profile, core_window_open,
                                now_ms, outcome, reason,
                                &owner_kind, lease_status, sizeof(lease_status),
                                local_status, sizeof(local_status));
    out->published = true;
    if (cache && req != NULL) {
        auth_dup_store(req->session, req->request_id, release,
                       message->payload, message->payload_len, outcome, reason,
                       owner_kind, core->authority_epoch, lease_status,
                       local_status);
    }
}

// Correction order §7. The production admission/handling path for
// command/core/request_authority and command/core/release_authority, taking
// the session core and message as explicit parameters instead of reading
// s_runtime, so this exact function - not a transcription of its gate order -
// is directly callable by a test. See argus_mqtt_runtime.h for the contract:
// callers must have already applied broker-level publish admission and the
// REQUEST_AUTHORITY capability check, matching what handle_authority_request
// below still does before calling in.
//
// A2.1 (retained/QoS refusal) and the capability check happen entirely
// outside this function - they are transport/authorization concerns the
// broker layer and its caller own, not this admission core's job, mirroring
// the split the pure argus_mqtt_session_* functions already use.
argus_mqtt_authority_outcome_t argus_mqtt_authority_admit(
    argus_mqtt_session_core_t *core, const argus_mqtt_broker_message_t *message,
    bool release, uint8_t authority_profile, bool core_window_open,
    bool machine_running, uint64_t now_ms)
{
    argus_mqtt_authority_outcome_t out = {0};

    if (message->retain) {
        out.stage = ARGUS_MQTT_AUTHORITY_ADMIT_RETAINED_REFUSED;
        return out;
    }
    if (message->qos != 1U) {
        out.stage = ARGUS_MQTT_AUTHORITY_ADMIT_QOS_REFUSED;
        // A2.5 places qos_1_required in the decode/envelope group, which
        // publishes a result. This previously returned with published=false,
        // so a client that used the wrong QoS got silence and no way to
        // diagnose it - the exact failure mode A2 was written to remove.
        // The request_id is not yet decoded at this point (decode happens
        // below, deliberately, so a malformed payload cannot be parsed
        // before the envelope is accepted), so the result carries an empty
        // request_id, same as a decode rejection.
        emit_authority_result(&out, NULL, message, core, authority_profile,
                              core_window_open, now_ms, "REJECTED",
                              "qos_1_required", release, false);
        return out;
    }

    argus_mqtt_authority_request_t req;
    out.decode = argus_mqtt_decode_authority_request(
        message->payload, message->payload_len, release, &req);
    if (out.decode != ARGUS_MQTT_DECODE_OK) {
        out.stage = ARGUS_MQTT_AUTHORITY_ADMIT_DECODE_REJECTED;
        // No request_id decoded: nothing to key a cache entry on.
        emit_authority_result(&out, NULL, message, core, authority_profile,
                              core_window_open, now_ms, "REJECTED",
                              decode_reason(out.decode), release, false);
        return out;
    }

    // Session binding. A request naming a previous controller session is
    // rejected with zero mutation.
    if (strcmp(req.session, core->session) != 0) {
        out.stage = ARGUS_MQTT_AUTHORITY_ADMIT_SESSION_MISMATCH;
        // NOT cached. A2.7 requires zero mutation for a prior-session
        // request, and caching one was worse than a mere technicality: the
        // entry is keyed on the SENDER's session, which by definition never
        // equals the controller's, so the lookup above can never find it.
        // Every such entry was pure eviction pressure - any authenticated
        // principal holding REQUEST_AUTHORITY could flush the whole table
        // by publishing ARGUS_MQTT_AUTH_DUP_CACHE_SIZE well-formed requests
        // carrying a bogus session, destroying QoS-1 idempotency for every
        // legitimate in-flight request and breaking A2.6's claim that
        // occupancy is bounded by enrolled machines rather than publish
        // rate.
        emit_authority_result(&out, &req, message, core, authority_profile,
                              core_window_open, now_ms, "REJECTED",
                              "session_mismatch", release, false);
        return out;
    }

    // A2.6 duplicate handling, before any arbitration.
    auth_dup_entry_t *existing = auth_dup_find(req.session, req.request_id, release);
    if (existing != NULL) {
        uint8_t incoming_hash[AUTH_DUP_HASH_LEN];
        auth_dup_hash(message->payload, message->payload_len, incoming_hash);
        if (existing->payload_len == message->payload_len &&
            memcmp(existing->payload_hash, incoming_hash,
                   AUTH_DUP_HASH_LEN) == 0) {
            out.stage = ARGUS_MQTT_AUTHORITY_ADMIT_DUPLICATE_REPLAY;
            auth_dup_replay(existing, out.result_json, sizeof(out.result_json));
            out.published = true;
            return out;   // idempotent replay: no arbitration, no epoch change
        }
        out.stage = ARGUS_MQTT_AUTHORITY_ADMIT_DUPLICATE_CONFLICT;
        // Deliberately NOT cached (final-focused-pass item 4): the ORIGINAL
        // record stays the single canonical entry for this request identity.
        // Storing the conflict too would create a second record under the
        // same key - and once the original was evicted, the surviving
        // conflict record would quietly become canonical, replaying a
        // REJECTED result for the identity's legitimate original payload.
        // A redelivered conflict simply re-detects against the original and
        // gets the same refusal - idempotent without being cached.
        emit_authority_result(&out, &req, message, core, authority_profile,
                              core_window_open, now_ms, "REJECTED",
                              "duplicate_conflict", release, false);
        return out;
    }

    if (release) {
        // A2.3: the epoch must match exactly, so a delayed release from a
        // previous owner cannot release a later holder.
        if (req.authority_epoch != core->authority_epoch) {
            out.stage = ARGUS_MQTT_AUTHORITY_ADMIT_RELEASE_STALE_EPOCH;
            emit_authority_result(&out, &req, message, core, authority_profile,
                                  core_window_open, now_ms, "REJECTED",
                                  "stale_epoch", release, true);
            return out;
        }
        bool released = argus_mqtt_session_release_authority(
            core, message->principal.identifier);
        out.stage = released ? ARGUS_MQTT_AUTHORITY_ADMIT_RELEASE_ACCEPTED
                              : ARGUS_MQTT_AUTHORITY_ADMIT_RELEASE_NOT_OWNER;
        out.state_changed = released;
        out.link_online = false;
        emit_authority_result(&out, &req, message, core, authority_profile,
                              core_window_open, now_ms,
                              released ? "ACCEPTED" : "REJECTED",
                              released ? "released" : "not_owner", release, true);
        return out;
    }

    // Final-focused-pass item 1: this check previously existed for releases
    // only. A2.2 defines the same predicate for acquisition requests -
    // epoch 0 means "no assumption" and is always permitted; a NONZERO
    // epoch is a claim about the current epoch, and if it does not match,
    // the request is stale and must be refused before arbitration or
    // mutation, exactly like a stale release. This is what stops a request
    // built against a since-superseded epoch from being evaluated as if it
    // were current.
    if (req.authority_epoch != 0U && req.authority_epoch != core->authority_epoch) {
        out.stage = ARGUS_MQTT_AUTHORITY_ADMIT_REQUEST_STALE_EPOCH;
        emit_authority_result(&out, &req, message, core, authority_profile,
                              core_window_open, now_ms, "REJECTED",
                              "stale_epoch", release, true);
        return out;
    }

    argus_mqtt_authority_result_t result = argus_mqtt_session_request_authority_with_state(
        core, message->connection_id, message->principal.identifier,
        message->principal.client_type, authority_profile, core_window_open,
        machine_running, req.authority_epoch, now_ms);
    out.stage = ARGUS_MQTT_AUTHORITY_ADMIT_REQUEST_EVALUATED;
    out.request_result = result;
    bool accepted = (result == ARGUS_MQTT_AUTHORITY_GRANTED ||
                      result == ARGUS_MQTT_AUTHORITY_ALREADY_HELD);
    out.state_changed = accepted;
    out.link_online = true;
    emit_authority_result(&out, &req, message, core, authority_profile,
                          core_window_open, now_ms,
                          accepted ? "ACCEPTED" : "REJECTED",
                          authority_result_reason(result), release, true);
    return out;
}

// Runtime glue: unpacks s_runtime, gathers the admission context that only
// the runtime can know (commissioned profile, acquisition window, current
// machine state), calls the real admission function under the session
// mutex, then performs the I/O the outcome calls for. All decision logic
// lives in argus_mqtt_authority_admit(); nothing here is decision logic.
static void handle_authority_request(const argus_mqtt_broker_message_t *message,
                                     bool release)
{
    if (!message->policy_admitted) return;

    // Authorization is checked HERE, before admission, and never inside the
    // pure core or argus_mqtt_authority_admit(). A principal without
    // REQUEST_AUTHORITY must not be able to influence ownership at all.
    if ((message->principal.permissions & ARGUS_PERMISSION_REQUEST_AUTHORITY) == 0U) {
        ESP_LOGW(TAG, "authority %s refused: machine %s lacks request_authority",
                 release ? "release" : "request", message->principal.identifier);
        return;
    }

    uint8_t profile = commissioned_authority_profile();
    bool window_open = core_acquisition_window_open();
    argus_state_snapshot_t machine_state = {0};
    argus_state_mgr_get_snapshot(&machine_state);
    // A2.8's "running" is "the motor is actively being driven" - STARTING,
    // RUNNING, DECELERATING per the trajectory/step-generator states
    // documented in V2_CONTROLLER_ARCHITECTURE.md. HOLDING has step
    // generation stopped (driver still enabled, no motion), so a transfer
    // there needs no bumpless tracking and is not gated by this rule.
    bool machine_running = machine_state.machine_state == ARGUS_STATE_STARTING ||
                           machine_state.machine_state == ARGUS_STATE_RUNNING ||
                           machine_state.machine_state == ARGUS_STATE_DECELERATING;
    uint64_t now = now_ms();

    xSemaphoreTake(s_runtime.mutex, portMAX_DELAY);
    // Captured BEFORE admit() so a rebind (ALREADY_HELD reached while the
    // link was not already ONLINE) can be told apart from an ordinary
    // renewal - see the CORE_REACQUISITION handling below.
    bool was_online_before = s_runtime.session.link == ARGUS_MQTT_LINK_ONLINE;
    argus_mqtt_authority_outcome_t outcome = argus_mqtt_authority_admit(
        &s_runtime.session, message, release, profile, window_open,
        machine_running, now);
    xSemaphoreGive(s_runtime.mutex);

    switch (outcome.stage) {
    case ARGUS_MQTT_AUTHORITY_ADMIT_RETAINED_REFUSED:
        // A2.1: a retained request would let the broker replay an
        // acquisition after a session change.
        ESP_LOGW(TAG, "authority request refused: retained");
        return;
    case ARGUS_MQTT_AUTHORITY_ADMIT_QOS_REFUSED:
        ESP_LOGW(TAG, "authority request refused: qos_1_required");
        return;
    case ARGUS_MQTT_AUTHORITY_ADMIT_DECODE_REJECTED:
        ESP_LOGW(TAG, "authority request rejected: %s", decode_reason(outcome.decode));
        break;
    case ARGUS_MQTT_AUTHORITY_ADMIT_RELEASE_ACCEPTED:
    case ARGUS_MQTT_AUTHORITY_ADMIT_RELEASE_NOT_OWNER:
        // Audited unconditionally: a refusal is as interesting as a grant
        // when working out why a release did not take effect.
        ESP_LOGW(TAG, "authority release by %s: %s",
                 message->principal.identifier,
                 outcome.stage == ARGUS_MQTT_AUTHORITY_ADMIT_RELEASE_ACCEPTED
                     ? "accepted" : "refused_not_holder");
        {
            const char *reason = argus_mqtt_authority_reason_for_release(
                outcome.stage == ARGUS_MQTT_AUTHORITY_ADMIT_RELEASE_ACCEPTED);
            if (reason != NULL) set_authority_reason(reason);
        }
        break;
    case ARGUS_MQTT_AUTHORITY_ADMIT_REQUEST_EVALUATED:
        ESP_LOGW(TAG, "authority request by %s (type=%u): %s (profile=%u window=%d running=%d)",
                 message->principal.identifier, (unsigned)message->principal.client_type,
                 authority_result_reason(outcome.request_result), (unsigned)profile,
                 (int)window_open, (int)machine_running);
        {
            const char *reason = argus_mqtt_authority_reason_for_request(
                outcome.request_result, was_online_before,
                message->principal.client_type, profile);
            if (reason != NULL) set_authority_reason(reason);
        }
        break;
    default:
        break;
    }

    if (outcome.published) {
        publish_value(s_runtime.topics.event_authority_result, outcome.result_json, false);
    }
    if (outcome.state_changed) {
        publish_value(s_runtime.topics.state_supervisor_link,
                      outcome.link_online ? "ONLINE" : "OFFLINE", true);
        publish_authority_state();
    }
}

// Retained so any client that connects later - or reconnects after a drop -
// learns who holds control without having to catch a transient event. This is
// what lets a panel say "ArgusCore has control" instead of the opaque
// "another interface has control" it could manage before.
static void publish_authority_state(void)
{
    // ONE snapshot under ONE mutex acquisition, then everything is derived
    // from that copy. This previously took the mutex twice - once here for
    // owner/type/epoch, again inside authority_status_snapshot() for the
    // lease/local status - and published the two together as if they were
    // one observation. Three tasks reach this function (the runtime task via
    // request/heartbeat handling, a broker client task via the disconnect
    // callback, and status_task at 1 Hz via argus_mqtt_runtime_tick), so a
    // lease expiring between the two acquisitions published an owner and
    // epoch from BEFORE the expiry alongside a lease status from AFTER it:
    // control_owner=LOCAL_HMI with core_lease_status=EXPIRED and
    // local_control_status=AVAILABLE simultaneously. A2.10 requires all six
    // retained values to be internally consistent after every transition.
    argus_mqtt_session_core_t snapshot;
    xSemaphoreTake(s_runtime.mutex, portMAX_DELAY);
    snapshot = s_runtime.session;
    xSemaphoreGive(s_runtime.mutex);

    uint32_t epoch = snapshot.authority_epoch;
    uint8_t profile = commissioned_authority_profile();
    bool window_open = core_acquisition_window_open();

    // Ownership is lease presence alone. NOT gated on link == ONLINE: a
    // disconnected-but-unexpired holder is still the truthful owner, per
    // A2.11.
    const char *owner_kind = "NONE";
    if (snapshot.lease_machine_id[0] != '\0') {
        switch (snapshot.lease_client_type) {
        case ARGUS_MACHINE_CLIENT_HMI:           owner_kind = "LOCAL_HMI"; break;
        case ARGUS_MACHINE_CLIENT_ARGUS_COMMAND: owner_kind = "ARGUSCORE"; break;
        case ARGUS_MACHINE_CLIENT_SERVICE_TOOL:  owner_kind = "SERVICE_TOOL"; break;
        default:                                 owner_kind = "OTHER"; break;
        }
    }

    char lease_status[AUTH_DUP_STATUS_CAP];
    char local_status[AUTH_DUP_STATUS_CAP];
    authority_status_from_core(&snapshot, profile, window_open, now_ms(),
                               lease_status, sizeof(lease_status),
                               local_status, sizeof(local_status));
    publish_value(s_runtime.topics.status_core_lease_status, lease_status, true);
    publish_value(s_runtime.topics.status_authority_reason, s_authority_reason, true);

    publish_value(s_runtime.topics.status_authority_profile,
                  profile == ARGUS_AUTHORITY_PROFILE_ARGUSCORE_PREFERRED
                      ? "ARGUSCORE_PREFERRED" : "STANDALONE_HMI", true);
    publish_value(s_runtime.topics.status_control_owner, owner_kind, true);
    publish_number(s_runtime.topics.status_authority_epoch, epoch);
    publish_value(s_runtime.topics.status_local_control, local_status, true);
}

static void handle_command(const argus_mqtt_broker_message_t *message,
                           argus_mqtt_action_t action)
{
    argus_mqtt_command_t command;
    const argus_config_t *config = argus_config_get();
    argus_mqtt_decode_result_t decode = argus_mqtt_decode_command(
        message->payload, message->payload_len, action,
        config->max_output_milli_rpm, &command);
    if (decode != ARGUS_MQTT_DECODE_OK) {
        ESP_LOGW(TAG, "command decode rejected: action=%s reason=%s",
                 argus_mqtt_action_name(action), decode_reason(decode));
        return;
    }

    if (!message->policy_admitted) {
        reject_decoded(&command, message->retain ? "retained_forbidden" : "topic_forbidden");
        return;
    }
    if (message->qos != 1U) {
        reject_decoded(&command, "qos_1_required");
        return;
    }

    char cached_result[ARGUS_MQTT_BROKER_PAYLOAD_CAP] = {0};
    argus_mqtt_sequence_result_t sequence;
    xSemaphoreTake(s_runtime.mutex, portMAX_DELAY);
    argus_mqtt_command_admission_t admission = argus_mqtt_command_admission_check(
        &s_runtime.session, message->principal.identifier,
        message->connection_id, &command);
    if (admission != ARGUS_MQTT_COMMAND_ADMIT_OK) {
        uint32_t current = s_runtime.session.authority_epoch;
        xSemaphoreGive(s_runtime.mutex);
        switch (admission) {
        case ARGUS_MQTT_COMMAND_ADMIT_SESSION_MISMATCH:
            reject_decoded(&command, "session_mismatch");
            break;
        case ARGUS_MQTT_COMMAND_ADMIT_NOT_HOLDER:
            reject_decoded(&command, "not_authority_holder");
            break;
        default:
            ESP_LOGW(TAG, "command rejected: stale authority epoch %" PRIu32
                     " (current %" PRIu32 ")", command.authority_epoch, current);
            reject_decoded(&command, "stale_epoch");
            break;
        }
        return;
    }
    sequence = argus_mqtt_session_check_sequence(
        &s_runtime.session, &command, message->payload);
    if (sequence == ARGUS_MQTT_SEQUENCE_DUPLICATE) {
        strlcpy(cached_result, s_runtime.session.cached_result, sizeof(cached_result));
    }
    xSemaphoreGive(s_runtime.mutex);

    if (sequence == ARGUS_MQTT_SEQUENCE_DUPLICATE) {
        publish_value(s_runtime.topics.command_result, cached_result, false);
        return;
    }
    if (sequence == ARGUS_MQTT_SEQUENCE_CONFLICT) {
        reject_decoded(&command, "sequence_conflict");
        return;
    }
    if (sequence == ARGUS_MQTT_SEQUENCE_STALE) {
        reject_decoded(&command, "stale_sequence");
        return;
    }

    argus_authority_snapshot_t authority = {0};
    if (argus_authority_mgr_get_snapshot(&authority) != ESP_OK) {
        reject_decoded(&command, "authority_unavailable");
        return;
    }
    argus_command_envelope_t envelope = {
        .source = ARGUS_CMD_SRC_MQTT_SUPERVISORY,
        .command_type = command_type(action),
        .authority_generation = authority.generation,
        .target_rpm_milli = command.target_rpm_milli,
        .forward = command.forward,
    };
    if (argus_cmd_router_check_authority(&envelope) != ESP_OK) {
        reject_decoded(&command, "authority_rejected");
        return;
    }

    esp_err_t dispatch = argus_cmd_router_dispatch(&envelope);
    char result[ARGUS_MQTT_BROKER_PAYLOAD_CAP];
    build_result(result, sizeof(result), &command,
                 dispatch == ESP_OK ? "ACCEPTED" : "REJECTED",
                 dispatch == ESP_OK ? "accepted" : "state_rejected");
    xSemaphoreTake(s_runtime.mutex, portMAX_DELAY);
    argus_mqtt_session_commit_result(&s_runtime.session, &command,
                                     message->payload, result);
    xSemaphoreGive(s_runtime.mutex);
    publish_value(s_runtime.topics.command_result, result, false);
    publish_operational_snapshot();
}

static void handle_message(const argus_mqtt_broker_message_t *message)
{
    argus_mqtt_action_t action = argus_mqtt_topics_classify(
        &s_runtime.topics, message->topic);
    if (action == ARGUS_MQTT_ACTION_HEARTBEAT) {
        handle_heartbeat(message);
    } else if (action == ARGUS_MQTT_ACTION_REQUEST_AUTHORITY) {
        handle_authority_request(message, false);
    } else if (action == ARGUS_MQTT_ACTION_RELEASE_AUTHORITY) {
        handle_authority_request(message, true);
    } else if (action != ARGUS_MQTT_ACTION_NONE) {
        handle_command(message, action);
    }
}

static void runtime_task(void *arg)
{
    (void)arg;
    runtime_event_t event;
    while (true) {
        if (xQueueReceive(s_runtime.queue, &event, portMAX_DELAY) != pdTRUE) continue;
        if (event.type == RUNTIME_EVENT_PUBLISH_BASELINE) {
            publish_baseline();
        } else {
            handle_message(&event.data.message);
        }
    }
}

static void broker_message_cb(const argus_mqtt_broker_message_t *message,
                              void *user_ctx)
{
    (void)user_ctx;
    runtime_event_t event = {.type = RUNTIME_EVENT_MESSAGE};
    event.data.message = *message;
    if (xQueueSend(s_runtime.queue, &event, 0) != pdTRUE) {
        ESP_LOGE(TAG, "protocol queue full; MQTT application message dropped");
    }
}

static esp_err_t broker_policy_cb(const argus_mqtt_broker_message_t *message,
                                  void *user_ctx)
{
    (void)user_ctx;
    return argus_mqtt_topics_external_publish_allowed(&s_runtime.topics, message)
               ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static argus_machine_auth_outcome_t broker_auth_cb(
    uint32_t peer_key, const argus_mqtt_connect_request_t *request,
    uint8_t receiving_interface, void *user_ctx)
{
    (void)user_ctx;
    if (request == NULL) {
        return (argus_machine_auth_outcome_t) {
            .result = ARGUS_MACHINE_AUTH_INVALID_REQUEST,
        };
    }
    argus_machine_auth_outcome_t outcome =
        argus_machine_service_authenticate(
        peer_key, request->client_id,
        request->username, request->username_len,
        request->password, request->password_len,
        receiving_interface);
    argus_audit_event_type_t event =
        outcome.result == ARGUS_MACHINE_AUTH_SUCCESS
            ? ARGUS_AUDIT_MQTT_AUTH_SUCCESS
            : outcome.result == ARGUS_MACHINE_AUTH_THROTTLED ||
                      outcome.result == ARGUS_MACHINE_AUTH_BUSY
                  ? ARGUS_AUDIT_MQTT_AUTH_THROTTLED
                  : ARGUS_AUDIT_MQTT_AUTH_FAILURE;
    const char *reason =
        outcome.result == ARGUS_MACHINE_AUTH_SUCCESS
            ? "authenticated"
            : outcome.result == ARGUS_MACHINE_AUTH_TRANSPORT_REJECTED
                  ? "transport_rejected"
                  : outcome.result == ARGUS_MACHINE_AUTH_INTERFACE_REJECTED
                        ? "interface_rejected"
                        : outcome.result == ARGUS_MACHINE_AUTH_THROTTLED
                              ? "throttled"
                              : outcome.result == ARGUS_MACHINE_AUTH_BUSY
                                    ? "kdf_busy"
                                    : "invalid_credentials";
    uint64_t now_us = (uint64_t)esp_timer_get_time();
    bool audit = outcome.result == ARGUS_MACHINE_AUTH_SUCCESS ||
        s_last_mqtt_auth_failure_audit_us == 0U ||
        now_us - s_last_mqtt_auth_failure_audit_us >= UINT64_C(60000000);
    if (audit) {
        (void)argus_security_audit_append(
            event,
            outcome.result == ARGUS_MACHINE_AUTH_SUCCESS
                ? ARGUS_AUDIT_OUTCOME_SUCCESS
                : ARGUS_AUDIT_OUTCOME_REJECTED,
            outcome.result == ARGUS_MACHINE_AUTH_SUCCESS
                ? ARGUS_PRINCIPAL_MACHINE
                : ARGUS_PRINCIPAL_NONE,
            outcome.result == ARGUS_MACHINE_AUTH_SUCCESS
                ? outcome.principal.identifier
                : "anonymous",
            "mqtt_connect",
            receiving_interface == ARGUS_MACHINE_INTERFACE_SOFTAP
                ? "SOFTAP"
                : receiving_interface == ARGUS_MACHINE_INTERFACE_STA
                      ? "STA"
                      : "UNKNOWN",
            reason, outcome.principal.record_security_epoch, false);
        if (outcome.result != ARGUS_MACHINE_AUTH_SUCCESS) {
            s_last_mqtt_auth_failure_audit_us = now_us;
        }
    }
    return outcome;
}

static esp_err_t broker_revalidate_cb(
    const argus_mqtt_broker_client_info_t *client, void *user_ctx)
{
    (void)user_ctx;
    return client == NULL
               ? ESP_ERR_INVALID_ARG
               : argus_machine_service_revalidate(
                     &client->principal, client->receiving_interface);
}

static esp_err_t broker_subscribe_policy_cb(
    const argus_mqtt_broker_client_info_t *client,
    const char *filter, void *user_ctx)
{
    (void)user_ctx;
    if (client == NULL || filter == NULL ||
        (client->principal.permissions &
         ARGUS_PERMISSION_VIEW_STATUS) == 0U ||
        !argus_mqtt_security_subscription_allowed(
            &s_runtime.topics, &client->principal, filter)) {
        uint64_t now_us = (uint64_t)esp_timer_get_time();
        if (s_last_mqtt_policy_audit_us == 0U ||
            now_us - s_last_mqtt_policy_audit_us >= UINT64_C(60000000)) {
            (void)argus_security_audit_append(
                ARGUS_AUDIT_MQTT_POLICY_REJECTED,
                ARGUS_AUDIT_OUTCOME_REJECTED,
                ARGUS_PRINCIPAL_MACHINE,
                client->principal.identifier,
                "mqtt_subscribe", "MQTT",
                "subscription_denied",
                client->principal.record_security_epoch, false);
            s_last_mqtt_policy_audit_us = now_us;
        }
        return ESP_ERR_NOT_ALLOWED;
    }
    return ESP_OK;
}

static esp_err_t broker_publish_authorize_cb(
    const argus_mqtt_broker_message_t *message, void *user_ctx)
{
    (void)user_ctx;
    if (message == NULL) {
        return ESP_ERR_NOT_ALLOWED;
    }
    if (argus_mqtt_security_publish_allowed(
            &s_runtime.topics, &message->principal, message->topic)) {
        return ESP_OK;
    }
    uint64_t now_us = (uint64_t)esp_timer_get_time();
    if (s_last_mqtt_policy_audit_us == 0U ||
        now_us - s_last_mqtt_policy_audit_us >= UINT64_C(60000000)) {
        (void)argus_security_audit_append(
            ARGUS_AUDIT_MQTT_POLICY_REJECTED,
            ARGUS_AUDIT_OUTCOME_REJECTED,
            ARGUS_PRINCIPAL_MACHINE,
            message->principal.identifier,
            "mqtt_publish", "MQTT", "capability_denied",
            message->principal.record_security_epoch, false);
        s_last_mqtt_policy_audit_us = now_us;
    }
    return ESP_ERR_NOT_ALLOWED;
}

static void broker_client_cb(argus_mqtt_broker_client_event_t broker_event,
                             const argus_mqtt_broker_client_info_t *client,
                             void *user_ctx)
{
    (void)user_ctx;
    if (broker_event == ARGUS_MQTT_BROKER_CLIENT_CONNECTED) {
        runtime_event_t event = {.type = RUNTIME_EVENT_PUBLISH_BASELINE};
        if (xQueueSend(s_runtime.queue, &event, 0) != pdTRUE) {
            ESP_LOGW(TAG, "protocol queue full; reconnect baseline deferred to periodic refresh");
        }
    } else if (broker_event == ARGUS_MQTT_BROKER_CLIENT_DISCONNECTED) {
        xSemaphoreTake(s_runtime.mutex, portMAX_DELAY);
        bool changed = argus_mqtt_session_disconnect(
            &s_runtime.session, client->connection_id);
        // Whether a lease still exists AFTER the disconnect decides whether
        // this event is the reason the state looks the way it does.
        bool still_owned = s_runtime.session.lease_machine_id[0] != '\0';
        xSemaphoreGive(s_runtime.mutex);
        if (changed) {
            // §4: disconnect is a listed transition requiring a coherent
            // republish. The lease itself survives (see
            // argus_mqtt_session_disconnect) but core_lease_status moves
            // toward EXPIRING, which the retained snapshot must reflect -
            // previously only state/supervisor/link was republished here,
            // leaving the six A2.10 values stale until some other event
            // happened to touch them.
            //
            // TRANSPORT_LOST only while a lease actually survives this
            // disconnect. argus_mqtt_session_disconnect() also returns true
            // for a socket that merely still held the heartbeat binding of
            // an ALREADY-EXPIRED lease (tick() deliberately leaves that
            // binding in place to keep rejecting replays). Setting the
            // reason unconditionally overwrote CORE_LEASE_EXPIRED with
            // TRANSPORT_LOST in that case, attributing an expiry to a
            // transport event - and A2.11 requires expiry to republish "with
            // an accurate reason". When no lease remains, whatever reason
            // expiry already recorded is the accurate one; leave it.
            if (still_owned) {
                set_authority_reason(argus_mqtt_authority_reason_for_disconnect());
            }
            publish_value(s_runtime.topics.state_supervisor_link, "OFFLINE", true);
            publish_authority_state();
        }
    }
}

esp_err_t argus_mqtt_runtime_init(void)
{
    if (s_runtime.initialized) return ESP_ERR_INVALID_STATE;
    s_runtime.mutex = xSemaphoreCreateMutex();
    s_runtime.queue = xQueueCreate(ARGUS_MQTT_RUNTIME_QUEUE_DEPTH, sizeof(runtime_event_t));
    if (s_runtime.mutex == NULL || s_runtime.queue == NULL) return ESP_ERR_NO_MEM;
    if (xTaskCreate(runtime_task, "mqtt_protocol", ARGUS_MQTT_RUNTIME_TASK_STACK,
                    NULL, 5, &s_runtime.task) != pdPASS) return ESP_ERR_NO_MEM;
    s_runtime.initialized = true;
    return ESP_OK;
}

esp_err_t argus_mqtt_runtime_prepare_start(void)
{
    if (!s_runtime.initialized || argus_mqtt_broker_is_running()) return ESP_ERR_INVALID_STATE;
    argus_identity_t identity;
    esp_err_t err = argus_identity_get(&identity);
    if (err != ESP_OK) return err;

    char session[ARGUS_MQTT_SESSION_HEX_LEN + 1U];
    uint32_t random_high;
    uint32_t random_low;
    do {
        random_high = esp_random();
        random_low = esp_random();
    } while (random_high == 0U && random_low == 0U);
    err = argus_mqtt_session_format(random_high, random_low,
                                    session, sizeof(session));
    if (err != ESP_OK) return err;

    xQueueReset(s_runtime.queue);
    xSemaphoreTake(s_runtime.mutex, portMAX_DELAY);
    s_runtime.identity = identity;
    err = argus_mqtt_topics_build(&s_runtime.topics,
                                  identity.client_id, identity.unit_id);
    if (err != ESP_OK) {
        s_runtime.prepared = false;
        xSemaphoreGive(s_runtime.mutex);
        return err;
    }
    argus_mqtt_session_core_init(&s_runtime.session, session);
    s_runtime.prepared = true;
    // A2.6: the duplicate cache is scoped to the controller session. A new
    // session invalidates every previous session's requests and duplicate
    // records (A2.7); the lookup being session-keyed already guarantees a
    // stale entry can never match, but clearing it here is deliberate
    // hygiene so a fresh broker lifecycle starts from a known-empty cache
    // rather than depending on that property alone.
    //
    // INSIDE the mutex. This memset ran unlocked, while every other access
    // to the table (auth_dup_find / auth_dup_store, both reached only
    // through argus_mqtt_authority_admit) is serialised by this same mutex -
    // and the two touch points are different tasks: prepare_start runs on
    // the network manager's task via the broker-start callback, while the
    // runtime task drains queued authority messages. On a broker restart
    // with messages still queued from the previous lifecycle, the wipe could
    // race a store and leave an entry marked in-use with a zeroed or
    // half-written key, or reset the insert cursor mid-insert.
    argus_mqtt_runtime_reset_duplicate_cache();
    s_authority_ready_ms = 0U;
    s_fallback_window_was_open = true;
    xSemaphoreGive(s_runtime.mutex);
    ESP_LOGI(TAG, "prepared Phase 4C MQTT root=%s session=%s",
             s_runtime.topics.root, session);
    return ESP_OK;
}

void argus_mqtt_runtime_get_broker_config(uint16_t port,
                                          argus_mqtt_broker_config_t *out)
{
    if (out == NULL) return;
    *out = (argus_mqtt_broker_config_t) {
        .port = port,
        .on_message = broker_message_cb,
        .publish_authorize = broker_publish_authorize_cb,
        .policy_check = broker_policy_cb,
        .authenticate = broker_auth_cb,
        .revalidate = broker_revalidate_cb,
        .subscribe_policy = broker_subscribe_policy_cb,
        .on_client_event = broker_client_cb,
        .user_ctx = NULL,
    };
}

esp_err_t argus_mqtt_runtime_broker_started(void)
{
    // Anchor the acquisition window here, not at MCU boot.
    argus_mqtt_runtime_mark_authority_ready();
    if (!s_runtime.prepared || !argus_mqtt_broker_is_running()) return ESP_ERR_INVALID_STATE;
    runtime_event_t event = {.type = RUNTIME_EVENT_PUBLISH_BASELINE};
    return xQueueSend(s_runtime.queue, &event, 0) == pdTRUE
               ? ESP_OK : ESP_ERR_NO_MEM;
}

void argus_mqtt_runtime_tick(void)
{
    if (!s_runtime.prepared || !argus_mqtt_broker_is_running()) return;
    xSemaphoreTake(s_runtime.mutex, portMAX_DELAY);
    bool stale = argus_mqtt_session_tick(&s_runtime.session, now_ms());
    xSemaphoreGive(s_runtime.mutex);
    if (stale) {
        ESP_LOGW(TAG, "supervisor heartbeat stale; motion state intentionally unchanged");
        // §4: lease expiry is a listed transition requiring a coherent
        // republish. Previously nothing here touched the six A2.10 topics at
        // all - core_lease_status would sit at its pre-expiry value (ACTIVE
        // or EXPIRING) until some unrelated event happened to republish it.
        set_authority_reason(argus_mqtt_authority_reason_for_expiry());
        publish_authority_state();
    }

    // §4 "fallback": the ArgusCore acquisition window closing with no owner
    // is not itself triggered by any message, so it is polled here. Reason
    // is deliberately left untouched - ownership has not changed, only
    // local_control_status's WAITING -> AVAILABLE value has, and
    // authority_reason describes why the CURRENT state is what it is, which
    // is still whatever it was (typically STARTUP, since nobody has
    // acquired). No A2.10 vocabulary entry exists for "fallback became
    // available and unclaimed" specifically, and inventing one here was not
    // part of this correction order - flagged for Shawn rather than guessed.
    bool window_open_now = core_acquisition_window_open();
    xSemaphoreTake(s_runtime.mutex, portMAX_DELAY);
    bool unowned = s_runtime.session.lease_machine_id[0] == '\0';
    xSemaphoreGive(s_runtime.mutex);
    if (s_fallback_window_was_open && !window_open_now && unowned) {
        ESP_LOGI(TAG, "ArgusCore acquisition window closed with no owner; local fallback now available");
        publish_authority_state();
    }
    s_fallback_window_was_open = window_open_now;

    publish_operational_snapshot();
}

esp_err_t argus_mqtt_runtime_get_session(char *out, size_t out_size)
{
    if (out == NULL || out_size < ARGUS_MQTT_SESSION_HEX_LEN + 1U ||
        !s_runtime.prepared) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_runtime.mutex, portMAX_DELAY);
    strlcpy(out, s_runtime.session.session, out_size);
    xSemaphoreGive(s_runtime.mutex);
    return ESP_OK;
}
