#include "argus_cmd_parser.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "esp_log.h"

static const char *TAG = "argus_cmd_parser";

esp_err_t argus_cmd_parser_speed_pct(const char *payload, int *out_pct)
{
    if (payload == NULL || out_pct == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // Skip leading whitespace
    const char *p = payload;
    while (*p && isspace((unsigned char)*p)) {
        p++;
    }

    if (*p == '\0') {
        return ESP_ERR_INVALID_ARG; // Empty or whitespace-only string
    }

    // Reject leading sign or non-digit (must start with digit)
    if (!isdigit((unsigned char)*p)) {
        return ESP_ERR_INVALID_ARG;
    }

    char *endptr = NULL;
    long val = strtol(p, &endptr, 10);

    // Skip trailing whitespace
    while (endptr && *endptr && isspace((unsigned char)*endptr)) {
        endptr++;
    }

    // If endptr is not at end of string, extra non-numeric characters exist (e.g. "50rpm")
    if (endptr == NULL || *endptr != '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    // Check bounds [0, 100]
    if (val < 0 || val > 100) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_pct = (int)val;
    return ESP_OK;
}

esp_err_t argus_cmd_parser_bool(const char *payload, bool *out_val)
{
    if (payload == NULL || out_val == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // Trim whitespace
    const char *p = payload;
    while (*p && isspace((unsigned char)*p)) {
        p++;
    }

    char buf[16];
    size_t i = 0;
    while (*p && !isspace((unsigned char)*p) && i < sizeof(buf) - 1) {
        buf[i++] = tolower((unsigned char)*p++);
    }
    buf[i] = '\0';

    // Verify remaining chars are whitespace
    while (*p && isspace((unsigned char)*p)) {
        p++;
    }
    if (*p != '\0' || i == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (strcmp(buf, "true") == 0 || strcmp(buf, "1") == 0 || strcmp(buf, "on") == 0) {
        *out_val = true;
        return ESP_OK;
    } else if (strcmp(buf, "false") == 0 || strcmp(buf, "0") == 0 || strcmp(buf, "off") == 0) {
        *out_val = false;
        return ESP_OK;
    }

    return ESP_ERR_INVALID_ARG;
}

esp_err_t argus_cmd_parser_validate_control_message(const char *topic, const char *payload, bool is_retained)
{
    (void)payload;
    if (is_retained && topic != NULL) {
        if (strstr(topic, "/cmd/") != NULL || strstr(topic, "/cmd") != NULL || strstr(topic, "/control/") != NULL) {
            ESP_LOGW(TAG, "Transport-neutral broker policy seam rejected retained control message: topic=%s", topic);
            return ESP_ERR_INVALID_STATE;
        }
    }
    return ESP_OK;
}
