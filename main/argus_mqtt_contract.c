#include "argus_mqtt_contract.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "argus_identity.h"
#include "argus_nvs_config.h"   // ARGUS_AUTHORITY_PROFILE_*

typedef struct {
    const char *p;
    const char *end;
} json_cursor_t;

static esp_err_t make_topic(char *out, size_t out_size,
                            const char *root, const char *leaf)
{
    int written = snprintf(out, out_size, "%s/%s", root, leaf);
    return written > 0 && (size_t)written < out_size
               ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

#define MAKE_TOPIC(field, leaf) \
    do { \
        if (make_topic(out->field, sizeof(out->field), out->root, leaf) != ESP_OK) \
            return ESP_ERR_INVALID_SIZE; \
    } while (0)

esp_err_t argus_mqtt_topics_build(argus_mqtt_topics_t *out,
                                   const char *client_id,
                                   const char *unit_id)
{
    if (out == NULL || !argus_identity_validate_client_id(client_id) ||
        !argus_identity_validate_unit_id(unit_id)) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));
    int written = snprintf(out->root, sizeof(out->root), "argus/%s/%s",
                           client_id, unit_id);
    if (written <= 0 || (size_t)written >= sizeof(out->root)) {
        return ESP_ERR_INVALID_SIZE;
    }

    MAKE_TOPIC(command_set_target, "command/pump1/set_target_rpm_milli");
    MAKE_TOPIC(command_start, "command/pump1/start");
    MAKE_TOPIC(command_stop, "command/pump1/stop");
    MAKE_TOPIC(command_unlock, "command/pump1/unlock");
    MAKE_TOPIC(command_e_stop, "command/pump1/e_stop");
    MAKE_TOPIC(command_reset_e_stop, "command/pump1/reset_e_stop");
    MAKE_TOPIC(command_recover, "command/pump1/recover");
    MAKE_TOPIC(heartbeat, "status/supervisor/heartbeat");
    MAKE_TOPIC(command_request_authority, "command/core/request_authority");
    MAKE_TOPIC(command_release_authority, "command/core/release_authority");
    MAKE_TOPIC(status_control_owner, "status/core/control_owner");
    MAKE_TOPIC(status_authority_epoch, "status/core/authority_epoch");
    MAKE_TOPIC(status_authority_profile, "status/core/authority_profile");
    MAKE_TOPIC(status_local_control, "status/core/local_control_status");
    MAKE_TOPIC(status_core_lease_status, "status/core/core_lease_status");
    MAKE_TOPIC(status_authority_reason, "status/core/authority_reason");
    MAKE_TOPIC(metadata_device_name, "metadata/core/device_name");
    MAKE_TOPIC(metadata_model, "metadata/core/model");
    MAKE_TOPIC(metadata_firmware_version, "metadata/core/firmware_version");
    MAKE_TOPIC(metadata_hardware_uid, "metadata/core/hardware_uid");
    MAKE_TOPIC(state_online, "state/core/online");
    MAKE_TOPIC(state_supervisor_link, "state/supervisor/link");
    MAKE_TOPIC(state_mode, "state/pump1/mode");
    MAKE_TOPIC(state_driver, "state/pump1/driver");
    MAKE_TOPIC(state_direction, "state/pump1/direction");
    MAKE_TOPIC(state_estop, "state/pump1/estop");
    MAKE_TOPIC(state_fault, "state/pump1/fault");
    MAKE_TOPIC(status_wifi, "status/core/wifi");
    MAKE_TOPIC(status_mqtt, "status/core/mqtt");
    MAKE_TOPIC(status_network_mode, "status/core/network_mode");
    MAKE_TOPIC(status_authority_mode, "status/core/authority_mode");
    MAKE_TOPIC(status_authority_owner, "status/core/authority_owner");
    MAKE_TOPIC(status_uptime_s, "status/core/uptime_s");
    MAKE_TOPIC(status_command_session, "status/core/command_session");
    MAKE_TOPIC(status_last_accepted_sequence, "status/core/last_accepted_sequence");
    MAKE_TOPIC(telemetry_configured_target, "telemetry/pump1/configured_target_rpm_milli");
    MAKE_TOPIC(telemetry_trajectory_target, "telemetry/pump1/trajectory_target_rpm_milli");
    MAKE_TOPIC(telemetry_applied, "telemetry/pump1/applied_rpm_milli");
    MAKE_TOPIC(telemetry_generated, "telemetry/pump1/generated_rpm_milli");
    MAKE_TOPIC(telemetry_step_count, "telemetry/pump1/generated_step_count");
    MAKE_TOPIC(telemetry_feedback_available, "telemetry/pump1/feedback_available");
    MAKE_TOPIC(command_result, "event/pump1/command_result");
    return ESP_OK;
}

