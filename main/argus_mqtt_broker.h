#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef void (*argus_mqtt_broker_message_cb_t)(const char *topic, const char *payload, void *user_ctx);

typedef struct {
    uint16_t port;
    argus_mqtt_broker_message_cb_t on_message;
    void *user_ctx;
} argus_mqtt_broker_config_t;

esp_err_t argus_mqtt_broker_start(const argus_mqtt_broker_config_t *config);
esp_err_t argus_mqtt_broker_publish(const char *topic, const char *payload, bool retain);
