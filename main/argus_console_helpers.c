#include "argus_console_helpers.h"
#include <ctype.h>
#include <string.h>

char *argus_console_trim_whitespace(char *str)
{
    if (str == NULL) return NULL;

    while (isspace((unsigned char)*str)) {
        str++;
    }

    if (*str == '\0') {
        return str;
    }

    char *end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) {
        end--;
    }

    end[1] = '\0';
    return str;
}

esp_err_t argus_console_parse_menu_key(const char *line, char *out_key)
{
    if (line == NULL || out_key == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    while (isspace((unsigned char)*line)) {
        line++;
    }

    if (*line == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    char candidate = *line++;

    while (*line != '\0') {
        if (!isspace((unsigned char)*line)) {
            return ESP_ERR_INVALID_ARG;
        }
        line++;
    }

    *out_key = candidate;
    return ESP_OK;
}

esp_err_t argus_console_validate_ssid(const char *ssid)
{
    if (ssid == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t len = strlen(ssid);
    if (len < 1 || len > 32) {
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

esp_err_t argus_console_validate_password(const char *pass)
{
    if (pass == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t len = strlen(pass);
    if (len < 8 || len > 63) {
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}
