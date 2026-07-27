#pragma once

#include <stddef.h>
#include "esp_err.h"
#include "argus_mqtt_broker.h"
#include "argus_mqtt_contract.h"

esp_err_t argus_mqtt_runtime_init(void);
esp_err_t argus_mqtt_runtime_prepare_start(void);
void argus_mqtt_runtime_get_broker_config(uint16_t port,
                                          argus_mqtt_broker_config_t *out);
esp_err_t argus_mqtt_runtime_broker_started(void);
// Anchors the bounded ARGUSCORE acquisition window to the moment
// acquisition is operationally possible, rather than to MCU uptime.
// Idempotent-safe: does not reset the window if an owner already holds
// a live lease, so a broker restart cannot hand control to someone else.
void argus_mqtt_runtime_mark_authority_ready(void);
void argus_mqtt_runtime_tick(void);
esp_err_t argus_mqtt_runtime_get_session(char *out, size_t out_size);
