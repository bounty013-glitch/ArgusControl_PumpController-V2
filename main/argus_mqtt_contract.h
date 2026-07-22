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
    argus_mqtt_action_t action;
    int32_t target_rpm_milli;
    bool forward;
} argus_mqtt_command_t;

typedef struct {
    char session[ARGUS_MQTT_SESSION_HEX_LEN + 1U];
    uint32_t counter;
} argus_mqtt_heartbeat_t;

typedef struct {
    char session[ARGUS_MQTT_SESSION_HEX_LEN + 1U];
    argus_mqtt_link_state_t link;
    uint64_t lease_connection_id;
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
bool argus_mqtt_session_is_newer(uint32_t candidate, uint32_t reference);
esp_err_t argus_mqtt_session_accept_heartbeat(
    argus_mqtt_session_core_t *core, uint64_t connection_id,
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
