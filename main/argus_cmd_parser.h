#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/**
 * @brief Parse a speed percentage string payload strictly.
 *        Valid range is [0, 100].
 *        Rejects non-numeric characters, negative numbers, overflow, empty, or whitespace strings.
 * @param payload Raw string payload.
 * @param out_pct Pointer to store parsed percentage.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG on parsing failure.
 */
esp_err_t argus_cmd_parser_speed_pct(const char *payload, int *out_pct);

/**
 * @brief Parse a boolean string payload strictly.
 *        Valid true payloads: "true", "1", "on", "ON".
 *        Valid false payloads: "false", "0", "off", "OFF".
 *        Rejects partial matches or invalid strings.
 * @param payload Raw string payload.
 * @param out_val Pointer to store parsed boolean.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG on parsing failure.
 */
esp_err_t argus_cmd_parser_bool(const char *payload, bool *out_val);

/**
 * @brief Evaluate control message policy before storage or delivery.
 *        Rejects retained control messages.
 * @param topic Message topic string.
 * @param payload Message payload string.
 * @param is_retained true if message has retained flag set.
 * @return ESP_OK if message is permitted, ESP_ERR_INVALID_STATE if rejected.
 */
esp_err_t argus_cmd_parser_validate_control_message(const char *topic, const char *payload, bool is_retained);
