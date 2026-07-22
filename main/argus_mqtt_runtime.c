#include "argus_mqtt_runtime.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

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
#include "argus_net_mgr.h"
#include "argus_state_mgr.h"

static const char *TAG = "argus_mqtt_runtime";

#define ARGUS_MQTT_RUNTIME_QUEUE_DEPTH 12U
#define ARGUS_MQTT_RUNTIME_TASK_STACK 6144U

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
    xSemaphoreTake(s_runtime.mutex, portMAX_DELAY);
    esp_err_t err = argus_mqtt_session_accept_heartbeat(
        &s_runtime.session, message->connection_id, &heartbeat, now_ms());
    xSemaphoreGive(s_runtime.mutex);
    if (err == ESP_OK) {
        publish_value(s_runtime.topics.state_supervisor_link, "ONLINE", true);
    }
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
    if (strcmp(command.session, s_runtime.session.session) != 0) {
        xSemaphoreGive(s_runtime.mutex);
        reject_decoded(&command, "session_mismatch");
        return;
    }
    if (s_runtime.session.link != ARGUS_MQTT_LINK_ONLINE ||
        s_runtime.session.lease_connection_id != message->connection_id) {
        xSemaphoreGive(s_runtime.mutex);
        reject_decoded(&command, "supervisor_not_bound");
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

static void broker_client_cb(argus_mqtt_broker_client_event_t broker_event,
                             const argus_mqtt_broker_client_info_t *client,
                             void *user_ctx)
{
    (void)user_ctx;
    if (broker_event == ARGUS_MQTT_BROKER_CLIENT_DISCONNECTED) {
        xSemaphoreTake(s_runtime.mutex, portMAX_DELAY);
        bool changed = argus_mqtt_session_disconnect(
            &s_runtime.session, client->connection_id);
        xSemaphoreGive(s_runtime.mutex);
        if (changed) {
            publish_value(s_runtime.topics.state_supervisor_link, "OFFLINE", true);
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
        .policy_check = broker_policy_cb,
        .on_client_event = broker_client_cb,
        .user_ctx = NULL,
    };
}

esp_err_t argus_mqtt_runtime_broker_started(void)
{
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
    }
    publish_operational_snapshot();
}

esp_err_t argus_mqtt_runtime_get_topics(argus_mqtt_topics_t *out)
{
    if (out == NULL || !s_runtime.prepared) return ESP_ERR_INVALID_STATE;
    *out = s_runtime.topics;
    return ESP_OK;
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
