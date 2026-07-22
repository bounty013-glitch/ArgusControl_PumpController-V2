#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define ARGUS_MQTT_BROKER_CLIENT_ID_CAP 33U
#define ARGUS_MQTT_BROKER_TOPIC_CAP 160U
#define ARGUS_MQTT_BROKER_PAYLOAD_CAP 385U
#define ARGUS_MQTT_BROKER_RETAINED_CAPACITY 32U

typedef struct {
    uint64_t connection_id;
    char client_id[ARGUS_MQTT_BROKER_CLIENT_ID_CAP];
    char topic[ARGUS_MQTT_BROKER_TOPIC_CAP];
    char payload[ARGUS_MQTT_BROKER_PAYLOAD_CAP];
    size_t payload_len;
    uint8_t qos;
    bool retain;
    bool dup;
    bool policy_admitted;
} argus_mqtt_broker_message_t;

typedef enum {
    ARGUS_MQTT_BROKER_CLIENT_CONNECTED = 0,
    ARGUS_MQTT_BROKER_CLIENT_DISCONNECTED,
} argus_mqtt_broker_client_event_t;

typedef struct {
    uint64_t connection_id;
    char client_id[ARGUS_MQTT_BROKER_CLIENT_ID_CAP];
} argus_mqtt_broker_client_info_t;

typedef void (*argus_mqtt_broker_message_cb_t)(
    const argus_mqtt_broker_message_t *message, void *user_ctx);
typedef esp_err_t (*argus_mqtt_broker_policy_cb_t)(
    const argus_mqtt_broker_message_t *message, void *user_ctx);
typedef void (*argus_mqtt_broker_client_cb_t)(
    argus_mqtt_broker_client_event_t event,
    const argus_mqtt_broker_client_info_t *client,
    void *user_ctx);

typedef struct {
    uint16_t port;
    argus_mqtt_broker_message_cb_t on_message;
    argus_mqtt_broker_policy_cb_t policy_check;
    argus_mqtt_broker_client_cb_t on_client_event;
    void *user_ctx;
} argus_mqtt_broker_config_t;

esp_err_t argus_mqtt_broker_init(void);
esp_err_t argus_mqtt_broker_start(const argus_mqtt_broker_config_t *config);
esp_err_t argus_mqtt_broker_stop(void);
esp_err_t argus_mqtt_broker_publish(const char *topic, const char *payload, bool retain);
bool argus_mqtt_broker_is_running(void);

typedef enum {
    BROKER_STATE_STOPPED = 0,
    BROKER_STATE_STARTING,
    BROKER_STATE_RUNNING,
    BROKER_STATE_STOPPING
} argus_broker_state_t;

typedef struct {
    argus_broker_state_t state;
    int32_t active_client_count;
    bool has_server_task;
    bool has_listener;
    bool running;
    bool stopped;
} argus_mqtt_broker_lifecycle_obs_t;

typedef esp_err_t (*argus_mqtt_broker_observe_fn_t)(
    void *ctx, argus_mqtt_broker_lifecycle_obs_t *out);
typedef esp_err_t (*argus_mqtt_broker_stop_fn_t)(void *ctx);
typedef void (*argus_mqtt_broker_wait_fn_t)(void *ctx, uint32_t delay_ms);

typedef struct {
    argus_mqtt_broker_observe_fn_t observe;
    argus_mqtt_broker_stop_fn_t stop;
    argus_mqtt_broker_wait_fn_t wait;
    void *ctx;
} argus_mqtt_broker_convergence_ops_t;

bool argus_mqtt_broker_observation_is_stopped(
    const argus_mqtt_broker_lifecycle_obs_t *obs);
bool argus_mqtt_broker_observation_is_running(
    const argus_mqtt_broker_lifecycle_obs_t *obs);
esp_err_t argus_mqtt_broker_get_lifecycle_obs(argus_mqtt_broker_lifecycle_obs_t *out);
esp_err_t argus_mqtt_broker_request_stop_converged(
    const argus_mqtt_broker_convergence_ops_t *ops);
esp_err_t argus_mqtt_broker_verify_stopped_converged(
    const argus_mqtt_broker_convergence_ops_t *ops,
    uint32_t observation_attempts,
    uint32_t delay_ms);
