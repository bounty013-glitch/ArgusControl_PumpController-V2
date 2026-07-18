/**
 * @file argus_identity.c
 * @brief Persistent Device Identity and Metadata Module Implementation
 */

#include "argus_identity.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include "esp_app_desc.h"
#include "esp_mac.h"
#include "esp_system.h"

static argus_identity_t s_identity;
static bool s_initialized = false;

esp_err_t argus_identity_init(void)
{
    uint8_t mac[6] = {0};
    esp_err_t err = esp_efuse_mac_get_default(mac);
    if (err != ESP_OK) {
        // Fallback for native test environment if efuse read fails
        mac[0] = 0xA1; mac[1] = 0xB2; mac[2] = 0xC3;
        mac[3] = 0xD4; mac[4] = 0xE5; mac[5] = 0xF6;
    }

    snprintf(s_identity.mac_uid, sizeof(s_identity.mac_uid),
             "ESP32S3-%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    snprintf(s_identity.service_ssid, sizeof(s_identity.service_ssid),
             "Argus-Service-%02X%02X%02X",
             mac[3], mac[4], mac[5]);

    snprintf(s_identity.client_id, sizeof(s_identity.client_id), "default_client");
    snprintf(s_identity.unit_id, sizeof(s_identity.unit_id), "unit_01");
    snprintf(s_identity.device_name, sizeof(s_identity.device_name), "Argus Peristaltic Pump V2");
    snprintf(s_identity.device_model, sizeof(s_identity.device_model), "ARGUS-PUMP-V2");

    const esp_app_desc_t *app_desc = esp_app_get_description();
    if (app_desc && app_desc->version[0] != '\0') {
        snprintf(s_identity.fw_version, sizeof(s_identity.fw_version), "v2-phase4a-dev [%s]", app_desc->version);
    } else {
        snprintf(s_identity.fw_version, sizeof(s_identity.fw_version), "v2-phase4a-dev");
    }

    s_initialized = true;
    return ESP_OK;
}

esp_err_t argus_identity_get(argus_identity_t *out_id)
{
    if (!out_id) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        argus_identity_init();
    }
    memcpy(out_id, &s_identity, sizeof(argus_identity_t));
    return ESP_OK;
}

bool argus_identity_validate_client_id(const char *client_id)
{
    if (!client_id) return false;
    size_t len = strlen(client_id);
    if (len < 1 || len > ARGUS_IDENTITY_CLIENT_ID_MAX) return false;

    for (size_t i = 0; i < len; i++) {
        char c = client_id[i];
        if (!isalnum((unsigned char)c) && c != '-' && c != '_') {
            return false;
        }
    }
    return true;
}

bool argus_identity_validate_unit_id(const char *unit_id)
{
    if (!unit_id) return false;
    size_t len = strlen(unit_id);
    if (len < 1 || len > ARGUS_IDENTITY_UNIT_ID_MAX) return false;

    for (size_t i = 0; i < len; i++) {
        char c = unit_id[i];
        if (!isalnum((unsigned char)c) && c != '-' && c != '_') {
            return false;
        }
    }
    return true;
}

bool argus_identity_validate_device_name(const char *device_name)
{
    if (!device_name) return false;
    size_t len = strlen(device_name);
    if (len < 1 || len > ARGUS_IDENTITY_DEV_NAME_MAX) return false;

    for (size_t i = 0; i < len; i++) {
        char c = device_name[i];
        if (!isprint((unsigned char)c)) {
            return false;
        }
    }
    return true;
}
