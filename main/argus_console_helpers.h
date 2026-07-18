#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Trim leading and trailing whitespace from a string in-place.
 * @param str Input/output null-terminated string.
 * @return Pointer to start of trimmed string inside str.
 */
char *argus_console_trim_whitespace(char *str);

/**
 * @brief Parse a single menu option character from a raw input line.
 * @param line Input line from console.
 * @param out_key Output pointer for the single menu character.
 * @return ESP_OK if line contains exactly one non-whitespace character,
 *         ESP_ERR_INVALID_ARG if line is empty, whitespace-only, or multi-character.
 */
esp_err_t argus_console_parse_menu_key(const char *line, char *out_key);

/**
 * @brief Validate an SSID string.
 * @param ssid Candidate SSID string.
 * @return ESP_OK if length is between 1 and 32 characters,
 *         ESP_ERR_INVALID_SIZE otherwise.
 */
esp_err_t argus_console_validate_ssid(const char *ssid);

/**
 * @brief Validate a WPA2 Password string.
 * @param pass Candidate WPA2 Password string.
 * @return ESP_OK if length is between 8 and 63 characters,
 *         ESP_ERR_INVALID_SIZE otherwise.
 */
esp_err_t argus_console_validate_password(const char *pass);

#ifdef __cplusplus
}
#endif
