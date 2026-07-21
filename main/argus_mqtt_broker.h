#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef void (*argus_mqtt_broker_message_cb_t)(const char *topic, const char *payload, bool retain, void *user_ctx);
typedef esp_err_t (*argus_mqtt_broker_policy_cb_t)(const char *topic, const char *payload, bool retain, void *user_ctx);

typedef struct {
    uint16_t port;
    argus_mqtt_broker_message_cb_t on_message;
    argus_mqtt_broker_policy_cb_t policy_check;
    void *user_ctx;
} argus_mqtt_broker_config_t;

/**
 * @brief One-time lifecycle object creation (mutex, event group).
 *
 * Must be called once before any start/stop cycle.  Safe to call from
 * app_main before the scheduler or networking is ready.  Creates
 * firmware-lifetime synchronisation objects that are never deleted.
 *
 * @return ESP_OK on success, ESP_ERR_NO_MEM if allocation fails,
 *         ESP_ERR_INVALID_STATE if already initialised.
 */
esp_err_t argus_mqtt_broker_init(void);

esp_err_t argus_mqtt_broker_start(const argus_mqtt_broker_config_t *config);
esp_err_t argus_mqtt_broker_stop(void);
esp_err_t argus_mqtt_broker_publish(const char *topic, const char *payload, bool retain);

/**
 * @brief Authoritative query for current MQTT broker running state.
 * @return true if broker server task and listening socket are active, false otherwise.
 */
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
