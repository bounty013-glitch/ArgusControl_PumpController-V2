#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "argus_machine_service.h"

#define ARGUS_MQTT_BROKER_CLIENT_ID_CAP 33U
#define ARGUS_MQTT_BROKER_TOPIC_CAP 160U
#define ARGUS_MQTT_BROKER_PAYLOAD_CAP 385U
#define ARGUS_MQTT_BROKER_RETAINED_CAPACITY 32U
#define ARGUS_MQTT_BROKER_USERNAME_CAP (ARGUS_SECURITY_ID_MAX + 1U)
#define ARGUS_MQTT_BROKER_PASSWORD_CAP 129U

typedef struct {
    char client_id[ARGUS_MQTT_BROKER_CLIENT_ID_CAP];
    uint8_t username[ARGUS_MQTT_BROKER_USERNAME_CAP];
    size_t username_len;
    uint8_t password[ARGUS_MQTT_BROKER_PASSWORD_CAP];
    size_t password_len;
    uint16_t keep_alive_s;
} argus_mqtt_connect_request_t;

typedef enum {
    ARGUS_MQTT_CONNECT_PARSE_OK = 0,
    ARGUS_MQTT_CONNECT_PARSE_MALFORMED,
    ARGUS_MQTT_CONNECT_PARSE_PROTOCOL,
    ARGUS_MQTT_CONNECT_PARSE_FLAGS,
    ARGUS_MQTT_CONNECT_PARSE_CLIENT_ID,
    ARGUS_MQTT_CONNECT_PARSE_CREDENTIALS,
    ARGUS_MQTT_CONNECT_PARSE_TOO_LARGE,
} argus_mqtt_connect_parse_result_t;

argus_mqtt_connect_parse_result_t argus_mqtt_broker_parse_connect(
    const uint8_t *packet, size_t length,
    argus_mqtt_connect_request_t *out);

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
    uint8_t receiving_interface;
    argus_machine_principal_t principal;
} argus_mqtt_broker_message_t;

typedef enum {
    ARGUS_MQTT_BROKER_CLIENT_CONNECTED = 0,
    ARGUS_MQTT_BROKER_CLIENT_DISCONNECTED,
} argus_mqtt_broker_client_event_t;

typedef struct {
    uint64_t connection_id;
    char client_id[ARGUS_MQTT_BROKER_CLIENT_ID_CAP];
    uint8_t receiving_interface;
    argus_machine_principal_t principal;
} argus_mqtt_broker_client_info_t;

typedef void (*argus_mqtt_broker_message_cb_t)(
    const argus_mqtt_broker_message_t *message, void *user_ctx);
typedef esp_err_t (*argus_mqtt_broker_policy_cb_t)(
    const argus_mqtt_broker_message_t *message, void *user_ctx);
typedef void (*argus_mqtt_broker_client_cb_t)(
    argus_mqtt_broker_client_event_t event,
    const argus_mqtt_broker_client_info_t *client,
    void *user_ctx);
typedef argus_machine_auth_outcome_t (*argus_mqtt_broker_auth_cb_t)(
    uint32_t peer_key, const argus_mqtt_connect_request_t *request,
    uint8_t receiving_interface, void *user_ctx);
typedef esp_err_t (*argus_mqtt_broker_revalidate_cb_t)(
    const argus_mqtt_broker_client_info_t *client, void *user_ctx);
typedef esp_err_t (*argus_mqtt_broker_subscribe_policy_cb_t)(
    const argus_mqtt_broker_client_info_t *client,
    const char *filter, void *user_ctx);

typedef struct {
    uint16_t port;
    argus_mqtt_broker_message_cb_t on_message;
    argus_mqtt_broker_policy_cb_t publish_authorize;
    argus_mqtt_broker_policy_cb_t policy_check;
    argus_mqtt_broker_auth_cb_t authenticate;
    argus_mqtt_broker_revalidate_cb_t revalidate;
    argus_mqtt_broker_subscribe_policy_cb_t subscribe_policy;
    argus_mqtt_broker_client_cb_t on_client_event;
    void *user_ctx;
} argus_mqtt_broker_config_t;

esp_err_t argus_mqtt_broker_init(void);
esp_err_t argus_mqtt_broker_start(const argus_mqtt_broker_config_t *config);
esp_err_t argus_mqtt_broker_stop(void);
esp_err_t argus_mqtt_broker_publish(const char *topic, const char *payload, bool retain);
esp_err_t argus_mqtt_broker_disconnect_machine(const char *identifier);
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