#undef MAKE_TOPIC

argus_mqtt_action_t argus_mqtt_topics_classify(
    const argus_mqtt_topics_t *topics, const char *topic)
{
    if (topics == NULL || topic == NULL) return ARGUS_MQTT_ACTION_NONE;
    if (strcmp(topic, topics->command_set_target) == 0) return ARGUS_MQTT_ACTION_SET_TARGET;
    if (strcmp(topic, topics->command_start) == 0) return ARGUS_MQTT_ACTION_START;
    if (strcmp(topic, topics->command_stop) == 0) return ARGUS_MQTT_ACTION_STOP;
    if (strcmp(topic, topics->command_unlock) == 0) return ARGUS_MQTT_ACTION_UNLOCK;
    if (strcmp(topic, topics->command_e_stop) == 0) return ARGUS_MQTT_ACTION_E_STOP;
    if (strcmp(topic, topics->command_reset_e_stop) == 0) return ARGUS_MQTT_ACTION_RESET_E_STOP;
    if (strcmp(topic, topics->command_recover) == 0) return ARGUS_MQTT_ACTION_RECOVER;
    if (strcmp(topic, topics->heartbeat) == 0) return ARGUS_MQTT_ACTION_HEARTBEAT;
    if (strcmp(topic, topics->command_request_authority) == 0) return ARGUS_MQTT_ACTION_REQUEST_AUTHORITY;
    if (strcmp(topic, topics->command_release_authority) == 0) return ARGUS_MQTT_ACTION_RELEASE_AUTHORITY;
    return ARGUS_MQTT_ACTION_NONE;
}

bool argus_mqtt_topics_external_publish_allowed(
    const argus_mqtt_topics_t *topics,
    const argus_mqtt_broker_message_t *message)
{
    if (topics == NULL || message == NULL || message->retain ||
        message->payload_len >= sizeof(message->payload) ||
        memchr(message->payload, '\0', message->payload_len) != NULL) return false;
    return argus_mqtt_topics_classify(topics, message->topic) != ARGUS_MQTT_ACTION_NONE;
}

const char *argus_mqtt_action_name(argus_mqtt_action_t action)
{
    switch (action) {
    case ARGUS_MQTT_ACTION_SET_TARGET: return "set_target_rpm_milli";
    case ARGUS_MQTT_ACTION_START: return "start";
    case ARGUS_MQTT_ACTION_STOP: return "stop";
    case ARGUS_MQTT_ACTION_UNLOCK: return "unlock";
    case ARGUS_MQTT_ACTION_E_STOP: return "e_stop";
    case ARGUS_MQTT_ACTION_RESET_E_STOP: return "reset_e_stop";
    case ARGUS_MQTT_ACTION_RECOVER: return "recover";
    case ARGUS_MQTT_ACTION_HEARTBEAT: return "heartbeat";
    case ARGUS_MQTT_ACTION_REQUEST_AUTHORITY: return "request_authority";
    case ARGUS_MQTT_ACTION_RELEASE_AUTHORITY: return "release_authority";
    default: return "unknown";
    }
}

const char *argus_mqtt_link_name(argus_mqtt_link_state_t link)
{
    switch (link) {
    case ARGUS_MQTT_LINK_ONLINE: return "ONLINE";
    case ARGUS_MQTT_LINK_STALE: return "STALE";
    default: return "OFFLINE";
    }
}

static void skip_ws(json_cursor_t *cursor)
{
    while (cursor->p < cursor->end &&
           (*cursor->p == ' ' || *cursor->p == '\t' ||
            *cursor->p == '\r' || *cursor->p == '\n')) {
        cursor->p++;
    }
}

static bool consume(json_cursor_t *cursor, char expected)
{
    skip_ws(cursor);
    if (cursor->p >= cursor->end || *cursor->p != expected) return false;
    cursor->p++;
    return true;
}

