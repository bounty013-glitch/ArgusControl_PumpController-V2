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

typedef struct {
    int state;           // argus_broker_state_t cast to int
    int32_t active_client_count;
    bool has_server_task;
    bool has_listener;
    bool running;
    bool stopped;
} argus_mqtt_broker_lifecycle_obs_t;

esp_err_t argus_mqtt_broker_get_lifecycle_obs(argus_mqtt_broker_lifecycle_obs_t *out);
