// #include "argus_can.h"

// #include "driver/twai.h"
//#include "esp_check.h"
//#include "esp_log.h"
//#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "argus_can";

static argus_can_config_t s_cfg;
static bool s_started;

static twai_timing_config_t argus_twai_timing(uint32_t bitrate)
{
    switch (bitrate) {
    case 250000:
        return (twai_timing_config_t)TWAI_TIMING_CONFIG_250KBITS();
    case 1000000:
        return (twai_timing_config_t)TWAI_TIMING_CONFIG_1MBITS();
    case 500000:
    default:
        return (twai_timing_config_t)TWAI_TIMING_CONFIG_500KBITS();
    }
}

static void argus_can_rx_task(void *arg)
{
    (void)arg;

    while (true) {
        twai_message_t msg = {0};
        if (twai_receive(&msg, pdMS_TO_TICKS(1000)) != ESP_OK) {
            continue;
        }

        if (msg.extd || msg.rtr) {
            continue;
        }

        if (msg.identifier == ARGUS_CAN_ID_SET_SPEED_CMD && msg.data_length_code >= 4U && s_cfg.on_set_speed != NULL) {
            argus_speed_cmd_t cmd = {
                .rpm_milli = (int32_t)((uint32_t)msg.data[0] |
                                       ((uint32_t)msg.data[1] << 8) |
                                       ((uint32_t)msg.data[2] << 16) |
                                       ((uint32_t)msg.data[3] << 24)),
            };
            s_cfg.on_set_speed(&cmd);
            continue;
        }

        if (msg.identifier == ARGUS_CAN_ID_START_CMD && s_cfg.on_start != NULL) {
            s_cfg.on_start();
            continue;
        }

        if (msg.identifier == ARGUS_CAN_ID_STOP_CMD && s_cfg.on_stop != NULL) {
            s_cfg.on_stop();
        }
    }
}

esp_err_t argus_can_init(const argus_can_config_t *config)
{
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG, "config is null");
    s_cfg = *config;

    twai_general_config_t general_config = TWAI_GENERAL_CONFIG_DEFAULT(config->tx_gpio, config->rx_gpio, TWAI_MODE_NORMAL);
    twai_timing_config_t timing_config = argus_twai_timing(config->bitrate);
    twai_filter_config_t filter_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    ESP_RETURN_ON_ERROR(twai_driver_install(&general_config, &timing_config, &filter_config), TAG, "twai install failed");
    ESP_RETURN_ON_ERROR(twai_start(), TAG, "twai start failed");
    s_started = true;

    BaseType_t created = xTaskCreate(argus_can_rx_task, "argus_can_rx", 4096, NULL, 8, NULL);
    ESP_RETURN_ON_FALSE(created == pdPASS, ESP_ERR_NO_MEM, TAG, "failed to create can rx task");

    ESP_LOGI(TAG, "twai started: tx=%d rx=%d bitrate=%lu", config->tx_gpio, config->rx_gpio, (unsigned long)config->bitrate);
    return ESP_OK;
}

esp_err_t argus_can_send_speed_status(const argus_speed_status_frame_t *status)
{
    ESP_RETURN_ON_FALSE(status != NULL, ESP_ERR_INVALID_ARG, TAG, "status is null");
    ESP_RETURN_ON_FALSE(s_started, ESP_ERR_INVALID_STATE, TAG, "twai not started");

    twai_message_t msg = {
        .identifier = ARGUS_CAN_ID_SPEED_STATUS,
        .extd = 0,
        .rtr = 0,
        .data_length_code = 8,
    };

    msg.data[0] = (uint8_t)(status->commanded_rpm_milli & 0xFFU);
    msg.data[1] = (uint8_t)((status->commanded_rpm_milli >> 8) & 0xFFU);
    msg.data[2] = (uint8_t)((status->commanded_rpm_milli >> 16) & 0xFFU);
    msg.data[3] = (uint8_t)((status->commanded_rpm_milli >> 24) & 0xFFU);
    msg.data[4] = (uint8_t)(status->applied_rpm_milli & 0xFFU);
    msg.data[5] = (uint8_t)((status->applied_rpm_milli >> 8) & 0xFFU);
    msg.data[6] = (uint8_t)((status->applied_rpm_milli >> 16) & 0xFFU);
    msg.data[7] = (uint8_t)((status->applied_rpm_milli >> 24) & 0xFFU);

    return twai_transmit(&msg, pdMS_TO_TICKS(20));
}

esp_err_t argus_can_send_state_status(const argus_state_status_frame_t *status)
{
    ESP_RETURN_ON_FALSE(status != NULL, ESP_ERR_INVALID_ARG, TAG, "status is null");
    ESP_RETURN_ON_FALSE(s_started, ESP_ERR_INVALID_STATE, TAG, "twai not started");

    twai_message_t msg = {
        .identifier = ARGUS_CAN_ID_STATE_STATUS,
        .extd = 0,
        .rtr = 0,
        .data_length_code = 8,
    };

    msg.data[0] = (uint8_t)(status->position_steps & 0xFFU);
    msg.data[1] = (uint8_t)((status->position_steps >> 8) & 0xFFU);
    msg.data[2] = (uint8_t)((status->position_steps >> 16) & 0xFFU);
    msg.data[3] = (uint8_t)((status->position_steps >> 24) & 0xFFU);
    msg.data[4] = status->state_flags;
    msg.data[5] = status->fault_code;
    msg.data[6] = 0U;
    msg.data[7] = 0U;

    return twai_transmit(&msg, pdMS_TO_TICKS(20));
}
