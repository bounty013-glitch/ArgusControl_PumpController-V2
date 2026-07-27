#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "argus_cmd_parser.h"
#include "argus_mqtt_broker.h"

#define ARGUS_MQTT_SESSION_HEX_LEN 16U
#define ARGUS_MQTT_COMMAND_ID_MAX 36U
#define ARGUS_MQTT_HEARTBEAT_TIMEOUT_MS 6000U

typedef enum {
    ARGUS_MQTT_ACTION_NONE = 0,
    ARGUS_MQTT_ACTION_SET_TARGET,
    ARGUS_MQTT_ACTION_START,
    ARGUS_MQTT_ACTION_STOP,
    ARGUS_MQTT_ACTION_UNLOCK,
    ARGUS_MQTT_ACTION_E_STOP,
    ARGUS_MQTT_ACTION_RESET_E_STOP,
    ARGUS_MQTT_ACTION_RECOVER,
    ARGUS_MQTT_ACTION_HEARTBEAT,
    // Amendment A1: authority is acquired explicitly, never by connecting
    // or by heartbeating.
    ARGUS_MQTT_ACTION_REQUEST_AUTHORITY,
    ARGUS_MQTT_ACTION_RELEASE_AUTHORITY,
} argus_mqtt_action_t;

typedef enum {
    ARGUS_MQTT_LINK_OFFLINE = 0,
    ARGUS_MQTT_LINK_ONLINE,
    ARGUS_MQTT_LINK_STALE,
} argus_mqtt_link_state_t;

typedef enum {
    ARGUS_MQTT_DECODE_OK = 0,
    ARGUS_MQTT_DECODE_INVALID_ARGUMENT,
    ARGUS_MQTT_DECODE_MALFORMED,
    ARGUS_MQTT_DECODE_UNKNOWN_FIELD,
    ARGUS_MQTT_DECODE_DUPLICATE_FIELD,
    ARGUS_MQTT_DECODE_MISSING_FIELD,
    ARGUS_MQTT_DECODE_INVALID_VALUE,
    ARGUS_MQTT_DECODE_TOO_LARGE,
    // A2.5 gives these their own wire vocabulary distinct from the generic
    // invalid_value, because a client reacts differently to "you asked for a
    // schema version I don't speak" than to "one field failed validation".
    ARGUS_MQTT_DECODE_SCHEMA_UNSUPPORTED,
    ARGUS_MQTT_DECODE_INVALID_REQUEST_ID,
} argus_mqtt_decode_result_t;

typedef enum {
    ARGUS_MQTT_SEQUENCE_FIRST = 0,
    ARGUS_MQTT_SEQUENCE_NEWER,
    ARGUS_MQTT_SEQUENCE_DUPLICATE,
    ARGUS_MQTT_SEQUENCE_CONFLICT,
    ARGUS_MQTT_SEQUENCE_STALE,
} argus_mqtt_sequence_result_t;