static bool parse_string(json_cursor_t *cursor, char *out, size_t out_size)
{
    skip_ws(cursor);
    if (cursor->p >= cursor->end || *cursor->p++ != '"' || out_size == 0U) return false;
    size_t len = 0;
    while (cursor->p < cursor->end && *cursor->p != '"') {
        unsigned char c = (unsigned char)*cursor->p++;
        if (c < 0x20U || c == '\\' || len + 1U >= out_size) return false;
        out[len++] = (char)c;
    }
    if (cursor->p >= cursor->end || *cursor->p++ != '"') return false;
    out[len] = '\0';
    return true;
}

static bool parse_u32(json_cursor_t *cursor, uint32_t *out)
{
    skip_ws(cursor);
    if (cursor->p >= cursor->end || !isdigit((unsigned char)*cursor->p)) return false;
    uint64_t value = 0;
    const char *start = cursor->p;
    while (cursor->p < cursor->end && isdigit((unsigned char)*cursor->p)) {
        value = value * 10U + (uint32_t)(*cursor->p++ - '0');
        if (value > UINT32_MAX) return false;
    }
    if (cursor->p - start > 1 && *start == '0') return false;
    if (cursor->p < cursor->end &&
        (*cursor->p == '.' || *cursor->p == 'e' || *cursor->p == 'E')) return false;
    *out = (uint32_t)value;
    return true;
}

static bool parse_true(json_cursor_t *cursor)
{
    skip_ws(cursor);
    if ((size_t)(cursor->end - cursor->p) < 4U ||
        memcmp(cursor->p, "true", 4U) != 0) return false;
    cursor->p += 4;
    return true;
}

static bool valid_session(const char *session)
{
    if (strlen(session) != ARGUS_MQTT_SESSION_HEX_LEN) return false;
    for (size_t i = 0; i < ARGUS_MQTT_SESSION_HEX_LEN; ++i) {
        if (!isdigit((unsigned char)session[i]) &&
            (session[i] < 'a' || session[i] > 'f')) return false;
    }
    return true;
}

static bool valid_command_id(const char *command_id)
{
    size_t len = strlen(command_id);
    if (len == 0U || len > ARGUS_MQTT_COMMAND_ID_MAX) return false;
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)command_id[i];
        if (!isalnum(c) && c != '-' && c != '_' && c != '.' && c != ':') return false;
    }
    return true;
}

static argus_mqtt_decode_result_t validate_payload(
    const char *payload, size_t payload_len, char *copy, size_t copy_size)
{
    if (payload == NULL || copy == NULL || copy_size == 0U) return ARGUS_MQTT_DECODE_INVALID_ARGUMENT;
    if (payload_len == 0U) return ARGUS_MQTT_DECODE_MALFORMED;
    if (payload_len >= copy_size) return ARGUS_MQTT_DECODE_TOO_LARGE;
    if (memchr(payload, '\0', payload_len) != NULL) return ARGUS_MQTT_DECODE_MALFORMED;
    memcpy(copy, payload, payload_len);
    copy[payload_len] = '\0';
    return ARGUS_MQTT_DECODE_OK;
}

argus_mqtt_decode_result_t argus_mqtt_decode_heartbeat(
    const char *payload, size_t payload_len, argus_mqtt_heartbeat_t *out)
{
    if (out == NULL) return ARGUS_MQTT_DECODE_INVALID_ARGUMENT;
    argus_mqtt_heartbeat_t decoded = {0};
    char copy[ARGUS_MQTT_BROKER_PAYLOAD_CAP];
    argus_mqtt_decode_result_t pre = validate_payload(payload, payload_len, copy, sizeof(copy));
    if (pre != ARGUS_MQTT_DECODE_OK) return pre;
    json_cursor_t cursor = {.p = copy, .end = copy + payload_len};
    if (!consume(&cursor, '{')) return ARGUS_MQTT_DECODE_MALFORMED;
    uint8_t fields = 0;
    while (true) {
        skip_ws(&cursor);
        if (cursor.p < cursor.end && *cursor.p == '}') { cursor.p++; break; }
        char key[24];
        if (!parse_string(&cursor, key, sizeof(key)) || !consume(&cursor, ':')) return ARGUS_MQTT_DECODE_MALFORMED;
        uint8_t bit = 0;
        bool valid = false;
        if (strcmp(key, "session") == 0) {
            bit = 1U;
            valid = parse_string(&cursor, decoded.session, sizeof(decoded.session)) && valid_session(decoded.session);
        } else if (strcmp(key, "counter") == 0) {
            bit = 2U;
            valid = parse_u32(&cursor, &decoded.counter) && decoded.counter != 0U;
        } else {
            return ARGUS_MQTT_DECODE_UNKNOWN_FIELD;
        }
        if ((fields & bit) != 0U) return ARGUS_MQTT_DECODE_DUPLICATE_FIELD;
        if (!valid) return ARGUS_MQTT_DECODE_INVALID_VALUE;
        fields |= bit;
        skip_ws(&cursor);
        if (cursor.p < cursor.end && *cursor.p == ',') { cursor.p++; continue; }
        if (cursor.p < cursor.end && *cursor.p == '}') { cursor.p++; break; }
        return ARGUS_MQTT_DECODE_MALFORMED;
    }
    skip_ws(&cursor);
    if (cursor.p != cursor.end) return ARGUS_MQTT_DECODE_MALFORMED;
    if (fields != 3U) return ARGUS_MQTT_DECODE_MISSING_FIELD;
    *out = decoded;
    return ARGUS_MQTT_DECODE_OK;
}

