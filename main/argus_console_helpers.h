#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the USB Serial/JTAG driver and linenoise console transport.
 * @return ESP_OK on successful initialization, or appropriate ESP error code.
 */
esp_err_t argus_console_transport_init(void);

/**
 * @brief Read a line of non-secret text using linenoise over the driver-backed VFS.
 * @param prompt Display prompt.
 * @param buffer Output buffer for the line.
 * @param buffer_size Capacity of buffer in bytes.
 * @return ESP_OK on success, ESP_ERR_INVALID_SIZE if overlength, ESP_FAIL on EOF/fail.
 */
esp_err_t argus_console_read_line(const char *prompt, char *buffer, size_t buffer_size);

/**
 * @brief Read a masked password (8-63 chars) using driver-backed POSIX STDIN/STDOUT.
 * @param prompt Display prompt.
 * @param buffer Output buffer for the password.
 * @param buffer_size Capacity of buffer in bytes (must be >= 65).
 * @return ESP_OK on success, ESP_ERR_INVALID_SIZE if invalid length, ESP_ERR_INVALID_STATE on cancel.
 */
esp_err_t argus_console_read_password(const char *prompt, char *buffer, size_t buffer_size);

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