typedef struct {
    char root[72];
    char command_set_target[ARGUS_MQTT_BROKER_TOPIC_CAP];
    char command_start[ARGUS_MQTT_BROKER_TOPIC_CAP];
    char command_stop[ARGUS_MQTT_BROKER_TOPIC_CAP];
    char command_unlock[ARGUS_MQTT_BROKER_TOPIC_CAP];
    char command_e_stop[ARGUS_MQTT_BROKER_TOPIC_CAP];
    char command_reset_e_stop[ARGUS_MQTT_BROKER_TOPIC_CAP];
    char command_recover[ARGUS_MQTT_BROKER_TOPIC_CAP];
    char heartbeat[ARGUS_MQTT_BROKER_TOPIC_CAP];
    // Authority is a core concern, not a pump1 concern, so these sit under
    // command/core/ rather than command/pump1/.
    char command_request_authority[ARGUS_MQTT_BROKER_TOPIC_CAP];
    char command_release_authority[ARGUS_MQTT_BROKER_TOPIC_CAP];
    // A2.1/A2.4. The authority result is a distinct application result from
    // event/pump1/command_result: an authority request is not a pump command
    // and must not be reported on the pump's own result channel.
    char event_authority_result[ARGUS_MQTT_BROKER_TOPIC_CAP];
    char status_control_owner[ARGUS_MQTT_BROKER_TOPIC_CAP];
    char status_authority_epoch[ARGUS_MQTT_BROKER_TOPIC_CAP];
    char status_authority_profile[ARGUS_MQTT_BROKER_TOPIC_CAP];
    char status_local_control[ARGUS_MQTT_BROKER_TOPIC_CAP];
    char status_core_lease_status[ARGUS_MQTT_BROKER_TOPIC_CAP];
    char status_authority_reason[ARGUS_MQTT_BROKER_TOPIC_CAP];
    char metadata_device_name[ARGUS_MQTT_BROKER_TOPIC_CAP];
    char metadata_model[ARGUS_MQTT_BROKER_TOPIC_CAP];
    char metadata_firmware_version[ARGUS_MQTT_BROKER_TOPIC_CAP];
    char metadata_hardware_uid[ARGUS_MQTT_BROKER_TOPIC_CAP];
    char state_online[ARGUS_MQTT_BROKER_TOPIC_CAP];
    char state_supervisor_link[ARGUS_MQTT_BROKER_TOPIC_CAP];
    char state_mode[ARGUS_MQTT_BROKER_TOPIC_CAP];
    char state_driver[ARGUS_MQTT_BROKER_TOPIC_CAP];
    char state_direction[ARGUS_MQTT_BROKER_TOPIC_CAP];
    char state_estop[ARGUS_MQTT_BROKER_TOPIC_CAP];
    char state_fault[ARGUS_MQTT_BROKER_TOPIC_CAP];
    char status_wifi[ARGUS_MQTT_BROKER_TOPIC_CAP];
    char status_mqtt[ARGUS_MQTT_BROKER_TOPIC_CAP];
    char status_network_mode[ARGUS_MQTT_BROKER_TOPIC_CAP];
    char status_authority_mode[ARGUS_MQTT_BROKER_TOPIC_CAP];
    char status_authority_owner[ARGUS_MQTT_BROKER_TOPIC_CAP];
    char status_uptime_s[ARGUS_MQTT_BROKER_TOPIC_CAP];
    char status_command_session[ARGUS_MQTT_BROKER_TOPIC_CAP];
    char status_last_accepted_sequence[ARGUS_MQTT_BROKER_TOPIC_CAP];
    char telemetry_configured_target[ARGUS_MQTT_BROKER_TOPIC_CAP];
    char telemetry_trajectory_target[ARGUS_MQTT_BROKER_TOPIC_CAP];
    char telemetry_applied[ARGUS_MQTT_BROKER_TOPIC_CAP];
    char telemetry_generated[ARGUS_MQTT_BROKER_TOPIC_CAP];
    char telemetry_step_count[ARGUS_MQTT_BROKER_TOPIC_CAP];
    char telemetry_feedback_available[ARGUS_MQTT_BROKER_TOPIC_CAP];
    char command_result[ARGUS_MQTT_BROKER_TOPIC_CAP];
} argus_mqtt_topics_t;

typedef struct {
    char session[ARGUS_MQTT_SESSION_HEX_LEN + 1U];
    uint32_t sequence;
    char command_id[ARGUS_MQTT_COMMAND_ID_MAX + 1U];
    // Amendment A2.9. REQUIRED. The authority epoch this command is
    // predicated on, validated against the controller's current epoch before
    // the sequence is consumed or any state moves.
    uint32_t authority_epoch;
    argus_mqtt_action_t action;
    int32_t target_rpm_milli;
    bool forward;
} argus_mqtt_command_t;

typedef struct {
    char session[ARGUS_MQTT_SESSION_HEX_LEN + 1U];
    uint32_t counter;
} argus_mqtt_heartbeat_t;

// Amendment A2.2/A2.3 request envelope. Bounded and strictly typed; a
// publish to the right topic is not a request until this decodes and
// validates.
#define ARGUS_MQTT_AUTHORITY_REQUEST_ID_MAX 36U
#define ARGUS_MQTT_AUTHORITY_INTENT_MAX 24U
#define ARGUS_MQTT_AUTHORITY_PAYLOAD_MAX 512U

typedef struct {
    uint32_t schema;
    char request_id[ARGUS_MQTT_AUTHORITY_REQUEST_ID_MAX + 1U];
    char session[ARGUS_MQTT_SESSION_HEX_LEN + 1U];
    uint32_t authority_epoch;
    char intent[ARGUS_MQTT_AUTHORITY_INTENT_MAX + 1U];
    bool has_intent;
} argus_mqtt_authority_request_t;

// Strict decode of an authority request or release. `is_release` tightens
// the epoch rule: a request may carry 0 meaning "no assumption", a release
// may not - it must name the epoch it is releasing, so a delayed release
// from a previous owner cannot release a later one.
argus_mqtt_decode_result_t argus_mqtt_decode_authority_request(
    const char *payload, size_t payload_len, bool is_release,
    argus_mqtt_authority_request_t *out);