argus_mqtt_decode_result_t argus_mqtt_decode_command(
    const char *payload, size_t payload_len, argus_mqtt_action_t action,
    int32_t max_rpm_milli, argus_mqtt_command_t *out)
{
    if (out == NULL || action < ARGUS_MQTT_ACTION_SET_TARGET ||
        action > ARGUS_MQTT_ACTION_RECOVER || max_rpm_milli < 0) {
        return ARGUS_MQTT_DECODE_INVALID_ARGUMENT;
    }
    argus_mqtt_command_t decoded = {.action = action, .forward = true};
    char copy[ARGUS_MQTT_BROKER_PAYLOAD_CAP];
    argus_mqtt_decode_result_t pre = validate_payload(payload, payload_len, copy, sizeof(copy));
    if (pre != ARGUS_MQTT_DECODE_OK) return pre;
    json_cursor_t cursor = {.p = copy, .end = copy + payload_len};
    if (!consume(&cursor, '{')) return ARGUS_MQTT_DECODE_MALFORMED;
    uint8_t fields = 0;
    while (true) {
        skip_ws(&cursor);
        if (cursor.p < cursor.end && *cursor.p == '}') { cursor.p++; break; }
        char key[24];
        if (!parse_string(&cursor, key, sizeof(key)) || !consume(&cursor, ':')) return ARGUS_MQTT_DECODE_MALFORMED;
        uint8_t bit = 0;
        bool valid = false;
        if (strcmp(key, "session") == 0) {
            bit = 1U;
            valid = parse_string(&cursor, decoded.session, sizeof(decoded.session)) && valid_session(decoded.session);
        } else if (strcmp(key, "sequence") == 0) {
            bit = 2U;
            valid = parse_u32(&cursor, &decoded.sequence) && decoded.sequence != 0U;
        } else if (strcmp(key, "command_id") == 0) {
            bit = 4U;
            valid = parse_string(&cursor, decoded.command_id, sizeof(decoded.command_id)) && valid_command_id(decoded.command_id);
        } else if (strcmp(key, "value") == 0) {
            bit = 8U;
            if (action == ARGUS_MQTT_ACTION_SET_TARGET) {
                uint32_t target = 0;
                valid = parse_u32(&cursor, &target) && target <= (uint32_t)max_rpm_milli;
                decoded.target_rpm_milli = (int32_t)target;
            } else {
                valid = parse_true(&cursor);
            }
        } else if (strcmp(key, "authority_epoch") == 0) {
            // A2.9: required on every operational command. 0 decodes fine but
            // can never match a live epoch, so a client cannot opt out of
            // admission by sending 0.
            bit = 16U;
            valid = parse_u32(&cursor, &decoded.authority_epoch);
        } else {
            return ARGUS_MQTT_DECODE_UNKNOWN_FIELD;
        }
        if ((fields & bit) != 0U) return ARGUS_MQTT_DECODE_DUPLICATE_FIELD;
        if (!valid) return ARGUS_MQTT_DECODE_INVALID_VALUE;
        fields |= bit;
        skip_ws(&cursor);
        if (cursor.p < cursor.end && *cursor.p == ',') { cursor.p++; continue; }
        if (cursor.p < cursor.end && *cursor.p == '}') { cursor.p++; break; }
        return ARGUS_MQTT_DECODE_MALFORMED;
    }
    skip_ws(&cursor);
    if (cursor.p != cursor.end) return ARGUS_MQTT_DECODE_MALFORMED;
    // 31 = session|sequence|command_id|value|authority_epoch, all required.
    if (fields != 31U) return ARGUS_MQTT_DECODE_MISSING_FIELD;
    *out = decoded;
    return ARGUS_MQTT_DECODE_OK;
}

