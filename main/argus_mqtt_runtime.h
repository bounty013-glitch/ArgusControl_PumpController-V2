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
void argus_mqtt_runtime_tick(void);
esp_err_t argus_mqtt_runtime_get_session(char *out, size_t out_size);