// Outcome of an explicit authority request. Every denial is distinct because
// the operator-facing reason differs: "another interface has control" and
// "this installation is commissioned standalone" call for different actions.
typedef enum {
    ARGUS_MQTT_AUTHORITY_GRANTED = 0,       // ownership changed, epoch advanced
    ARGUS_MQTT_AUTHORITY_ALREADY_HELD,      // requester already owns it; no epoch change
    ARGUS_MQTT_AUTHORITY_DENIED_HELD,       // a higher- or equal-standing holder has it
    ARGUS_MQTT_AUTHORITY_DENIED_PROFILE,    // commissioned profile forbids this client type
    ARGUS_MQTT_AUTHORITY_DENIED_INVALID,    // malformed request
    // A2.8. An otherwise-admissible ArgusCore transfer from LOCAL_HMI/
    // SERVICE_TOOL, refused only because the pump is actively driving the
    // motor. Live running transfer awaits proven bumpless PID tracking; until
    // then it is deferred, not simulated, and the current owner keeps
    // authority while the pump keeps running.
    ARGUS_MQTT_AUTHORITY_TRANSFER_UNSUPPORTED_RUNNING,
    // Final-focused-pass item 4. A request that would TRANSFER the lease
    // away from a different, currently-recorded owner must name the epoch
    // it is transferring from - "no assumption" (epoch 0) is only valid for
    // a request that cannot displace anyone (granting an unowned lease, or
    // a same-principal renewal). This is what closes the gap where a stale,
    // evicted, epoch-0 redelivery of an old transfer request could
    // re-preempt whoever holds authority now: once the real owner has
    // changed, the epoch has advanced, so an epoch-0 replay is refused here
    // and a same-epoch replay is refused earlier, at admission, by the
    // ordinary stale-epoch check. See argus_mqtt_runtime.c's duplicate-cache
    // documentation for the full argument.
    ARGUS_MQTT_AUTHORITY_TRANSFER_EPOCH_REQUIRED,
    // The commissioned profile PERMITS this requester, but only after the
    // bounded ArgusCore acquisition window closes. Distinct from
    // DENIED_PROFILE because the operator remedy is completely different:
    // this one resolves itself within seconds, whereas a profile denial
    // requires recommissioning the unit. Reporting the two identically told
    // an operator watching an HMI wait out ArgusCore's startup window to go
    // recommission a correctly-commissioned controller.
    ARGUS_MQTT_AUTHORITY_DENIED_WINDOW_OPEN,
} argus_mqtt_authority_result_t;

typedef struct {
    char session[ARGUS_MQTT_SESSION_HEX_LEN + 1U];
    argus_mqtt_link_state_t link;
    // Who holds the command lease. lease_machine_id is the stable
    // authenticated identity and is what the lease actually belongs to;
    // lease_connection_id proves that holder is still on the far end of a
    // live socket and changes on every reconnect. Keying the lease to the
    // connection alone locked a supervisor out of its own lease for up to
    // ARGUS_MQTT_HEARTBEAT_TIMEOUT_MS after any drop, which is a routine
    // event on field cellular. Empty lease_machine_id means "no identity
    // recorded" and preserves the pre-identity behaviour exactly.
    char lease_machine_id[ARGUS_SECURITY_ID_MAX + 1U];
    uint8_t lease_client_type;
    uint64_t lease_connection_id;
    // Authority epoch. Increments on every OWNERSHIP CHANGE - initial grant,
    // transfer, and release - and never on a renewal. Commands carrying a
    // stale epoch are rejected, which is what makes a transfer immediately
    // invalidate the previous owner's in-flight commands without having to
    // chase them. Starts at 0 meaning "never owned".
    uint32_t authority_epoch;
    uint64_t heartbeat_connection_id;
    uint32_t heartbeat_counter;
    uint64_t last_heartbeat_ms;
    bool has_sequence;
    uint32_t last_sequence;
    argus_mqtt_action_t cached_action;
    char cached_command_id[ARGUS_MQTT_COMMAND_ID_MAX + 1U];
    char cached_payload[ARGUS_MQTT_BROKER_PAYLOAD_CAP];
    char cached_result[ARGUS_MQTT_BROKER_PAYLOAD_CAP];
} argus_mqtt_session_core_t;

esp_err_t argus_mqtt_topics_build(argus_mqtt_topics_t *out,
                                   const char *client_id,
                                   const char *unit_id);
argus_mqtt_action_t argus_mqtt_topics_classify(
    const argus_mqtt_topics_t *topics, const char *topic);
bool argus_mqtt_topics_external_publish_allowed(
    const argus_mqtt_topics_t *topics,
    const argus_mqtt_broker_message_t *message);
const char *argus_mqtt_action_name(argus_mqtt_action_t action);
const char *argus_mqtt_link_name(argus_mqtt_link_state_t link);

argus_mqtt_decode_result_t argus_mqtt_decode_heartbeat(
    const char *payload, size_t payload_len, argus_mqtt_heartbeat_t *out);
argus_mqtt_decode_result_t argus_mqtt_decode_command(
    const char *payload, size_t payload_len, argus_mqtt_action_t action,
    int32_t max_rpm_milli, argus_mqtt_command_t *out);