static bool valid_request_id(const char *id)
{
    size_t len = strlen(id);
    if (len == 0U || len > ARGUS_MQTT_AUTHORITY_REQUEST_ID_MAX) return false;
    for (size_t i = 0; i < len; ++i) {
        char ch = id[i];
        bool ok = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                  (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' || ch == '-';
        if (!ok) return false;
    }
    return true;
}

argus_mqtt_decode_result_t argus_mqtt_decode_authority_request(
    const char *payload, size_t payload_len, bool is_release,
    argus_mqtt_authority_request_t *out)
{
    if (out == NULL) return ARGUS_MQTT_DECODE_INVALID_ARGUMENT;
    if (payload_len > ARGUS_MQTT_AUTHORITY_PAYLOAD_MAX) {
        return ARGUS_MQTT_DECODE_TOO_LARGE;
    }
    argus_mqtt_authority_request_t decoded = {0};
    char copy[ARGUS_MQTT_BROKER_PAYLOAD_CAP];
    argus_mqtt_decode_result_t pre =
        validate_payload(payload, payload_len, copy, sizeof(copy));
    if (pre != ARGUS_MQTT_DECODE_OK) return pre;
    json_cursor_t cursor = {.p = copy, .end = copy + payload_len};
    if (!consume(&cursor, '{')) return ARGUS_MQTT_DECODE_MALFORMED;
    uint8_t fields = 0;
    while (true) {
        skip_ws(&cursor);
        if (cursor.p < cursor.end && *cursor.p == '}') { cursor.p++; break; }
        char key[24];
        if (!parse_string(&cursor, key, sizeof(key)) || !consume(&cursor, ':')) {
            return ARGUS_MQTT_DECODE_MALFORMED;
        }
        uint8_t bit = 0;
        bool valid = false;
        if (strcmp(key, "schema") == 0) {
            bit = 1U;
            valid = parse_u32(&cursor, &decoded.schema);
        } else if (strcmp(key, "request_id") == 0) {
            bit = 2U;
            valid = parse_string(&cursor, decoded.request_id,
                                 sizeof(decoded.request_id)) &&
                    valid_request_id(decoded.request_id);
        } else if (strcmp(key, "session") == 0) {
            bit = 4U;
            valid = parse_string(&cursor, decoded.session, sizeof(decoded.session)) &&
                    valid_session(decoded.session);
        } else if (strcmp(key, "authority_epoch") == 0) {
            bit = 8U;
            valid = parse_u32(&cursor, &decoded.authority_epoch);
        } else if (strcmp(key, "intent") == 0) {
            bit = 16U;   // optional
            valid = parse_string(&cursor, decoded.intent, sizeof(decoded.intent));
            decoded.has_intent = valid;
        } else {
            return ARGUS_MQTT_DECODE_UNKNOWN_FIELD;
        }
        if ((fields & bit) != 0U) return ARGUS_MQTT_DECODE_DUPLICATE_FIELD;
        if (!valid) return ARGUS_MQTT_DECODE_INVALID_VALUE;
        fields |= bit;
        skip_ws(&cursor);
        if (cursor.p < cursor.end && *cursor.p == ',') { cursor.p++; continue; }
        if (cursor.p < cursor.end && *cursor.p == '}') { cursor.p++; break; }
        return ARGUS_MQTT_DECODE_MALFORMED;
    }
    skip_ws(&cursor);
    if (cursor.p != cursor.end) return ARGUS_MQTT_DECODE_MALFORMED;
    // 15 = schema|request_id|session|authority_epoch. intent is optional.
    if ((fields & 15U) != 15U) return ARGUS_MQTT_DECODE_MISSING_FIELD;
    if (decoded.schema != 1U) return ARGUS_MQTT_DECODE_INVALID_VALUE;
    // A2.3: a release must name the epoch it releases. 0 is "no assumption",
    // which is meaningless for a release and is refused.
    if (is_release && decoded.authority_epoch == 0U) {
        return ARGUS_MQTT_DECODE_INVALID_VALUE;
    }
    *out = decoded;
    return ARGUS_MQTT_DECODE_OK;
}

void argus_mqtt_session_core_init(argus_mqtt_session_core_t *core,
                                  const char *session)
{
    if (core == NULL) return;
    memset(core, 0, sizeof(*core));
    core->link = ARGUS_MQTT_LINK_OFFLINE;
    if (session != NULL) strlcpy(core->session, session, sizeof(core->session));
}

esp_err_t argus_mqtt_session_format(uint32_t high, uint32_t low,
                                    char *out, size_t out_size)
{
    if (out == NULL || out_size < ARGUS_MQTT_SESSION_HEX_LEN + 1U ||
        (high == 0U && low == 0U)) return ESP_ERR_INVALID_ARG;
    int written = snprintf(out, out_size, "%08lx%08lx",
                           (unsigned long)high, (unsigned long)low);
    return written == (int)ARGUS_MQTT_SESSION_HEX_LEN
               ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

bool argus_mqtt_session_is_newer(uint32_t candidate, uint32_t reference)
{
    uint32_t delta = candidate - reference;
    return delta != 0U && delta < 0x80000000U;
}

// True when `machine_id` names the principal that currently holds the lease.
// Requires both sides to carry an identity: a blank stored identity means the
// lease predates identity tracking and must not match a named claimant, and a
// blank claimant must never match a named holder. Either way the caller falls
// back to connection-identity arbitration.
static bool lease_held_by(const argus_mqtt_session_core_t *core, const char *machine_id)
{
    return machine_id != NULL && machine_id[0] != '\0' &&
           core->lease_machine_id[0] != '\0' &&
           strcmp(core->lease_machine_id, machine_id) == 0;
}

static void lease_record_holder(argus_mqtt_session_core_t *core,
                                const char *machine_id, uint8_t client_type)
{
    if (machine_id != NULL && machine_id[0] != '\0') {
        snprintf(core->lease_machine_id, sizeof(core->lease_machine_id), "%s", machine_id);
        core->lease_client_type = client_type;
    } else {
        core->lease_machine_id[0] = '\0';
        core->lease_client_type = 0U;
    }
}

// Does the commissioned profile permit this client type to hold ordinary
// command authority at all? This is the rule that makes initial ownership a
// property of how the installation was commissioned rather than of who
// happened to connect first.
static bool profile_permits(uint8_t authority_profile, uint8_t client_type,
                            bool core_window_open)
{
    if (authority_profile == ARGUS_AUTHORITY_PROFILE_STANDALONE_HMI) {
        // No ArgusCore is assigned to this pump. The panel is the authority,
        // and a Core that turns up on the network cannot take control without
        // the unit being recommissioned.
        return client_type == ARGUS_MACHINE_CLIENT_HMI ||
               client_type == ARGUS_MACHINE_CLIENT_SERVICE_TOOL;
    }

    // ARGUSCORE_PREFERRED.
    if (client_type == ARGUS_MACHINE_CLIENT_ARGUS_COMMAND) return true;
    if (client_type == ARGUS_MACHINE_CLIENT_HMI ||
        client_type == ARGUS_MACHINE_CLIENT_SERVICE_TOOL) {
        // The local fallback, but only once ArgusCore has had its bounded
        // chance. While the window is open the panel stays view-only, so a
        // shorter boot time never converts into control.
        return !core_window_open;
    }
    return false;
}

// May `requester` take authority away from `holder`? Deliberately asymmetric,
// per the governing decision: ArgusCore may request a transfer from the panel
// and the controller adjudicates it, but the panel may NOT take authority from
// a healthy ArgusCore lease.
static bool may_preempt(uint8_t requester_type, uint8_t holder_type)
{
    return requester_type == ARGUS_MACHINE_CLIENT_ARGUS_COMMAND &&
           (holder_type == ARGUS_MACHINE_CLIENT_HMI ||
            holder_type == ARGUS_MACHINE_CLIENT_SERVICE_TOOL);
}

static void advance_epoch(argus_mqtt_session_core_t *core)
{
    // 0 is reserved for "never owned", so wrap to 1 rather than back to 0.
    core->authority_epoch++;
    if (core->authority_epoch == 0U) core->authority_epoch = 1U;
}

argus_mqtt_authority_result_t argus_mqtt_session_request_authority(
    argus_mqtt_session_core_t *core, uint64_t connection_id,
    const char *machine_id, uint8_t client_type,
    uint8_t authority_profile, bool core_window_open, uint64_t now_ms)
{
    if (core == NULL || connection_id == 0U ||
        machine_id == NULL || machine_id[0] == '\0' ||
        authority_profile > ARGUS_AUTHORITY_PROFILE_MAX) {
        return ARGUS_MQTT_AUTHORITY_DENIED_INVALID;
    }
    if (!profile_permits(authority_profile, client_type, core_window_open)) {
        return ARGUS_MQTT_AUTHORITY_DENIED_PROFILE;
    }

    bool held = (core->link == ARGUS_MQTT_LINK_ONLINE &&
                 core->lease_machine_id[0] != '\0');

    if (held && strcmp(core->lease_machine_id, machine_id) == 0) {
        // Already ours. Rebind to the current socket and refresh liveness, but
        // do NOT advance the epoch - nothing changed hands, and bumping it
        // would invalidate our own in-flight commands.
        core->lease_connection_id = connection_id;
        core->last_heartbeat_ms = now_ms;
        return ARGUS_MQTT_AUTHORITY_ALREADY_HELD;
    }

    if (held && !may_preempt(client_type, core->lease_client_type)) {
        return ARGUS_MQTT_AUTHORITY_DENIED_HELD;
    }

    // Grant: either unowned, or an admissible transfer. Ownership changed, so
    // the epoch advances and the previous owner's in-flight commands become
    // invalid immediately.
    lease_record_holder(core, machine_id, client_type);
    core->lease_connection_id = connection_id;
    core->heartbeat_connection_id = connection_id;
    core->heartbeat_counter = 0U;
    core->last_heartbeat_ms = now_ms;
    core->link = ARGUS_MQTT_LINK_ONLINE;
    advance_epoch(core);
    return ARGUS_MQTT_AUTHORITY_GRANTED;
}

bool argus_mqtt_session_release_authority(
    argus_mqtt_session_core_t *core, const char *machine_id)
{
    if (core == NULL || !lease_held_by(core, machine_id)) return false;
    lease_record_holder(core, NULL, 0U);
    core->lease_connection_id = 0U;
    core->heartbeat_connection_id = 0U;
    core->heartbeat_counter = 0U;
    core->link = ARGUS_MQTT_LINK_OFFLINE;
    advance_epoch(core);
    return true;
}

esp_err_t argus_mqtt_session_accept_heartbeat(
    argus_mqtt_session_core_t *core, uint64_t connection_id,
    const char *machine_id, uint8_t client_type,
    const argus_mqtt_heartbeat_t *heartbeat, uint64_t now_ms)
{
    if (core == NULL || heartbeat == NULL || connection_id == 0U ||
        strcmp(core->session, heartbeat->session) != 0) return ESP_ERR_INVALID_ARG;

    // Reconnect continuity. The lease belongs to the authenticated identity,
    // not to the socket, so the same principal arriving on a new connection
    // reclaims its own lease immediately instead of being refused until the
    // 6s heartbeat timeout expires - during which any other client could
    // have taken it. This does NOT let a different principal preempt; that
    // is Phase 3 §5.3 and is deliberately not implemented here.
    bool same_principal = lease_held_by(core, machine_id);
    if (core->lease_connection_id != 0U && core->lease_connection_id != connection_id &&
        core->link == ARGUS_MQTT_LINK_ONLINE && !same_principal) {
        return ESP_ERR_INVALID_STATE;
    }

    // Replay guard applies per connection. A reclaiming principal on a fresh
    // connection legitimately restarts its counter, so the guard is scoped to
    // the connection that produced the stored counter, as before.
    if (core->heartbeat_connection_id == connection_id && core->heartbeat_counter != 0U &&
        !argus_mqtt_session_is_newer(heartbeat->counter, core->heartbeat_counter)) {
        return ESP_ERR_INVALID_STATE;
    }
    lease_record_holder(core, machine_id, client_type);
    core->lease_connection_id = connection_id;
    core->heartbeat_connection_id = connection_id;
    core->heartbeat_counter = heartbeat->counter;
    core->last_heartbeat_ms = now_ms;
    core->link = ARGUS_MQTT_LINK_ONLINE;
    return ESP_OK;
}

bool argus_mqtt_session_tick(argus_mqtt_session_core_t *core, uint64_t now_ms)
{
    // Expiry is driven by the LEASE, not by the transport. Keying this on
    // link == ONLINE meant a lease could never expire once the socket
    // dropped, because disconnect had already moved the link to OFFLINE.
    // A lease exists whenever an owner is recorded; the deadline keeps
    // running whether or not the owner is currently connected, which is what
    // makes a genuinely dead supervisor expire on schedule instead of hiding
    // inside a reconnect loop.
    if (core == NULL || core->lease_machine_id[0] == '\0' ||
        now_ms - core->last_heartbeat_ms < ARGUS_MQTT_HEARTBEAT_TIMEOUT_MS) return false;
    core->link = ARGUS_MQTT_LINK_STALE;
    core->lease_connection_id = 0U;
    lease_record_holder(core, NULL, 0U);
    // Lease loss ends the authority epoch, so any command still in flight from
    // the expired owner is rejected rather than applied late. This does NOT
    // touch machine state, motion, RUN intent or the accepted setpoint - see
    // the fail-operational rule in the contract's section 1.
    advance_epoch(core);
    return true;
}

bool argus_mqtt_session_disconnect(argus_mqtt_session_core_t *core,
                                   uint64_t connection_id)
{
    if (core == NULL || connection_id == 0U ||
        (core->lease_connection_id != connection_id &&
         core->heartbeat_connection_id != connection_id)) return false;

    // A transport disconnect invalidates the CONNECTION BINDING only. It does
    // not destroy an otherwise unexpired lease and does not advance the
    // epoch.
    //
    // This previously cleared the owner and bumped the epoch, which meant a
    // momentary TCP drop handed authority away - the exact opposite of the
    // accepted decision that a comms event must not move authority. It also
    // silently invalidated the departing owner's in-flight commands. The
    // heartbeat deadline keeps running from last_heartbeat_ms, so if the
    // owner does not come back and renew in time, argus_mqtt_session_tick()
    // expires the lease and advances the epoch there instead - one place,
    // driven by the deadline rather than by socket noise.
    //
    // The same authenticated principal may rebind to a new connection while
    // the lease is unexpired, preserving owner and epoch. Commands from the
    // departed connection stop being accepted immediately because
    // lease_connection_id no longer matches it.
    core->lease_connection_id = 0U;
    core->heartbeat_connection_id = 0U;
    core->heartbeat_counter = 0U;
    core->link = ARGUS_MQTT_LINK_OFFLINE;
    return true;
}

argus_mqtt_sequence_result_t argus_mqtt_session_check_sequence(
    const argus_mqtt_session_core_t *core,
    const argus_mqtt_command_t *command,
    const char *payload)
{
    if (core == NULL || command == NULL || payload == NULL || !core->has_sequence)
        return ARGUS_MQTT_SEQUENCE_FIRST;
    if (command->sequence == core->last_sequence) {
        return command->action == core->cached_action &&
               strcmp(command->command_id, core->cached_command_id) == 0 &&
               strcmp(payload, core->cached_payload) == 0
                   ? ARGUS_MQTT_SEQUENCE_DUPLICATE
                   : ARGUS_MQTT_SEQUENCE_CONFLICT;
    }
    return argus_mqtt_session_is_newer(command->sequence, core->last_sequence)
               ? ARGUS_MQTT_SEQUENCE_NEWER : ARGUS_MQTT_SEQUENCE_STALE;
}

void argus_mqtt_session_commit_result(argus_mqtt_session_core_t *core,
                                      const argus_mqtt_command_t *command,
                                      const char *payload,
                                      const char *result_json)
{
    if (core == NULL || command == NULL || payload == NULL || result_json == NULL) return;
    core->has_sequence = true;
    core->last_sequence = command->sequence;
    core->cached_action = command->action;
    strlcpy(core->cached_command_id, command->command_id, sizeof(core->cached_command_id));
    strlcpy(core->cached_payload, payload, sizeof(core->cached_payload));
    strlcpy(core->cached_result, result_json, sizeof(core->cached_result));
}
