#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "argus_protocol.h"

typedef void (*argus_can_speed_callback_t)(const argus_speed_cmd_t *cmd);
typedef void (*argus_can_simple_callback_t)(void);

typedef struct {
    int tx_gpio;
    int rx_gpio;
    uint32_t bitrate;
    argus_can_speed_callback_t on_set_speed;
    argus_can_simple_callback_t on_start;
    argus_can_simple_callback_t on_stop;
} argus_can_config_t;

esp_err_t argus_can_init(const argus_can_config_t *config);
esp_err_t argus_can_send_speed_status(const argus_speed_status_frame_t *status);
esp_err_t argus_can_send_state_status(const argus_state_status_frame_t *status);