void argus_mqtt_session_core_init(argus_mqtt_session_core_t *core,
                                  const char *session);
esp_err_t argus_mqtt_session_format(uint32_t high, uint32_t low,
                                    char *out, size_t out_size);
bool argus_mqtt_session_is_newer(uint32_t candidate, uint32_t reference);
// Explicit authority acquisition (Phase 4C Amendment A1). Connection proves
// presence; this is what grants control.
//
// `authority_profile` is the COMMISSIONED argus_authority_profile_t, not
// anything inferred from live traffic. `core_window_open` is true only while
// the bounded startup acquisition window is still running under
// ARGUSCORE_PREFERRED; it is what keeps an early-booting HMI view-only until
// ArgusCore has had its chance, so that a shorter boot time can never convert
// into control.
//
// Capability (ARGUS_PERMISSION_REQUEST_AUTHORITY) is deliberately NOT checked
// here - this core is pure and has no security store. The caller must reject
// an unpermitted principal BEFORE calling. Keeping authorization and
// arbitration separate is the same split the rest of the system uses.
argus_mqtt_authority_result_t argus_mqtt_session_request_authority(
    argus_mqtt_session_core_t *core, uint64_t connection_id,
    const char *machine_id, uint8_t client_type,
    uint8_t authority_profile, bool core_window_open, uint64_t now_ms);

// Thin, additive wrapper around argus_mqtt_session_request_authority() that
// adds the machine-state- and epoch-aware rules the wire contract requires
// before a TRANSFER (taking the lease from a different, currently-recorded
// owner) may proceed, without mutating anything until the base function's
// own admission runs. `machine_running` is the caller's own judgement of
// "actively driving" (STARTING, RUNNING, or DECELERATING) - this core has no
// state-manager coupling and is not the place that decision belongs.
// `supplied_epoch` is the request's own `authority_epoch` field, already
// known by the caller to be either 0 or equal to the current epoch - a
// nonzero MISMATCH is rejected earlier, at admission, before arbitration is
// ever reached (final-focused-pass item 1).
//
// Ordering matters and is deliberate: commissioning-profile permission is
// checked FIRST. A requester the commissioned profile forbids outright
// (e.g. ArgusCore against a unit commissioned STANDALONE_HMI) must be
// refused `denied_by_profile` regardless of machine state or epoch - neither
// "the pump is running" nor "you didn't name an epoch" is the real remedy,
// and reporting either would misdirect the operator. Only a requester the
// profile WOULD permit is evaluated against the running-transfer and
// transfer-epoch rules; everyone else falls straight through to the base
// function, which reproduces the same profile check and returns
// DENIED_PROFILE on its own.
//
// Deliberately NOT folded into the base function: the base function is
// exercised directly by dozens of existing tests that have no notion of
// machine state or this wrapper's epoch argument, and every one of them is
// implicitly "not running, no assumption", which is still a real and current
// case. Duplicating the arbitration logic here would risk the two drifting;
// this wrapper delegates entirely to the base function once its own checks
// clear.
argus_mqtt_authority_result_t argus_mqtt_session_request_authority_with_state(
    argus_mqtt_session_core_t *core, uint64_t connection_id,
    const char *machine_id, uint8_t client_type,
    uint8_t authority_profile, bool core_window_open, bool machine_running,
    uint32_t supplied_epoch, uint64_t now_ms);

// Voluntary release by the current holder. Advances the epoch, so commands
// still in flight from the releasing owner become invalid immediately.
// Returns false if the caller is not the holder - releasing someone else's
// authority is not a thing any client may do.
bool argus_mqtt_session_release_authority(
    argus_mqtt_session_core_t *core, const char *machine_id);

// machine_id is the authenticated identity of the heartbeat's sender, taken
// from the broker message's principal. It may be NULL or empty, in which
// case arbitration falls back to connection identity exactly as before.
// client_type is recorded for the holder topic and for the precedence rules
// planned in Phase 3; it does not affect arbitration in this pass.
esp_err_t argus_mqtt_session_accept_heartbeat(
    argus_mqtt_session_core_t *core, uint64_t connection_id,
    const char *machine_id, uint8_t client_type,
    const argus_mqtt_heartbeat_t *heartbeat, uint64_t now_ms);
bool argus_mqtt_session_tick(argus_mqtt_session_core_t *core, uint64_t now_ms);
bool argus_mqtt_session_disconnect(argus_mqtt_session_core_t *core,
                                   uint64_t connection_id);
argus_mqtt_sequence_result_t argus_mqtt_session_check_sequence(
    const argus_mqtt_session_core_t *core,
    const argus_mqtt_command_t *command,
    const char *payload);
void argus_mqtt_session_commit_result(argus_mqtt_session_core_t *core,
                                      const argus_mqtt_command_t *command,
                                      const char *payload,
                                      const char *result_json);
